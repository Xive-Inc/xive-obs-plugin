#include "ui/xive-browser-dock.h"

#include "ui/xive-theme.h"
#include "xive-auth.h"
#include "xive-config.h"
#include "xive-log.h"

/*
 * ⚠️ THE ONLY TRANSLATION UNIT THAT MAY INCLUDE THIS HEADER.
 *
 * browser-panel.hpp is a verbatim copy of obs-browser's, and `obs_browser_init_panel()` inside it
 * is `static inline` — including it twice would give the plugin two independent copies of the CEF
 * handle. Everything else talks to this file through xive-browser-dock.h, which names no CEF type.
 */
#include "ui/browser-panel.hpp"

#include <obs-frontend-api.h>

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QPoint>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QDialog>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>

namespace {

/*
 * One CEF handle and one cookie store for the whole plugin.
 *
 * The store is shared on purpose: chat, activity, gifts and quick actions are four views of ONE
 * creator, so signing in on any of them has to sign in on all four. Separate stores would ask for
 * the same password four times and then drift apart as sessions expired at different moments.
 */
QCef *g_cef = nullptr;
QCefCookieManager *g_cookies = nullptr;
bool g_probed = false;
QString g_reason;

/*
 * Set by shutdown(). Everything that can write into a panel checks it.
 *
 * OBS keeps turning the event loop after OBS_FRONTEND_EVENT_EXIT — it has a main window to close
 * and a layout to save — so a queued timer or a stray urlChanged can still land after the browsers
 * are gone. Each of those would be a call into a closed CefBrowser.
 */
bool g_shuttingDown = false;

/* Held so shutdown() can stop it explicitly rather than relying on its parent outliving us. */
QPointer<QTimer> g_tokenTimer;

/*
 * Relative, NOT absolute. obs-browser resolves a relative storage path under the OBS config
 * directory, which is where its own docks keep theirs — an absolute path under the module config
 * dir would work too, but it would put a CEF cache somewhere no OBS tooling knows to clear.
 */
constexpr const char *kCookieStore = "xive_dock_cookies";

/*
 * Throws away the PAGE's own session and reloads.
 *
 * localStorage, not cookies: the web session lives in `xive-session` there
 * (packages/core/src/utils/sessionManager.ts), so DeleteCookies would clear nothing that matters.
 *
 * The reload lands on a page the plugin hands its current token straight back to, so this is not
 * "sign out" — it is "forget whoever you think you are and ask again".
 */
constexpr const char *kResetSessionScript =
	"try { localStorage.clear(); sessionStorage.clear(); } catch (e) {} location.reload();";

/*
 * Every panel currently alive, so one token refresh reaches all four.
 *
 * QPointer and not a raw list: OBS owns these widgets and destroys them with the main window, which
 * happens while this module is still loaded. A raw pointer would leave the refresh timer writing
 * JavaScript into freed memory on the way out.
 */
struct LivePanel {
	QPointer<QCefWidget> widget;
	/* Kept so a refreshed token can be written back into the STARTUP script too, not just into
	 * the page that is loaded right now — otherwise the next reload would go back to null. */
	QString kind;
	/* The last URL the panel reported, so a token is only ever pushed into our own site. */
	QString url;
};

QList<LivePanel> g_panels;

/**
 * `https://thexive.com` — the one origin a panel may hand the token to.
 *
 * ⚠️ THE TOKEN IS A FULL XIVE BEARER. A panel can leave our site — the identity provider's pages on
 * every sign-in, a same-window link, a redirect — and the startup script and the push both run on
 * whatever page is loaded. Without this, that page would read `window.__xiveObsDock.token`.
 */
QString siteOrigin()
{
	const QUrl base(XiveConfig::siteBase());
	QString origin = base.scheme() + QStringLiteral("://") + base.host();
	if (base.port() != -1)
		origin += QLatin1Char(':') + QString::number(base.port());
	return origin;
}

bool isOnSite(const QString &url)
{
	const QUrl seen(url);
	const QUrl base(XiveConfig::siteBase());
	return seen.scheme() == base.scheme() && seen.host() == base.host() &&
	       seen.port() == base.port();
}

/** Record a panel's new URL before anything pushes into it. */
void rememberUrl(QCefWidget *panel, const QString &url)
{
	for (LivePanel &p : g_panels) {
		if (p.widget == panel)
			p.url = url;
	}
}

/**
 * How often the plugin's current access token is pushed into the panels.
 *
 * Well under any plausible token lifetime, and the call is free when nothing has changed:
 * withAccessToken() hands back the cached token and only performs a refresh when it is near
 * expiry. The cost of being wrong in the other direction is a dock that quietly de-authenticates
 * mid-broadcast, which is exactly the failure this plugin is meant not to have.
 */
constexpr int kTokenPushMs = 10 * 60 * 1000;

/** Push the current token — whatever it is — into every panel still alive. */
void pushTokenToPanels();

/**
 * Throw away the web session in every panel and reload them.
 *
 * ⚠️ THIS IS WHAT MAKES SIGNING OUT OF THE PLUGIN MEAN ANYTHING TO THE DOCKS.
 *
 * A panel's session is its own — `xive-session` in the shared CEF localStorage — and clearing the
 * plugin's token does not touch it. Without this, signing out left four docks logged in as the
 * creator who just left, and signing back in as somebody else did not dislodge them: the page sees
 * itself as already authenticated and never spends the new token.
 *
 * Called on sign-out only. A sign-IN needs nothing here — a panel with no session takes the token
 * on its own, and one that already holds a session for a DIFFERENT creator notices the account it
 * is handed and drops it (see obsDockSessionHandoff.ts), which is the case a creator can reach
 * without the plugin ever emitting a sign-out.
 */
void resetPanelSessions();

/** A string as a JavaScript literal, escaped by the JSON writer rather than by hand. */
QString jsLiteral(const QString &value)
{
	if (value.isEmpty())
		return QStringLiteral("null");

	/* QJsonDocument will only serialise a container, so the value rides in an array and the
	 * brackets come off — this is the standard way to borrow its escaping for one scalar. */
	const QByteArray wrapped =
		QJsonDocument(QJsonArray{QJsonValue(value)}).toJson(QJsonDocument::Compact);
	return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}

/**
 * Runs before the page's own scripts, on every load.
 *
 * ⚠️ THE TOKEN IS WHY THE CREATOR DOES NOT SIGN IN TWICE.
 *
 * The plugin has its OWN OAuth client and its own Authentik application (see
 * XiveConfig::clientId()), but its access token is still an ordinary Xive bearer — the API accepts
 * that application's `iss` and `aud` alongside the web/mobile one, so `/api/profiles/me` answers it
 * exactly as it answers the website's own. The page turns it into a session with its own
 * `login()`; see src/features/auth/obsDockSessionHandoff.ts for why the session is built there and
 * not here.
 *
 * Empty when there is no session yet. That is not a failure — the docks are built at module load,
 * which is often before the stored refresh token has been exchanged, and `pushToken()` below fills
 * it in the moment it arrives.
 */
QString startupScript(const QString &kind, const QString &token, const QString &account,
		      bool signedIn)
{
	/* Guarded in the page as well as by isOnSite(): the startup script runs on EVERY load, and
	 * the plugin cannot see a navigation before it happens. See siteOrigin(). */
	return QStringLiteral("if (location.origin === %6) {"
			      " window.__xiveObsDock = { version: %1, kind: %2, token: %3, "
			      "account: %4, signedIn: %5 }; }")
		.arg(jsLiteral(QString::fromUtf8(XIVE_PLUGIN_VERSION)), jsLiteral(kind),
		     jsLiteral(token), jsLiteral(account),
		     signedIn ? QStringLiteral("true") : QStringLiteral("false"),
		     jsLiteral(siteOrigin()));
}

/**
 * Hand a token to a page that has ALREADY loaded.
 *
 * The startup script only runs on a load, and two things happen after one: the plugin's first token
 * exchange returns, and — every hour or so, for as long as OBS is open — that token is replaced.
 * Reloading the panel for either would blank a creator's chat mid-broadcast, so the value is
 * replaced in place and the page is told, which is what OBS_DOCK_TOKEN_EVENT is for.
 */
QString pushTokenScript(const QString &token, const QString &account, bool signedIn)
{
	return QStringLiteral(
		       "(function () {"
		       "  if (location.origin !== %4) return;"
		       "  window.__xiveObsDock = window.__xiveObsDock || {};"
		       "  window.__xiveObsDock.token = %1;"
		       "  window.__xiveObsDock.account = %2;"
		       "  window.__xiveObsDock.signedIn = %3;"
		       "  window.dispatchEvent(new Event(\"xive-obs-dock-token\"));"
		       "})();")
		.arg(jsLiteral(token), jsLiteral(account),
		     signedIn ? QStringLiteral("true") : QStringLiteral("false"),
		     jsLiteral(siteOrigin()));
}

void pushTokenToPanels()
{
	if (g_shuttingDown)
		return;

	/* Panels die with the OBS main window while this module is still loaded, so the list is
	 * swept before it is used rather than trusted. Erase-remove and not QList::removeIf, which
	 * only arrived in Qt 6.1. */
	g_panels.erase(std::remove_if(g_panels.begin(), g_panels.end(),
				      [](const LivePanel &p) { return p.widget.isNull(); }),
		       g_panels.end());
	if (g_panels.isEmpty())
		return;

	XiveAuth *auth = XiveAuth::instance();
	if (!auth)
		return;

	/*
	 * ⚠️ WHO, NOT JUST WHAT. The token alone cannot tell a page that the creator behind it has
	 * changed — a panel already holding a web session simply keeps it, which is how signing out
	 * and back in as a different account left four docks on the previous one.
	 *
	 * `signedIn` is pushed as its own flag rather than inferred from an empty token, because an
	 * empty token means two different things: signed out, and a refresh that failed. Only the
	 * first is a reason for a dock to drop its session; treating the second that way would blank
	 * a creator's chat over a momentary network fault mid-broadcast.
	 *
	 * `account` is empty until userinfo answers (and stays empty if it failed), so the page
	 * treats "no account" as "do not know" rather than as a mismatch. See
	 * src/features/auth/obsDockSessionHandoff.ts.
	 */
	const bool signedIn = auth->isSignedIn();
	const QString account = signedIn ? auth->accountEmail() : QString();

	auto apply = [signedIn, account](const QString &token) {
		const std::string push = pushTokenScript(token, account, signedIn).toStdString();
		for (const LivePanel &p : g_panels) {
			if (!p.widget)
				continue;
			/* Both, deliberately: the running page gets it now, and the next load —
			 * a reload, or the walk-back after a sign-in redirect — gets it too. */
			p.widget->setStartupScript(
				startupScript(p.kind, token, account, signedIn).toStdString());
			/* Off our site, the page gets nothing — see siteOrigin(). */
			if (isOnSite(p.url))
				p.widget->executeJavaScript(push);
		}
	};

	/*
	 * Signed out is a value to push, not a reason to skip: a creator who signs out of the plugin
	 * should not be left with four docks still holding their token.
	 */
	if (!signedIn) {
		apply(QString());
		return;
	}

	/* Hands back the cached token unless it is near expiry, so this is nearly always free. An
	 * empty string means the refresh failed — which is exactly what the panels should be told. */
	auth->withAccessToken([apply](const QString &token) { apply(token); });
}

void resetPanelSessions()
{
	if (g_shuttingDown)
		return;

	int reset = 0;
	for (const LivePanel &p : g_panels) {
		if (!p.widget)
			continue;
		p.widget->executeJavaScript(kResetSessionScript);
		++reset;
	}

	if (reset > 0)
		xive_info("cleared the web session in %d dock panel(s)", reset);
}

/**
 * Keep a dock on its own panel, whatever the page navigates to.
 *
 * ⚠️ THIS IS WHAT MAKES SIGN-IN WORK IN A DOCK, not a tidiness measure.
 *
 * Signing in is a full-page redirect (MSAL `loginRedirect`, and entraMsal.ts sets
 * `navigateToLoginRequestUrl: false`), so the round trip is:
 *
 *   thexive.com/popout/chat → login.microsoftonline.com → thexive.com/auth/callback → the app's
 *   default view
 *
 * — and the creator's chat dock is now showing the whole website. In a browser that is a back
 * button; in a dock there is no chrome to press. So: when the panel settles on our own origin
 * somewhere that is not a popout and not the auth callback, send it home.
 *
 * The identity provider's own pages are a DIFFERENT origin and are left alone, or the redirect
 * would be yanked out from under the creator mid-login. `/auth/` is skipped for the same reason on
 * our side — the callback route is still finishing the token exchange when it renders.
 *
 * Deferred by a beat rather than snapped instantly: `navigateAfterMsalCallback()` fires once the
 * session is written, but the writes behind it are async, and reloading on top of a half-persisted
 * session would sign the creator straight back out.
 */
void keepOnPanel(QCefWidget *panel, const QString &home)
{
	const QString host = QUrl(home).host();

	QObject::connect(panel, &QCefWidget::urlChanged, panel,
			 [panel, home, host](const QString &url) {
				 const QUrl seen(url);
				 if (seen.host() != host)
					 return;

				 const QString path = seen.path();
				 if (path.startsWith(QStringLiteral("/popout/")) ||
				     path.startsWith(QStringLiteral("/auth/")))
					 return;

				 if (g_shuttingDown)
					 return;

				 xive_info("dock wandered to %s; returning to %s",
					   qUtf8Printable(url), qUtf8Printable(home));
				 QTimer::singleShot(1200, panel, [panel, home]() {
					 if (g_shuttingDown)
						 return;
					 panel->setURL(home.toStdString());
				 });
			 });
}

/*
 * ── THE PAGE→PLUGIN CHANNEL ──────────────────────────────────────────────────────────────────
 *
 * A dock's page has no bridge to us: `window.obsstudio` belongs to browser SOURCES, not panels.
 * What obs-browser's panel client does raise is `titleChanged`, from CEF's OnTitleChange — so the
 * page writes a message into `document.title` and we read it here. `document.title` is invisible
 * in a dock (OBS titles the dock itself), so nothing is lost by using it as a wire.
 *
 * The two halves of this protocol live in src/features/obs/obsDockBridge.ts. Change them together.
 *
 *   xive-obs-action:<name>:<w>x<h>:<nonce>   open this quick action in its own window
 *   xive-obs-action-close:<nonce>            close the window this page is in
 *
 * The nonce is not decoration: we are notified of a title by CHANGE, so clicking the same action
 * twice would otherwise write an identical string and the second click would be swallowed.
 */
constexpr const char *kActionClosePrefix = "xive-obs-action-close:";

/** Opens `/popout/action?do=<action>` as a window this plugin owns. Defined below. */
void openActionWindow(const QString &action, int width, int height);

/**
 * Read an open request. False when `title` is an ordinary page title.
 *
 * Deliberately strict — a title that merely starts with the prefix but carries no well-formed size
 * is ignored rather than opened at some guessed dimension.
 */
bool parseActionRequest(const QString &title, QString *action, int *width, int *height)
{
	static const QRegularExpression re(
		QStringLiteral("^xive-obs-action:([a-z]+):([0-9]{2,4})x([0-9]{2,4}):[0-9]+$"));

	const QRegularExpressionMatch m = re.match(title);
	if (!m.hasMatch())
		return false;

	*action = m.captured(1);
	*width = m.captured(2).toInt();
	*height = m.captured(3).toInt();
	return true;
}

/**
 * Right-click menu for a panel.
 *
 * Reload and zoom are here because a dock has no browser chrome to reach them from, and a wedged
 * panel with no way to reload it is a restart of OBS. "Sign out of Xive docks" clears the shared
 * store — worth stating in the plural, since it signs out all four.
 */
void installContextMenu(QWidget *container, QCefWidget *panel)
{
	container->setContextMenuPolicy(Qt::CustomContextMenu);

	/*
	 * QAction built by hand rather than QMenu::addAction(text, context, functor): that overload
	 * only arrived in Qt 6.3, and the plugin is built against whatever Qt the host OBS ships.
	 */
	auto item = [](QMenu *menu, const QString &text, std::function<void()> run) {
		auto *action = menu->addAction(text);
		QObject::connect(action, &QAction::triggered, menu, [run]() { run(); });
	};

	QObject::connect(container, &QWidget::customContextMenuRequested, panel,
			 [container, panel, item](const QPoint &pos) {
				 QMenu menu(container);
				 menu.setStyleSheet(XiveTheme::menuStyleSheet());

				 item(&menu, QObject::tr("Reload"),
				      [panel]() { panel->reloadPage(); });
				 menu.addSeparator();
				 item(&menu, QObject::tr("Zoom in"),
				      [panel]() { panel->zoomPage(1); });
				 item(&menu, QObject::tr("Zoom out"),
				      [panel]() { panel->zoomPage(-1); });
				 item(&menu, QObject::tr("Reset zoom"),
				      [panel]() { panel->zoomPage(0); });
				 menu.addSeparator();
				 /*
				  * "Reset", not "Sign out", because that is what it does: the reload
				  * lands on a page the plugin hands its token straight back to. It is
				  * the fix for a panel wedged in a bad session, not a way to sign out
				  * — signing out is done in the Xive dock, and the panels follow it
				  * on their own now (see resetPanelSessions()).
				  *
				  * One panel, not all of them: a creator reaching for this has one
				  * dock misbehaving, and there is no reason to reload the other three
				  * out from under a broadcast. The sign-out path is the one that
				  * means all four.
				  */
				 item(&menu, QObject::tr("Reset dock session"), [panel]() {
					 panel->executeJavaScript(kResetSessionScript);
				 });

				 menu.exec(container->mapToGlobal(pos));
			 });
}

/*
 * One window per action, so a second click on "Manage goals" raises the editor already open rather
 * than stacking a second copy of it over the first.
 */
QHash<QString, QPointer<QDialog>> g_actionWindows;

void openActionWindow(const QString &action, int width, int height)
{
	if (g_shuttingDown || !g_cef)
		return;

	if (QDialog *existing = g_actionWindows.value(action)) {
		existing->raise();
		existing->activateWindow();
		return;
	}

	/*
	 * Parented to OBS's main window, which is what makes this feel like part of OBS rather than
	 * a stray window: it floats above the app, follows it, and closes with it.
	 */
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());

	auto *dialog = new QDialog(main);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setObjectName(QStringLiteral("xiveDock"));
	dialog->setAttribute(Qt::WA_StyledBackground, true);
	dialog->setStyleSheet(QStringLiteral("QWidget#xiveDock { background: %1; }")
				      .arg(QString::fromUtf8(XiveTheme::kBg)));
	/* Replaced by the page's own document.title as soon as it loads — see below. */
	dialog->setWindowTitle(QObject::tr("Xive"));
	dialog->resize(width, height);

	const QString url = XiveConfig::siteBase() + QStringLiteral("/popout/action?do=") + action;

	QCefWidget *panel = g_cef->create_widget(dialog, url.toStdString(), g_cookies);
	if (!panel) {
		xive_error("obs-browser refused an action window for %s", qUtf8Printable(action));
		delete dialog;
		return;
	}

	panel->setStartupScript(
		startupScript(QStringLiteral("action"), QString(), QString(), false).toStdString());
	panel->allowAllPopups(false);

	auto *layout = new QVBoxLayout(dialog);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(panel);

	/*
	 * The same title channel, in the other direction. An action page writes its own heading into
	 * document.title ("Raid channel · Xive"), which is exactly what this window should be called,
	 * and writes the close sentinel when its Cancel or Done button is pressed — because
	 * obs-browser has stubbed out the window.close() it would otherwise use.
	 */
	QObject::connect(panel, &QCefWidget::titleChanged, dialog,
			 [dialog](const QString &title) {
				 if (title.startsWith(QString::fromUtf8(kActionClosePrefix))) {
					 dialog->close();
					 return;
				 }
				 dialog->setWindowTitle(title);
			 });

	/*
	 * ⚠️ CLOSED EXPLICITLY, for the same reason the docks are — see XiveBrowserDock::shutdown().
	 * WA_DeleteOnClose would destroy the widget and obs-browser would close the browser in its
	 * destructor, but only if that happens before CEF goes away. Doing it on `finished` means the
	 * browser is gone the moment the creator dismisses the window, whenever that is.
	 */
	QObject::connect(dialog, &QDialog::finished, panel, [panel]() { panel->closeBrowser(); });

	QObject::connect(dialog, &QObject::destroyed, [action]() { g_actionWindows.remove(action); });

	/* Registered so a token refresh reaches it, and so shutdown() closes it with the rest. */
	g_panels.append(LivePanel{QPointer<QCefWidget>(panel), QStringLiteral("action"), url});

	/* Tracked so a push never lands on a page this window has navigated off our site to. */
	QObject::connect(panel, &QCefWidget::urlChanged, panel,
			 [panel](const QString &seen) { rememberUrl(panel, seen); });
	g_actionWindows.insert(action, dialog);

	dialog->show();
	pushTokenToPanels();

	xive_info("action window opened: %s (%dx%d)", qUtf8Printable(action), width, height);
}

} // namespace

namespace XiveBrowserDock {

bool available()
{
	if (g_probed)
		return g_cef != nullptr;

	g_probed = true;

	g_cef = obs_browser_init_panel();
	if (!g_cef) {
		/*
		 * Two causes, and the plugin cannot tell them apart from here: obs-browser is not
		 * loaded (a stripped or self-built OBS), or the session is Wayland, where
		 * browser-panel.hpp refuses outright. Both are stated, because both are true fixes.
		 */
		g_reason = QObject::tr(
			"OBS's browser panels are unavailable — either this OBS was built without "
			"the browser source, or this is a Wayland session, where OBS does not "
			"support browser docks.");
		xive_warn("obs-browser panels unavailable; falling back to the native docks");
		return false;
	}

	/*
	 * init_browser() answers true when CEF is already up. Otherwise it starts the process and
	 * wait_for_browser_init() blocks until it is ready — on the UI thread, which is what OBS
	 * itself does before it opens a browser dock. It is a one-off at module load, not per dock.
	 */
	if (!g_cef->init_browser())
		g_cef->wait_for_browser_init();

	g_cookies = g_cef->create_cookie_manager(kCookieStore, /*persist_session_cookies=*/true);
	if (!g_cookies) {
		/*
		 * Not fatal. Without a manager the panels fall back to CEF's global store, which
		 * still holds a session for as long as OBS is running — the creator just signs in
		 * again after a restart. A dock that works today beats no dock at all.
		 */
		xive_warn("could not create the Xive dock cookie store; sessions will not persist "
			  "across restarts");
	}

	/*
	 * ── KEEPING THE DOCKS SIGNED IN ──────────────────────────────────────────────────────
	 *
	 * Two triggers, because they cover different failures. The signal catches a creator signing
	 * in or out of the plugin while OBS is open — the panels have to follow, in both directions.
	 * The timer catches the silent one: an access token expiring part-way through a broadcast,
	 * where nothing is emitted and nothing looks wrong until chat stops loading.
	 *
	 * The timer is parented to the auth object so it dies with the singleton rather than
	 * outliving it and firing into a destroyed instance.
	 */
	if (XiveAuth *auth = XiveAuth::instance()) {
		QObject::connect(auth, &XiveAuth::signedInChanged, auth, [](bool signedIn) {
			/*
			 * Push FIRST, then reset. The push rewrites each panel's startup script, so
			 * the reload the reset triggers lands on a page that is already told the
			 * plugin is signed out — rather than one that briefly reads the token the
			 * creator just signed away from.
			 */
			pushTokenToPanels();
			if (!signedIn)
				resetPanelSessions();
		});

		/*
		 * ⚠️ AND ON EVERY NEW TOKEN. The timer alone left up to ten minutes between the plugin
		 * rotating its token and a panel hearing about it — and a panel's web session was built
		 * from the OLD token, so it could expire inside that gap and 401 for good. Kept as well as
		 * the timer, which still covers a page that reloaded and lost the pushed value.
		 */
		QObject::connect(auth, &XiveAuth::accessTokenChanged, auth,
				 []() { pushTokenToPanels(); });

		auto *refresh = new QTimer(auth);
		refresh->setInterval(kTokenPushMs);
		QObject::connect(refresh, &QTimer::timeout, auth, []() { pushTokenToPanels(); });
		refresh->start();
		g_tokenTimer = refresh;
	}

	xive_info("obs-browser panels available (qcef version %d)", obs_browser_qcef_version());
	return true;
}

QString unavailableReason()
{
	return g_reason;
}

void shutdown()
{
	if (g_shuttingDown)
		return;

	/* Set FIRST. closeBrowser() spins the event loop, and anything that runs in there must find
	 * the flag already up rather than reach for a browser that is halfway closed. */
	g_shuttingDown = true;

	if (g_tokenTimer)
		g_tokenTimer->stop();

	/*
	 * Taken and cleared BEFORE closing anything. closeBrowser() spins the event loop, so iterating
	 * the live list across it would be iterating a container that something in there could still
	 * touch. Nothing should — create() does not run at exit and every other writer checks the flag
	 * above — but the copy costs four pointers and removes the question.
	 */
	/*
	 * Action windows first. Each is a QDialog holding a panel that is ALSO in g_panels, so
	 * closing the dialog runs its `finished` handler and closes that browser; the sweep below
	 * then finds it already closed, which closeBrowser() tolerates. Doing it in this order means
	 * the creator does not watch four dialogs linger on top of a closing OBS.
	 */
	const QList<QPointer<QDialog>> windows = g_actionWindows.values();
	g_actionWindows.clear();
	for (const QPointer<QDialog> &w : windows) {
		if (w)
			w->close();
	}

	const QList<LivePanel> panels = g_panels;
	g_panels.clear();

	int closed = 0;
	for (const LivePanel &p : panels) {
		if (!p.widget)
			continue;
		p.widget->closeBrowser();
		++closed;
	}

	/*
	 * Flush before the store goes, or the sign-in the creator just did is lost on the way out and
	 * the docks ask for a password again next launch. This is the whole point of the persistent
	 * cookie manager.
	 *
	 * Deleting it as well as flushing mirrors OBS's own DestroyPanelCookieManager(): the manager
	 * owns a CefRequestContext, and CEF wants that released before it shuts down for the same
	 * reason it wants the browsers closed.
	 */
	if (g_cookies) {
		g_cookies->FlushStore();
		delete g_cookies;
		g_cookies = nullptr;
	}

	xive_info("closed %d browser dock(s) before CEF shutdown", closed);
}

QWidget *create(const QString &kind)
{
	if (!available())
		return nullptr;

	const QString url = XiveConfig::siteBase() + QStringLiteral("/popout/") + kind;

	QCefWidget *panel = g_cef->create_widget(nullptr, url.toStdString(), g_cookies);
	if (!panel) {
		xive_error("obs-browser refused a panel for %s", qUtf8Printable(url));
		return nullptr;
	}

	/* Null token for now; pushTokenToPanels() below rewrites this the moment one is in hand. */
	panel->setStartupScript(startupScript(kind, QString(), QString(), false).toStdString());

	/*
	 * ⚠️ POPUPS OFF — INCLUDING FOR QUICK ACTIONS.
	 *
	 * The panels open no windows of their own, so anything that tried would be a link the creator
	 * did not mean to follow, and in a dock parked over a live scene there is no address bar to
	 * notice it with. Blocked, obs-browser hands such a URL to the system browser instead (see
	 * OnBeforePopup in its panel client), which is where a stray link belongs.
	 *
	 * The quick-action windows are NOT popups — this plugin opens them, as dialogs it owns. Two
	 * reasons it has to, both from obs-browser's own source: its client stubs `window.close` on
	 * every panel frame, so a popup's Cancel button could never close it; and a popup's browser
	 * has no QCefWidget behind it, so shutdown() could not close it and CEF would abort OBS on
	 * exit. See openActionWindow() below.
	 */
	panel->allowAllPopups(false);

	/*
	 * A container rather than handing the panel straight to OBS. Two reasons: the panel is a
	 * native child window on Windows and painting a background behind it stops the white flash
	 * on load, and the right-click menu needs a widget that is not the browser to hang off —
	 * CEF eats the events on the panel itself.
	 */
	auto *container = new QWidget();
	container->setObjectName(QStringLiteral("xiveDock"));
	container->setAttribute(Qt::WA_StyledBackground, true);
	container->setStyleSheet(
		QStringLiteral("QWidget#xiveDock { background: %1; }").arg(QString::fromUtf8(XiveTheme::kBg)));

	auto *layout = new QVBoxLayout(container);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(panel);

	g_panels.append(LivePanel{QPointer<QCefWidget>(panel), kind, url});

	/*
	 * The quick-actions grid is the only panel that asks us for anything. Clicking one of its
	 * buttons writes a request into document.title, which arrives here — see the protocol note
	 * further up, and src/features/obs/obsDockBridge.ts for the other half.
	 */
	if (kind == QStringLiteral("actions")) {
		QObject::connect(panel, &QCefWidget::titleChanged, panel,
				 [](const QString &title) {
					 QString action;
					 int width = 0;
					 int height = 0;
					 if (parseActionRequest(title, &action, &width, &height))
						 openActionWindow(action, width, height);
				 });
	}

	/*
	 * On every load, not just the first. A panel that has just navigated has a fresh `window`
	 * and only the startup script on it — which was written with whatever token was current when
	 * the script was last set, and may since have been replaced. Pushing here closes that gap and
	 * covers the startup race too: the docks are built before the plugin's first token exchange
	 * returns, so the first load's script legitimately carries null.
	 */
	QObject::connect(panel, &QCefWidget::urlChanged, panel, [panel](const QString &seen) {
		/* Remembered FIRST: the push below decides from it whether this page may have the
		 * token at all. */
		rememberUrl(panel, seen);
		pushTokenToPanels();
	});

	keepOnPanel(panel, url);
	installContextMenu(container, panel);

	/* And once now, for the token that may already be in hand. */
	pushTokenToPanels();

	xive_info("browser dock ready: %s", qUtf8Printable(url));
	return container;
}

} // namespace XiveBrowserDock
