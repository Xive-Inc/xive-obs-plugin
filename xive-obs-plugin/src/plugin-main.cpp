#include "xive-api.h"
#include "xive-auth.h"
#include "xive-channel.h"
#include "xive-activity-feed.h"
#include "xive-chat-feed.h"
#include "xive-surge-feed.h"
#include "xive-realtime.h"
#include "xive-config.h"
#include "xive-service-catalog.h"
#include "xive-update.h"
#include "xive-log.h"

#include "ui/xive-activity-dock.h"
#include "ui/xive-alerts-dock.h"
#include "ui/xive-browser-dock.h"
#include "ui/xive-chat-dock.h"
#include "ui/xive-dock.h"
#include "ui/xive-manager-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QByteArray>
#include <QDockWidget>
#include <QMainWindow>
#include <QStringList>
#include <QTimer>

#ifdef _WIN32
#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("xive-obs-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Sign in to Xive, configure your stream key, and read chat from inside OBS.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Xive for OBS";
}

namespace {

bool g_docksAdded = false;

#ifdef _WIN32
/*
 * Qt loads its TLS backend as a plugin from a "tls" subdirectory of one of its library paths, and
 * OBS's Windows Qt runtime ships no such directory — QSslSocket then reports "No functional TLS
 * backend was found" and every HTTPS request fails with HTTP 0 before a byte is sent.
 *
 * The Windows install rule puts qschannelbackend.dll in bin/64bit/tls/, beside this module, so
 * adding the module's own directory as a Qt library path is what makes it discoverable. Done in
 * obs_module_load rather than buildUi because it must be in place before anything constructs a
 * QNetworkAccessManager, and it touches no frontend state.
 */
void registerTlsBackendPath()
{
	const char *binaryPath = obs_get_module_binary_path(obs_current_module());
	if (!binaryPath) {
		xive_warn("no module binary path; TLS backend not registered, sign-in will fail");
		return;
	}

	const QString dir = QFileInfo(QString::fromUtf8(binaryPath)).absolutePath();
	QCoreApplication::addLibraryPath(dir);
	xive_info("TLS backend search path registered: %s", qUtf8Printable(dir));
}
#endif

/*
 * The dock ids, in registration order. Iterated by both the restore and the shutdown log.
 *
 * `const`, not `constexpr`: each kDockId is a `const char *` defined in its own translation unit,
 * so it is initialised at runtime and is not a constant expression. MSVC rejects the constexpr
 * form outright (C2131) rather than quietly accepting it.
 */
/*
 * The quick-actions dock has no widget class to hang its id off — it is a browser panel and
 * nothing else — so its id lives here beside the others rather than in a header of its own.
 */
const char *const kActionsDockId = "xive_actions_dock";

/*
 * Multi-platform chat (2026-09-28): `/popout/multichat` — Xive, Twitch and Kick chat in one list,
 * with a "Send to" picker for replying on each. Browser panel only, like quick actions: the
 * Twitch/Kick half is web code (public/js/studio-multichat.js) with no Qt ancestor to fall back to.
 */
const char *const kMultichatDockId = "xive_multichat_dock";

const char *const kDockIds[] = {XiveDock::kDockId,          XiveChatDock::kDockId,
				XiveManagerDock::kDockId,   XiveActivityDock::kDockId,
				XiveAlertsDock::kDockId,    kActionsDockId,
				kMultichatDockId};

/*
 * What state is each dock actually in, right now.
 *
 * The mirror of the restore logging, and the half that was missing. Startup told us the saved
 * state says hidden+floating; it could not tell us WHY, because that is decided at shutdown, when
 * OBS writes QMainWindow::saveState(). Logging the same three facts as OBS is closing says whether
 * the docks were still where the creator put them when we were told the app was exiting.
 *
 * If they read visible+docked here and the next launch restores hidden+floating, then something
 * between this point and OBS's write is doing it, and it is not us. If they already read
 * hidden+floating here, whatever does it runs earlier still.
 *
 * hidden() is reported alongside visible(), and it is the one that matters: QDockAreaLayoutInfo
 * serialises the flag from isHidden(), not isVisible(). The two disagree whenever an ancestor is
 * hidden — which is exactly what happens while a main window is closing — so reading isVisible()
 * alone here would report "hidden" for a dock Qt is about to save as visible, and send us after a
 * bug that is not there.
 *
 * Tab partners are listed because an inactive tab in a stack is a second way to be legitimately
 * not-visible while still docked, and it would otherwise look identical to the failure.
 */
void logDockStates(const char *when)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main) {
		xive_warn("%s: no main window; cannot report dock state", when);
		return;
	}

	for (const char *id : kDockIds) {
		auto *dock = main->findChild<QDockWidget *>(QString::fromUtf8(id));
		if (!dock) {
			xive_warn("%s: '%s' no longer exists", when, id);
			continue;
		}
		QStringList partners;
		for (const QDockWidget *t : main->tabifiedDockWidgets(dock))
			partners << t->objectName();

		xive_info("%s: '%s' hidden=%d visible=%d floating=%d area=%d geom=%dx%d+%d+%d tabbed=[%s]",
			  when, id, dock->isHidden() ? 1 : 0, dock->isVisible() ? 1 : 0,
			  dock->isFloating() ? 1 : 0,
			  static_cast<int>(main->dockWidgetArea(dock)), dock->width(),
			  dock->height(), dock->x(), dock->y(),
			  qUtf8Printable(partners.join(QStringLiteral(","))));
	}
}

/*
 * Reapply OBS's own saved dock layout, from the bytes OBS actually persisted.
 *
 * WHY NOT restoreDockWidget(). It was tried, and it reports success while doing the wrong thing.
 * Measured across one close/open cycle on a dock the creator had placed:
 *
 *   at exit:  'xive_chat_dock' hidden=0 visible=1 floating=0 area=2 geom=319x702+1601+27
 *   on load:  'xive_chat_dock': restored (hidden=1 visible=0 floating=1 area=2)
 *
 * The saved state was right both in the decoded DockState and at exit, so the fault was never in
 * saving. obs_frontend_add_dock_by_id adds a plugin dock floating and hidden — that is what the
 * 100x30-at-0,0 default on every unplaced dock records — and Qt leaves a FRESH placeholder behind
 * whenever a dock is floated or hidden, so it can go back. restoreDockWidget() finds that new
 * placeholder, not the one restoreState() laid down from the saved layout, and faithfully restores
 * OBS's default. It returns true because it found *a* placeholder; it was never ours.
 *
 * So read the bytes instead. `[BasicWindow] DockState` is base64 of QMainWindow::saveState(), and
 * handing it back to restoreState() reinstates the entire layout — every dock, tab group and
 * splitter — exactly as OBS wrote it. Being OBS's own state and not a store of our own invention,
 * it cannot shove anyone else's dock somewhere they did not put it.
 *
 * Runs once, queued, after all five docks exist: restoreState() can only place a dock whose widget
 * is already there, which is the whole reason the layout was lost in the first place.
 */
bool restoreLayoutFromObsConfig()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main) {
		xive_warn("no main window; cannot restore the dock layout");
		return false;
	}

	/* OBS >= 30.1 splits the per-user config out of the global one, and DockState lives in the
	 * user half (user.ini). This plugin is built against matching OBS 31/32 headers. */
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg) {
		xive_warn("no OBS user config; cannot restore the dock layout");
		return false;
	}

	const char *encoded = config_get_string(cfg, "BasicWindow", "DockState");
	if (!encoded || !*encoded) {
		/* Normal on a first run, and the one case where OBS's defaults are what we want. */
		xive_info("OBS has no saved DockState yet; leaving docks at their defaults");
		return false;
	}

	const QByteArray state = QByteArray::fromBase64(QByteArray(encoded));
	if (state.isEmpty()) {
		xive_warn("OBS DockState did not decode; leaving docks alone");
		return false;
	}

	/* False means Qt rejected the blob outright — a version or format mismatch, not a missing
	 * dock. Worth shouting about, because the layout silently reverting is the bug this whole
	 * exercise was about. */
	const bool ok = main->restoreState(state);
	if (ok)
		xive_info("dock layout restored from OBS DockState (%lld bytes)",
			  static_cast<long long>(state.size()));
	else
		xive_warn("OBS DockState was REJECTED by restoreState; docks keep OBS's defaults");
	return ok;
}

/*
 * Everything is built here rather than in obs_module_load.
 *
 * Two reasons: obs_frontend_* only works once the main window exists, and the Qt objects the
 * plugin relies on (QNetworkAccessManager, QTcpServer, QTimer) need a running application and
 * event loop. Modules load before the UI is up, so doing this work there is a crash waiting for
 * a slow machine.
 */
void buildUi()
{
	if (g_docksAdded)
		return;

	XiveConfig::instance().load();

	XiveAuth::createInstance();
	XiveApi::createInstance(XiveAuth::instance());
	XiveChannel::createInstance();
	XiveChatFeed::createInstance();
	XiveActivityFeed::createInstance();
	XiveSurgeFeed::createInstance();
	XiveRealtime::createInstance();
	/* Before the docks: the account dock connects to its signals in its constructor. */
	XiveUpdate::createInstance();

	/*
	 * Wire the realtime stream (uni_sse) into the polled views. Instant chat + deletes feed the
	 * chat feed, which backs its own poll off to a slow reconciliation cadence while the socket
	 * delivers; the live viewer count feeds the channel header; activity events feed the activity
	 * dock, whose poll deliberately does NOT back off (see XiveActivityFeed — a lost publication
	 * there is a gift alert that never fires). All strictly additive — every poll keeps running
	 * as the fallback, so a socket that never opens changes nothing.
	 */
	if (XiveRealtime *rt = XiveRealtime::instance()) {
		QObject::connect(rt, &XiveRealtime::chatMessage, XiveChatFeed::instance(),
				 &XiveChatFeed::ingestRealtimeMessage);
		QObject::connect(rt, &XiveRealtime::chatDeleted, XiveChatFeed::instance(),
				 &XiveChatFeed::applyRealtimeDelete);
		QObject::connect(rt, &XiveRealtime::connectedChanged, XiveChatFeed::instance(),
				 &XiveChatFeed::setRealtimeConnected);
		QObject::connect(rt, &XiveRealtime::viewers, XiveChannel::instance(),
				 &XiveChannel::applyRealtimeViewers);
		QObject::connect(rt, &XiveRealtime::activityEvent, XiveActivityFeed::instance(),
				 &XiveActivityFeed::ingestRealtimeEvent);
		/* ⚠️ QUEUED, unlike the others. applyRealtimeStreamState() calls back into
		 * XiveRealtime::setStreamId() when a go-live changes the id, which tears down the very
		 * SSE reply whose read is delivering this push. Deferring to the next event-loop turn
		 * takes that reentrancy off the table entirely. */
		QObject::connect(rt, &XiveRealtime::streamState, XiveChannel::instance(),
				 &XiveChannel::applyRealtimeStreamState, Qt::QueuedConnection);
		QObject::connect(rt, &XiveRealtime::connectedChanged, XiveActivityFeed::instance(),
				 &XiveActivityFeed::setRealtimeConnected);
		QObject::connect(rt, &XiveRealtime::surgeState, XiveSurgeFeed::instance(),
				 &XiveSurgeFeed::ingestRealtimeSurge);
		QObject::connect(rt, &XiveRealtime::connectedChanged, XiveSurgeFeed::instance(),
				 &XiveSurgeFeed::setRealtimeConnected);
	}

	/* obs_frontend_add_dock_by_id takes ownership of the widget, so these are deliberately
	 * parentless and deliberately not deleted here. The account dock is the one exception that
	 * keeps a pointer, because the launch check below has to drive it — OBS owns it either way,
	 * and it outlives this function. */
	auto *accountDock = new XiveDock();
	obs_frontend_add_dock_by_id(XiveDock::kDockId, obs_module_text("Dock.Account"), accountDock);

	/*
	 * ── CHAT, ACTIVITY, GIFTS AND QUICK ACTIONS ARE THE WEBSITE ─────────────────────────────
	 *
	 * Each is a CEF panel on one of the studio's popout routes, so a dock and the creator's
	 * studio tab are the same UI rather than two implementations that drift. See
	 * ui/xive-browser-dock.h for why, and for how the panel signs in.
	 *
	 * ⚠️ THE DOCK IDS ARE UNCHANGED. OBS keys its saved layout on them, so reusing
	 * `xive_chat_dock` for the panel means an existing creator's placement survives the
	 * upgrade; a new id would have dropped every dock back to floating-and-hidden.
	 *
	 * The native docks below are the no-panel fallback, not a second product: obs-browser
	 * refuses panels on Wayland and is absent from some self-built OBS installs, and shipping
	 * four blank docks to those creators would be worse than shipping the Qt ones they have
	 * today. Everywhere else this branch is never taken.
	 */
	const bool panels = XiveBrowserDock::available();

	if (panels) {
		/*
		 * Every registration goes through this, because obs_frontend_add_dock_by_id() with a
		 * null widget is a crash, not an empty dock — and create() can still refuse one
		 * panel after available() has said yes. `fallback` is the Qt dock to use instead;
		 * null for the actions dock, which has no Qt ancestor to fall back to.
		 */
		auto addPanel = [](const char *id, const char *title, const QString &kind,
				   QWidget *(*makeFallback)()) {
			if (QWidget *panel = XiveBrowserDock::create(kind)) {
				obs_frontend_add_dock_by_id(id, title, panel);
				return;
			}
			if (!makeFallback) {
				xive_warn("no panel for '%s'; dock not registered", id);
				return;
			}
			xive_warn("no panel for '%s'; using the native dock", id);
			obs_frontend_add_dock_by_id(id, title, makeFallback());
		};

		addPanel(XiveChatDock::kDockId, obs_module_text("Dock.Chat"),
			 QStringLiteral("chat"), []() -> QWidget * { return new XiveChatDock(); });
		addPanel(XiveActivityDock::kDockId, obs_module_text("Dock.Activity"),
			 QStringLiteral("activity"),
			 []() -> QWidget * { return new XiveActivityDock(); });
		addPanel(XiveAlertsDock::kDockId, obs_module_text("Dock.Alerts"),
			 QStringLiteral("gifts"),
			 []() -> QWidget * { return new XiveAlertsDock(); });
		/*
		 * The one dock with no Qt ancestor. It exists because the web's quick actions are
		 * ten and the native card's were two — see xive-quick-actions.h for the eight that
		 * were never portable. With no panel there is nothing to register, and the manager
		 * dock keeps its own two instead.
		 */
		addPanel(kActionsDockId, obs_module_text("Dock.Actions"),
			 QStringLiteral("actions"), nullptr);
		/*
		 * Xive + Twitch + Kick in one chat. The existing Chat dock stays Xive-only on
		 * purpose: a creator who never set up other platforms keeps exactly what they had.
		 */
		addPanel(kMultichatDockId, obs_module_text("Dock.MultiChat"),
			 QStringLiteral("multichat"), nullptr);
	} else {
		xive_warn("no browser panels: %s", qUtf8Printable(XiveBrowserDock::unavailableReason()));
		obs_frontend_add_dock_by_id(XiveChatDock::kDockId, obs_module_text("Dock.Chat"),
					    new XiveChatDock());
		obs_frontend_add_dock_by_id(XiveActivityDock::kDockId,
					    obs_module_text("Dock.Activity"), new XiveActivityDock());
		obs_frontend_add_dock_by_id(XiveAlertsDock::kDockId, obs_module_text("Dock.Alerts"),
					    new XiveAlertsDock());
	}

	/*
	 * Native either way. The title/category form and the channel link have no popout route to
	 * defer to — `/popout/actions` refuses the stream-details panel on purpose, because that
	 * panel carries the ingest credentials. Its quick-actions card is dropped whenever the
	 * browser dock above is carrying the real set.
	 */
	obs_frontend_add_dock_by_id(XiveManagerDock::kDockId, obs_module_text("Dock.Manager"),
				    new XiveManagerDock(!panels));

	g_docksAdded = true;

	/*
	 * Queued, so OBS has finished floating and hiding these before the saved layout goes back
	 * over the top of it. Inline it would simply be undone.
	 */
	QTimer::singleShot(0, accountDock, []() {
		logDockStates("before layout restore");
		restoreLayoutFromObsConfig();
		logDockStates("after layout restore");
	});

	/*
	 * Put "Xive" in OBS's Service menu now, before anyone opens Stream settings, so it is
	 * selectable even on a fresh install that has never signed in. The entry is refreshed with
	 * the real ingest URL later, when a credential fetch returns one.
	 */
	const XiveServiceCatalog::Result catalog = XiveServiceCatalog::ensureEntry();
	if (!catalog.ok)
		xive_warn("could not add Xive to the service list: %s",
			  qUtf8Printable(catalog.message));

	/* Only start polling if there is a session to poll with — an unsigned-in install should
	 * make no network requests at all until the creator connects. */
	if (XiveAuth::instance()->isSignedIn()) {
		xive_info("stored session found; resuming");
		XiveChannel::instance()->start();
	}

	xive_info("plugin ready (version %s)", XIVE_PLUGIN_VERSION);

	/*
	 * Is this copy still the published one? Silent unless it is not — the first check is 20s
	 * out so it never competes with the launch check below, and it needs no sign-in, so a
	 * creator whose session has expired still learns their plugin is old.
	 */
	XiveUpdate::instance()->start();

	/*
	 * The once-per-launch check: open the account dock, confirm the session, pull the ingest
	 * credentials down again, close the dock if it all worked. See XiveDock::runStartupCheck().
	 *
	 * Queued rather than called straight out: the QDockWidget OBS wraps this widget in has only
	 * just been created, and the check's first act is to find it and show it. Letting the event
	 * loop turn once means the frontend has finished with the dock before anything moves it.
	 */
	if (XiveConfig::instance().startupCheck())
		QTimer::singleShot(0, accountDock, [accountDock]() { accountDock->runStartupCheck(); });
	else
		xive_info("launch check disabled by preference");
}

void teardownUi()
{
	/* Before anything else here: whatever OBS saves has to be decided around now. */
	logDockStates("at exit");

	/*
	 * ⚠️ BEFORE ANYTHING ELSE THAT TOUCHES A DOCK.
	 *
	 * The browsers inside the panels have to be closed while OBS is still running, because CEF is
	 * shut down by obs-browser's obs_module_unload() — which happens in obs_shutdown(), AFTER the
	 * main window has destroyed our widgets. Leaving them open aborts OBS on every exit with a
	 * Chromium CHECK. See XiveBrowserDock::shutdown() for the crash it fixes.
	 *
	 * This closes browsers only. The docks themselves stay attached, for the reason spelled out
	 * below.
	 */
	XiveBrowserDock::shutdown();

	if (XiveChannel *ch = XiveChannel::instance())
		ch->stop();

	/*
	 * The docks are deliberately NOT removed here.
	 *
	 * OBS remembers a dock layout with QMainWindow::saveState(), which serialises only the docks
	 * still attached to the main window at the moment it runs. This used to call
	 * obs_frontend_remove_dock() for all five on the way out, which detaches and destroys them —
	 * so by the time OBS wrote its layout there were no Xive docks in it, and every restart came
	 * up with them missing no matter where they had been placed or whether the UI was locked.
	 *
	 * There is nothing to clean up by hand: obs_frontend_add_dock_by_id took ownership of these
	 * widgets, and OBS destroys them with the main window. Leaving them attached is both the fix
	 * and the smaller amount of work.
	 *
	 * ⚠️ THAT LAST SENTENCE HAS ONE EXCEPTION, and it is the CEF panels — see the
	 * XiveBrowserDock::shutdown() call above. "OBS destroys them with the main window" is exactly
	 * the problem there: that moment is after obs-browser has already shut CEF down. The widgets
	 * still go with the window; only the browsers inside them are closed early.
	 *
	 * The singletons below still go, and the docks outliving them is safe: no dock has a
	 * destructor, Qt drops a connection when its sender is destroyed, and every dock reaches a
	 * singleton through an `if (X *p = X::instance())` guard that reads the null.
	 *
	 * g_docksAdded stays true on purpose. The docks really are still added, and resetting it
	 * would let a second buildUi() register a duplicate set under ids OBS already knows.
	 */

	/*
	 * Realtime goes first: it holds an open streaming reply and a token request that runs through
	 * XiveApi, so it must be torn down while the API client and the event loop are still alive.
	 * Its token callback is QPointer-guarded, so a request still in flight resolves to a no-op.
	 * Order matters below too: the channel and the API client both hold a raw XiveAuth pointer.
	 */
	XiveRealtime::destroyInstance();
	XiveUpdate::destroyInstance();
	XiveActivityFeed::destroyInstance();
	XiveSurgeFeed::destroyInstance();
	XiveChatFeed::destroyInstance();
	XiveChannel::destroyInstance();
	XiveApi::destroyInstance();
	XiveAuth::destroyInstance();
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		buildUi();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		/* Tear down here rather than in obs_module_unload: by the time modules unload the
		 * Qt application is already going away, and destroying network objects against a
		 * dying event loop is how plugins produce crash-on-exit reports. */
		teardownUi();
		break;
	default:
		break;
	}
}

} // namespace

bool obs_module_load(void)
{
#ifdef _WIN32
	registerTlsBackendPath();
#endif
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
}
