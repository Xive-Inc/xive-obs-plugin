#include "ui/xive-dock.h"

#include "ui/xive-theme.h"
#include "xive-api.h"
#include "xive-auth.h"
#include "xive-channel.h"
#include "xive-config.h"
#include "xive-log.h"
#include "xive-overlay-source.h"
#include "xive-service.h"
#include "xive-update.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#ifdef _WIN32
#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#endif

#include <obs-frontend-api.h>

const char *XiveDock::kDockId = "xive_account_dock";

XiveDock::XiveDock(QWidget *parent) : QWidget(parent)
{
	XiveTheme::applyDockTheme(this);

	auto *shell = new QVBoxLayout(this);
	shell->setContentsMargins(0, 0, 0, 0);
	shell->setSpacing(0);

	shell->addWidget(XiveTheme::makeHeader(this, tr("Xive"), &m_headerDot, &m_headerState));

	auto *layout = new QVBoxLayout();
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);
	shell->addLayout(layout, 1);

	/* ---- update notice ----
	 *
	 * First in the column and hidden until there is something to say. A creator running a
	 * months-old build against an API that has moved is the failure this is here to catch, and
	 * it has to be the first thing on the panel when it happens — under the account card it
	 * would sit below the fold in a narrow dock.
	 */

	m_updateCard = XiveTheme::makeCard(this);
	auto *update = new QVBoxLayout(m_updateCard);
	update->setContentsMargins(10, 10, 10, 10);
	update->setSpacing(8);

	update->addWidget(XiveTheme::makeSectionLabel(m_updateCard, tr("UPDATE")));

	m_updateLabel = new QLabel(m_updateCard);
	m_updateLabel->setWordWrap(true);
	update->addWidget(m_updateLabel);

	auto *updateRow = new QHBoxLayout();
	updateRow->setSpacing(8);

	m_updateHowButton = new QPushButton(tr("How to update…"), m_updateCard);
	connect(m_updateHowButton, &QPushButton::clicked, this, &XiveDock::onUpdateHowClicked);
	updateRow->addWidget(m_updateHowButton, 1);

	m_updateSkipButton = new QPushButton(tr("Skip"), m_updateCard);
	m_updateSkipButton->setToolTip(
		tr("Hide this version. The next release will say so again, and \"Check for "
		   "updates\" brings this one back."));
	connect(m_updateSkipButton, &QPushButton::clicked, this, &XiveDock::onUpdateSkipClicked);
	updateRow->addWidget(m_updateSkipButton);

	update->addLayout(updateRow);

	m_updateCard->setVisible(false);
	layout->addWidget(m_updateCard);

	/* ---- account ---- */

	auto *accountCard = XiveTheme::makeCard(this);
	auto *account = new QVBoxLayout(accountCard);
	account->setContentsMargins(10, 10, 10, 10);
	account->setSpacing(8);

	account->addWidget(XiveTheme::makeSectionLabel(accountCard, tr("Account")));

	m_accountLabel = new QLabel(accountCard);
	m_accountLabel->setWordWrap(true);
	account->addWidget(m_accountLabel);

	m_connectButton = new QPushButton(accountCard);
	connect(m_connectButton, &QPushButton::clicked, this, &XiveDock::onConnectClicked);
	account->addWidget(m_connectButton);

	m_channelLabel = new QLabel(accountCard);
	m_channelLabel->setWordWrap(true);
	account->addWidget(m_channelLabel);

	layout->addWidget(accountCard);

	/* ---- OBS ingest ---- */

	auto *ingestCard = XiveTheme::makeCard(this);
	auto *ingest = new QVBoxLayout(ingestCard);
	ingest->setContentsMargins(10, 10, 10, 10);
	ingest->setSpacing(8);

	ingest->addWidget(XiveTheme::makeSectionLabel(ingestCard, tr("OBS output")));

	m_obsStateLabel = new QLabel(ingestCard);
	m_obsStateLabel->setWordWrap(true);
	ingest->addWidget(m_obsStateLabel);

	/* Pointing OBS at Xive is the whole reason this dock exists — the one gradient button. */
	m_applyButton = new QPushButton(tr("Set up OBS to stream to Xive"), ingestCard);
	m_applyButton->setObjectName(QStringLiteral("xivePrimary"));
	connect(m_applyButton, &QPushButton::clicked, this, &XiveDock::onApplyClicked);
	ingest->addWidget(m_applyButton);

	m_rotateButton = new QPushButton(tr("Rotate stream key…"), ingestCard);
	connect(m_rotateButton, &QPushButton::clicked, this, &XiveDock::onRotateClicked);
	ingest->addWidget(m_rotateButton);

	m_startupCheckBox = new QCheckBox(tr("Check this when OBS starts"), ingestCard);
	m_startupCheckBox->setToolTip(
		tr("On launch, confirm your Xive sign-in and fetch your stream key again. This panel "
		   "only opens if something needs you; otherwise your dock layout is left alone. "
		   "Turn it off if you would rather do it yourself."));
	m_startupCheckBox->setChecked(XiveConfig::instance().startupCheck());
	connect(m_startupCheckBox, &QCheckBox::toggled, this,
		[](bool on) { XiveConfig::instance().setStartupCheck(on); });
	ingest->addWidget(m_startupCheckBox);

	m_updateCheckBox = new QCheckBox(tr("Tell me about plugin updates"), ingestCard);
	m_updateCheckBox->setToolTip(
		tr("Once a day, ask Xive whether a newer version of this plugin has been published. "
		   "Nothing is downloaded or installed for you — the panel shows you the one command "
		   "that does it."));
	m_updateCheckBox->setChecked(XiveConfig::instance().updateCheck());
	connect(m_updateCheckBox, &QCheckBox::toggled, this, [](bool on) {
		XiveConfig::instance().setUpdateCheck(on);
		/* Answer straight away when it is switched back on, rather than at the next
		 * launch. start() re-checks instead of stacking a second timer. */
		if (on) {
			if (XiveUpdate *up = XiveUpdate::instance())
				up->start();
		}
	});
	ingest->addWidget(m_updateCheckBox);

	m_updateNowButton = new QPushButton(tr("Check for updates"), ingestCard);
	connect(m_updateNowButton, &QPushButton::clicked, this, [this]() {
		XiveUpdate *up = XiveUpdate::instance();
		if (!up)
			return;
		showMessage(tr("Checking for a newer version…"), false);
		up->checkNow(true);
	});
	ingest->addWidget(m_updateNowButton);

	layout->addWidget(ingestCard);

	/* ---- gift overlay ----
	 *
	 * Its own card rather than a fourth button under "OBS output": everything above changes
	 * where this OBS publishes, and this changes what is IN the creator's scene. Those are
	 * different kinds of change, and someone is entitled to see which one they are making.
	 */

	auto *overlayCard = XiveTheme::makeCard(this);
	auto *overlay = new QVBoxLayout(overlayCard);
	overlay->setContentsMargins(10, 10, 10, 10);
	overlay->setSpacing(8);

	overlay->addWidget(XiveTheme::makeSectionLabel(overlayCard, tr("Gift overlay")));

	auto *overlayHint = new QLabel(
		tr("Draws gift celebrations over your video, so they reach your viewers and your "
		   "replay — not just your own screen."),
		overlayCard);
	overlayHint->setWordWrap(true);
	overlay->addWidget(overlayHint);

	m_overlayButton = new QPushButton(tr("Add gift overlay to scene"), overlayCard);
	m_overlayButton->setToolTip(
		tr("Adds a full-canvas browser source to the scene you are editing. Press it again "
		   "and the overlay you already have is refreshed — you never end up with two "
		   "drawing the same gift."));
	connect(m_overlayButton, &QPushButton::clicked, this, &XiveDock::onAddOverlayClicked);
	overlay->addWidget(m_overlayButton);

	layout->addWidget(overlayCard);

	m_messageLabel = new QLabel(this);
	m_messageLabel->setWordWrap(true);
	m_messageLabel->setStyleSheet(XiveTheme::noticeCss(false));
	m_messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(m_messageLabel);

	layout->addStretch(1);

	if (XiveAuth *auth = XiveAuth::instance()) {
		connect(auth, &XiveAuth::signedInChanged, this, [this](bool signedIn) {
			refreshUi();
			if (signedIn) {
				if (XiveChannel *ch = XiveChannel::instance())
					ch->start();
			} else if (XiveChannel *ch = XiveChannel::instance()) {
				ch->stop();
				/* Not part of stopping the poll: the handle names the person who
				 * just signed out, and the share link is built from it. */
				ch->forgetUser();
			}
		});
		connect(auth, &XiveAuth::statusMessage, this,
			[this](const QString &m) { showMessage(m, false); });
		connect(auth, &XiveAuth::authFailed, this, [this](const QString &m) {
			setBusy(false);
			showMessage(m, true);
		});
	}

	if (XiveChannel *ch = XiveChannel::instance())
		connect(ch, &XiveChannel::changed, this, &XiveDock::refreshUi);

	/*
	 * upToDate and checkFailed are only ever emitted for a check a PERSON asked for (see
	 * xive-update.h), so writing them into the message line cannot trample what the launch
	 * check left there.
	 */
	if (XiveUpdate *up = XiveUpdate::instance()) {
		connect(up, &XiveUpdate::updateAvailable, this, &XiveDock::showUpdate);
		connect(up, &XiveUpdate::upToDate, this, [this](const QString &version) {
			showMessage(tr("Xive for OBS %1 is the latest version.").arg(version),
				    false);
		});
		connect(up, &XiveUpdate::checkFailed, this,
			[this](const QString &message) { showMessage(message, true); });

		/* A check that answered before this dock was built — possible only when the dock
		 * is rebuilt, but the notice would otherwise be lost until tomorrow. */
		if (up->hasPending())
			showUpdate(up->pending());
	}

	refreshUi();
}

/* ------------------------------------------------------------------------- */
/* The update notice                                                         */
/* ------------------------------------------------------------------------- */

void XiveDock::showUpdate(const XiveRelease &release)
{
	m_update = release;

	QString text = tr("<b>Xive for OBS %1 is available.</b><br>You are running %2.")
			       .arg(release.version.toHtmlEscaped(),
				    QString::fromUtf8(XIVE_PLUGIN_VERSION).toHtmlEscaped());

	if (!release.hasPlatform) {
		text += QStringLiteral("<br>") +
			tr("No build is published for this system yet.");
	} else if (!release.supported) {
		text += QStringLiteral("<br>") +
			(release.requiresNote.isEmpty()
				 ? tr("That build is not recommended for this system.")
				 : tr("That build needs %1.")
					   .arg(release.requiresNote.toHtmlEscaped()));
	}

	m_updateLabel->setText(text);
	m_updateCard->setVisible(true);
}

void XiveDock::onUpdateHowClicked()
{
	const QString command = XiveUpdate::installCommand(m_update.platform);

	QMessageBox box(this);
	box.setWindowTitle(tr("Update Xive for OBS"));
	box.setIcon(QMessageBox::Information);
	box.setTextInteractionFlags(Qt::TextSelectableByMouse);

	/* hasPlatform, not just the command: an installer can exist for a platform whose binary is
	 * not staged yet, in which case it would run, reach the manifest and refuse. Saying so here
	 * is better than sending someone to a command that tells them the same thing more slowly.
	 * Every platform the manifest carries has an installer now, so the empty-command half of
	 * this is the floor for a platform a future manifest names and this build does not know. */
	if (!m_update.hasPlatform || command.isEmpty()) {
		box.setText(tr("Version %1 is published, but there is no installer for this "
			       "system yet.")
				    .arg(m_update.version));

		QPushButton *open = nullptr;
		if (!m_update.url.isEmpty()) {
			box.setInformativeText(
				tr("A file is published for it; installing that file is manual."));
			open = box.addButton(tr("Open download"), QMessageBox::ActionRole);
		}
		box.addButton(QMessageBox::Close);
		box.exec();

		if (open && box.clickedButton() == open)
			QDesktopServices::openUrl(QUrl(m_update.url));
		return;
	}

	/*
	 * Whatever happens here, the installer is the same one the docs point at, it verifies the
	 * SHA-256 in the manifest, and it runs OUTSIDE this process — OBS holds this module's
	 * binary open for as long as it is running, so nothing can replace it from in here. The
	 * only question is who types the command.
	 */
	bool oneClick = false;
#ifdef _WIN32
	oneClick = (m_update.platform == QLatin1String("windows-x86_64"));
#endif

	if (oneClick) {
		box.setText(tr("OBS will close, the update will install, and OBS will start again."));
		/* The command is shown, not hidden behind the button: this is the moment a person
		 * gets to see what is about to run on their machine, and it is the only such
		 * moment. */
		box.setInformativeText(tr("This runs:\n\n%1\n\nNothing else is changed. You can also "
					  "copy it and run it yourself.")
					       .arg(command));
	} else {
		box.setText(tr("Close OBS, run this, then start OBS again:"));
		box.setInformativeText(command);
	}

	QPushButton *run = nullptr;
	if (oneClick)
		run = box.addButton(tr("Update and restart OBS"), QMessageBox::AcceptRole);
	QPushButton *copy = box.addButton(tr("Copy command"), QMessageBox::ActionRole);
	box.addButton(QMessageBox::Close);
	box.exec();

	if (run && box.clickedButton() == run) {
#ifdef _WIN32
		startWindowsUpdate();
#endif
		return;
	}

	if (box.clickedButton() == copy) {
		if (QClipboard *clip = QGuiApplication::clipboard()) {
			clip->setText(command);
			showMessage(tr("Update command copied. Close OBS, run it, then start OBS "
				       "again."),
				    false);
		}
	}
}

#ifdef _WIN32

bool XiveDock::startWindowsUpdate()
{
	/* 🔴 THE ONE UNRECOVERABLE MISTAKE HERE IS CLOSING OBS UNDER A LIVE BROADCAST. Checked
	 * before anything is spawned, and not re-checked afterwards — between this and the exit
	 * there is only the frontend's own shutdown, which asks its own questions. */
	if (obs_frontend_streaming_active() || obs_frontend_recording_active() ||
	    obs_frontend_replay_buffer_active() || obs_frontend_virtualcam_active()) {
		QMessageBox::warning(
			this, tr("Update Xive for OBS"),
			tr("Stop streaming, recording, the replay buffer and the virtual camera "
			   "first.\n\nUpdating closes OBS, because Windows will not let a plugin be "
			   "replaced while OBS has it loaded."));
		return false;
	}

	if (m_updateLaunched) {
		showMessage(tr("The updater is already waiting for OBS to close."), false);
		return false;
	}

	const QString url = XiveUpdate::installerUrl(m_update.platform);
	if (url.isEmpty()) {
		showMessage(tr("Could not work out where the installer lives."), true);
		return false;
	}

	/*
	 * The path of the OBS that is running, which is better than anything the installer could
	 * work out for itself: it probes three known locations, and a portable OBS is in none of
	 * them.
	 *
	 * Single quotes doubled because it is going inside a PowerShell single-quoted string, where
	 * that is the only escape. A creator named O'Brien has a home directory that would
	 * otherwise end the string early.
	 */
	QString obsExe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
	obsExe.replace(QLatin1Char('\''), QLatin1String("''"));

	/*
	 * 300 seconds, then the script gives up and says so rather than waiting forever: OBS may
	 * ask "are you sure you want to exit", and the answer may be no.
	 *
	 * Nothing in this string comes from the manifest. `url` is composed from the manifest URL
	 * this build was configured with — never from the JSON it returned — so a hostile manifest
	 * cannot get a command of its choosing onto this command line.
	 */
	const QString script = QStringLiteral("$ErrorActionPreference='Stop'; "
					      "& ([scriptblock]::Create((irm %1))) "
					      "-WaitForObs 300 -RelaunchObs '%2'")
				       .arg(url, obsExe);

	/* -NoExit so a failure stays on screen. This window is the only place the creator can see
	 * what happened, because by then OBS is gone. */
	const bool started = QProcess::startDetached(
		QStringLiteral("powershell.exe"),
		{QStringLiteral("-NoProfile"), QStringLiteral("-NoExit"),
		 QStringLiteral("-Command"), script});

	if (!started) {
		QMessageBox::warning(this, tr("Update Xive for OBS"),
				     tr("Could not start PowerShell, so nothing was changed.\n\n"
					"Close OBS and run the command yourself instead."));
		return false;
	}

	m_updateLaunched = true;
	xive_info("update: spawned the installer for %s; closing OBS",
		  qUtf8Printable(m_update.version));

	/*
	 * Queued, and via the main window rather than qApp->quit(): closing the window is OBS's
	 * own exit path, so the scene collection is saved and the creator's "confirm on exit"
	 * preference is still honoured. Queued because this runs from a button's click handler,
	 * inside a dialog that is still on the way down.
	 */
	if (auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window()))
		QMetaObject::invokeMethod(main, "close", Qt::QueuedConnection);

	return true;
}

#endif /* _WIN32 */

void XiveDock::onUpdateSkipClicked()
{
	XiveConfig::instance().setSkippedUpdateVersion(m_update.version);
	m_updateCard->setVisible(false);
	showMessage(tr("Hiding %1. \"Check for updates\" brings it back.").arg(m_update.version),
		    false);
}

void XiveDock::refreshUi()
{
	XiveAuth *auth = XiveAuth::instance();
	const bool signedIn = auth && auth->isSignedIn();
	XiveChannel *ch = XiveChannel::instance();

	if (signedIn) {
		/*
		 * ⚠️ THE XIVE NAME, NOT THE IDENTITY PROVIDER'S.
		 *
		 * accountLabel() is built from Authentik's OIDC `name`/`email` claims — a legal name
		 * and whatever address the account was opened with. Creators do not recognise
		 * themselves in it; the name on their channel page is the one they know. So: display
		 * name, then the handle, and only then the account label, which stays as the last
		 * resort because it is the one thing that is always known the moment a session exists.
		 *
		 * See XiveChannel::displayName(). Both seeds emit changed(), which lands here.
		 */
		QString label = ch ? ch->displayName() : QString();
		if (label.isEmpty() && ch && !ch->username().isEmpty())
			label = QStringLiteral("@") + ch->username();
		if (label.isEmpty())
			label = auth->accountLabel();

		m_accountLabel->setText(label.isEmpty()
						? tr("<b>Signed in to Xive.</b>")
						: tr("<b>Signed in as</b><br>%1").arg(label.toHtmlEscaped()));
		m_connectButton->setText(tr("Sign out"));
	} else {
		m_accountLabel->setText(tr("Not connected to Xive."));
		m_connectButton->setText(tr("Connect to Xive"));
	}

	XiveTheme::setLiveState(m_headerDot, m_headerState, signedIn && ch && ch->isLive());

	if (signedIn && ch && !ch->streamId().isEmpty()) {
		const QString state = ch->isLive()
					      ? tr("LIVE — %n viewer(s)", "", ch->viewers())
					      : tr("offline");
		m_channelLabel->setText(tr("<b>Channel</b><br>%1<br>%2")
						.arg(ch->title().isEmpty()
							     ? tr("(untitled)")
							     : ch->title().toHtmlEscaped(),
						     state));
	} else if (signedIn) {
		m_channelLabel->setText(tr("No channel resolved yet."));
	} else {
		m_channelLabel->clear();
	}

	/* Show whether OBS is already pointed at Xive. See XiveService::pointedAtXive() for why this
	 * is not a comparison against the API host. */
	const QString server = XiveService::currentServerUrl();
	if (XiveService::pointedAtXive()) {
		m_obsStateLabel->setText(XiveService::hasStreamKey()
						 ? tr("OBS is configured for Xive.")
						 : tr("OBS is set to Xive but has no stream key yet."));
	} else if (server.isEmpty()) {
		m_obsStateLabel->setText(tr("OBS has no stream server configured."));
	} else {
		m_obsStateLabel->setText(
			tr("OBS is currently configured for another service:<br>%1")
				.arg(server.toHtmlEscaped()));
	}

	m_applyButton->setEnabled(signedIn && !m_busy);
	m_rotateButton->setEnabled(signedIn && !m_busy && ch && !ch->streamId().isEmpty());
	/* No channel needed: the overlay link is issued against the ACCOUNT, not a broadcast, so
	 * a creator can set the source up before they have ever gone live. */
	m_overlayButton->setEnabled(signedIn && !m_busy);
	m_connectButton->setEnabled(!m_busy);
}

void XiveDock::setBusy(bool busy)
{
	m_busy = busy;
	refreshUi();
}

void XiveDock::showMessage(const QString &message, bool isError)
{
	m_messageLabel->setStyleSheet(XiveTheme::noticeCss(isError));
	m_messageLabel->setText(message);
}

/* ------------------------------------------------------------------------- */
/* The once-per-launch check                                                 */
/* ------------------------------------------------------------------------- */

void XiveDock::revealDock()
{
	/*
	 * Only ever opens, never closes — see finishStartupCheck(). Closing is the user's decision
	 * and OBS already remembers it; the one thing the plugin is entitled to do is surface a
	 * panel that has something the creator has to act on.
	 *
	 * obs_frontend_add_dock_by_id wraps the widget in a QDockWidget the plugin never sees, and
	 * the frontend API offers no way to get it back. It does set the dock's object name to the
	 * id, though, so it can be found under the main window.
	 */
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main) {
		xive_warn("no main window; cannot open the account dock");
		return;
	}

	auto *dock = main->findChild<QDockWidget *>(QString::fromUtf8(kDockId));
	if (!dock) {
		/* Not fatal: everything else about the check still works, the panel simply stays as
		 * the user left it. Logged because it would otherwise look like the check never ran. */
		xive_warn("account dock widget '%s' not found; leaving its visibility alone",
			  kDockId);
		return;
	}

	/* Already where the creator put it — raising it would steal focus from whatever they were
	 * doing to tell them something the panel is already showing. */
	if (dock->isVisible()) {
		xive_info("launch check needs attention; account dock is already open");
		return;
	}

	dock->setVisible(true);
	dock->raise();
	xive_info("launch check needs attention; account dock opened");
}

void XiveDock::runStartupCheck()
{
	m_inStartupCheck = true;

	/* One line saying what OBS was pointed at when the check ran. Cheap, and it is the only
	 * record of why the check decided what it decided — the dock's message is gone as soon as
	 * the creator clicks something. */
	xive_info("launch check: OBS output is %s (server: %s)",
		  XiveService::pointedAtXive() ? "Xive" : "not Xive",
		  qUtf8Printable(XiveService::currentServerUrl()));

	XiveAuth *auth = XiveAuth::instance();
	if (!auth || !auth->isSignedIn()) {
		showMessage(tr("Connect to Xive to finish setting OBS up."), false);
		finishStartupCheck(false);
		return;
	}

	setBusy(true);
	showMessage(tr("Checking your Xive sign-in…"), false);

	/*
	 * withAccessToken() refreshes when the cached token is missing or near expiry, so this is
	 * the check: a token comes back or it does not. It hands back an empty string in two very
	 * different situations, which is why isSignedIn() is consulted again afterwards — the auth
	 * layer only clears the session when the refresh token is genuinely dead (400 invalid_grant
	 * or 401). A 5xx or an offline laptop keeps the session, and telling that creator to sign in
	 * again would be both wrong and destructive.
	 */
	auth->withAccessToken([this](const QString &token) {
		if (!token.isEmpty()) {
			fetchCredentialsOnStartup();
			return;
		}

		setBusy(false);

		XiveAuth *current = XiveAuth::instance();
		if (current && current->isSignedIn())
			showMessage(tr("Could not reach Xive to check your sign-in. Your session is "
				       "still saved — try again in a moment."),
				    true);
		else
			showMessage(tr("Your Xive sign-in has expired. Connect again to keep "
				       "streaming to Xive."),
				    true);

		finishStartupCheck(false);
	});
}

void XiveDock::fetchCredentialsOnStartup()
{
	XiveApi *api = XiveApi::instance();
	if (!api) {
		setBusy(false);
		finishStartupCheck(false);
		return;
	}

	/*
	 * 🔴 Do not quietly repoint an encoder that is set up for somebody else.
	 *
	 * A creator who streams to Twitch from this OBS profile and only sometimes to Xive would
	 * otherwise find their server and key replaced by the mere act of launching. The check says
	 * so and stops; the "Set up OBS to stream to Xive" button is still one click away, and that
	 * click is a decision the person made.
	 */
	const QString server = XiveService::currentServerUrl();
	if (!server.isEmpty() && !XiveService::pointedAtXive()) {
		/* Logged with the URL: this bailing out wrongly is exactly the failure that is
		 * invisible otherwise — the creator sees a message about "another service" with no
		 * way to tell which URL the plugin objected to. */
		xive_info("launch check: OBS is set up for another service (%s); leaving it alone",
			  qUtf8Printable(server));
		setBusy(false);
		showMessage(tr("Signed in to Xive. OBS is still set up for another service, so "
			       "nothing was changed — use the button above when you want to stream "
			       "to Xive.\n\nCurrent server: %1")
				    .arg(server),
			    false);
		finishStartupCheck(false);
		return;
	}

	showMessage(tr("Fetching your stream key…"), false);

	/*
	 * refresh-credentials ONLY. There is deliberately no fall through to obs/setup here, unlike
	 * the button: setup MINTS A NEW CHANNEL when it cannot reuse one, and creating a channel is
	 * not something a launch check should do behind a creator's back. No channel yet simply
	 * leaves the dock open pointing at the button.
	 */
	api->obsRefreshCredentials([this](bool ok, const QJsonObject &body, int status,
					  const QString &error) {
		if (ok && !body.value(QStringLiteral("streamKey")).toString().isEmpty()) {
			/* applyFromResponse() finishes the run: it writes the credentials, reports
			 * the result, and closes the dock if the write succeeded. */
			applyFromResponse(body);
			return;
		}

		setBusy(false);

		if (status == 404 || status == 409)
			showMessage(tr("Signed in to Xive. You do not have a Xive channel yet — use "
				       "the button above to create one and set OBS up."),
				    false);
		else
			showMessage(error, true);

		finishStartupCheck(false);
	});
}

void XiveDock::finishStartupCheck(bool succeeded)
{
	if (!m_inStartupCheck)
		return;

	m_inStartupCheck = false;

	/*
	 * The check INTERRUPTS, it does not rearrange.
	 *
	 * It used to open the dock up front and close it again on a clean run, which meant the
	 * plugin overwrote this dock's visibility on every single launch — a creator who had docked
	 * Xive into their layout found it closed each time OBS started, and one who had closed it
	 * saw it flash open. That is the plugin fighting the layout the user arranged, and no amount
	 * of locking the UI would have stopped it.
	 *
	 * Now nothing is touched unless the check has something to say. A clean run leaves the dock
	 * exactly as it was found — open if the creator keeps it open, closed if they do not — and
	 * OBS's own saved dock state is left to be the only thing deciding that.
	 */
	if (!succeeded)
		revealDock();
}

void XiveDock::onConnectClicked()
{
	XiveAuth *auth = XiveAuth::instance();
	if (!auth)
		return;

	if (auth->isSignedIn()) {
		const auto choice = QMessageBox::question(
			this, tr("Sign out of Xive"),
			tr("Sign out of Xive in OBS?\n\nYour stream settings stay as they are — "
			   "this only forgets the saved sign-in."),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (choice != QMessageBox::Yes)
			return;

		auth->signOut();
		return;
	}

	showMessage(tr("Opening your browser…"), false);
	auth->beginSignIn();
}

void XiveDock::onApplyClicked()
{
	XiveApi *api = XiveApi::instance();
	XiveChannel *ch = XiveChannel::instance();
	if (!api || !ch)
		return;

	if (XiveService::streamingActive()) {
		showMessage(tr("OBS is streaming right now. Stop the broadcast first."), true);
		return;
	}

	setBusy(true);
	showMessage(tr("Fetching your ingest credentials…"), false);

	/*
	 * refresh-credentials FIRST, always. It is read-only and resolves by creator, preferring a
	 * channel that is currently live. obs/setup, by contrast, refuses to reuse a LIVE row and
	 * mints a brand-new channel instead — which on 2026-08-09 left a creator's studio bound to
	 * an empty channel while their real broadcast carried on elsewhere. Only fall through to
	 * setup when the creator genuinely has no channel yet.
	 */
	api->obsRefreshCredentials([this, api, ch](bool ok, const QJsonObject &body, int status,
						   const QString &error) {
		if (ok && !body.value(QStringLiteral("streamKey")).toString().isEmpty()) {
			applyFromResponse(body);
			return;
		}

		/* No channel yet: create one. Any other failure is reported as-is rather than
		 * guessing that creation is the fix. */
		const bool noChannel = status == 404 || status == 409;
		if (!ok && !noChannel) {
			setBusy(false);
			showMessage(error, true);
			return;
		}

		showMessage(tr("Creating your Xive channel…"), false);

		const QString title = ch->title().isEmpty() ? tr("Live on Xive") : ch->title();
		api->obsSetup(title, ch->category(), ch->categorySlug(),
			      [this](bool ok2, const QJsonObject &body2, int, const QString &error2) {
				      if (!ok2) {
					      setBusy(false);
					      showMessage(error2, true);
					      return;
				      }
				      applyFromResponse(body2);
			      });
	});
}

void XiveDock::applyFromResponse(const QJsonObject &body)
{
	const QString serverUrl = body.value(QStringLiteral("serverUrl")).toString();
	const QString streamKey = body.value(QStringLiteral("streamKey")).toString();
	QString streamId = body.value(QStringLiteral("streamId")).toString();

	if (!streamId.isEmpty()) {
		if (XiveChannel *ch = XiveChannel::instance())
			ch->setStreamId(streamId);
	} else if (XiveChannel *ch = XiveChannel::instance()) {
		/* The publish path is built from the stream id, so fall back to the channel we
		 * already resolved rather than writing settings that cannot authorise. */
		streamId = ch->streamId();
	}

	const XiveService::ApplyResult result =
		XiveService::applyCredentials(serverUrl, streamId, streamKey);

	setBusy(false);
	showMessage(result.message, !result.ok);

	if (result.ok) {
		if (XiveChannel *ch = XiveChannel::instance())
			ch->start();
	}

	/* No-op unless this came from the launch check, which closes the dock on a clean run. */
	finishStartupCheck(result.ok);
}

void XiveDock::onRotateClicked()
{
	XiveApi *api = XiveApi::instance();
	XiveChannel *ch = XiveChannel::instance();
	if (!api || !ch)
		return;

	if (XiveService::streamingActive()) {
		showMessage(tr("OBS is streaming right now. Stop the broadcast first."), true);
		return;
	}

	/*
	 * Rotation is destructive in a way that is easy to underestimate: the Xive stream key is
	 * PERMANENT per creator by design, so it may be sitting in other encoders, a second PC, or
	 * a streaming appliance. All of those stop working the moment it rotates.
	 */
	const auto choice = QMessageBox::warning(
		this, tr("Rotate your Xive stream key"),
		tr("This replaces your Xive stream key with a new one.\n\n"
		   "Any other encoder or device still set up with the old key will stop being able "
		   "to go live until you update it there too.\n\nRotate the key?"),
		QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
	if (choice != QMessageBox::Yes)
		return;

	setBusy(true);
	showMessage(tr("Rotating your stream key…"), false);

	const QString title = ch->title().isEmpty() ? tr("Live on Xive") : ch->title();
	api->obsRotateKey(title, ch->category(), ch->categorySlug(),
			  [this](bool ok, const QJsonObject &body, int, const QString &error) {
				  if (!ok) {
					  setBusy(false);
					  showMessage(error, true);
					  return;
				  }
				  applyFromResponse(body);
			  });
}

/* ------------------------------------------------------------------------- */
/* The gift overlay                                                          */
/* ------------------------------------------------------------------------- */

/*
 * Fetch the creator's overlay link and put a browser source on it into the scene they are
 * editing.
 *
 * The halves are deliberately apart: XiveApi does the network, XiveOverlaySource does
 * everything that touches OBS, and this is the wiring plus the failure messages. Every one of
 * those failures is something a creator can act on, which is why none of them is a bare
 * "failed".
 */
void XiveDock::onAddOverlayClicked()
{
	XiveApi *api = XiveApi::instance();
	XiveAuth *auth = XiveAuth::instance();
	if (!api || !auth)
		return;

	if (!auth->isSignedIn()) {
		showMessage(tr("Connect to Xive first — the overlay link belongs to your "
			       "account."),
			    true);
		return;
	}

	setBusy(true);
	showMessage(tr("Fetching your gift overlay link…"), false);

	/*
	 * The scene is only touched once the link is in hand, and it is touched from this
	 * callback: QNetworkAccessManager delivers it back on the UI thread, which is the thread
	 * the frontend and scene calls inside XiveOverlaySource have to run on. Same shape as
	 * every other request in this dock, and nothing here blocks.
	 */
	api->overlayLink([this](bool ok, const QJsonObject &body, int status,
				const QString &error) {
		setBusy(false);

		if (!ok) {
			/*
			 * 503 is the ENVIRONMENT, not the creator: no overlay signing secret is
			 * configured, so there is no link to be had and pressing the button again
			 * will not produce one. Everything else — an expired sign-in, an
			 * unreachable API — arrives with the API client's own message, which
			 * already says which it was.
			 */
			if (status == 503)
				showMessage(tr("Gift overlays are not set up on this environment "
					       "yet, so nothing was changed."),
					    true);
			else
				showMessage(error, true);
			return;
		}

		/* HTTP 200 carrying `success: false` is a shape these routes really do answer
		 * with — see the note at the top of xive-api.h. */
		if (!body.value(QStringLiteral("success")).toBool()) {
			showMessage(tr("Xive could not issue an overlay link just now, so nothing "
				       "was changed."),
				    true);
			return;
		}

		const QString url =
			XiveOverlaySource::linkUrl(body.value(QStringLiteral("path")).toString());
		if (url.isEmpty()) {
			/* Logged without the value: the path carries the overlay token. */
			xive_warn("overlay: the link Xive returned was not a usable relative path");
			showMessage(tr("Xive returned an overlay link this version cannot use, so "
				       "nothing was changed."),
				    true);
			return;
		}

		const XiveOverlaySource::Result result = XiveOverlaySource::addToCurrentScene(url);
		showMessage(result.message, !result.ok);
	});
}
