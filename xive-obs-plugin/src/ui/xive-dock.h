#pragma once

#include "xive-release.h"

#include <QWidget>

class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;

/*
 * The account dock: sign in, see which channel you own, and push Xive's ingest credentials into
 * OBS's stream settings.
 */
class XiveDock : public QWidget {
	Q_OBJECT

public:
	static const char *kDockId;

	explicit XiveDock(QWidget *parent = nullptr);

	/*
	 * The once-per-launch check, run from OBS_FRONTEND_EVENT_FINISHED_LOADING.
	 *
	 * Opens this dock, confirms the stored session still works, pulls the ingest credentials
	 * down again, and closes the dock only if all of that succeeded and OBS is now pointed at
	 * Xive. Anything that needs a person — an expired sign-in, no channel yet, OBS set up for a
	 * different service — leaves the dock open with the reason on it, which is the entire point:
	 * a stream key rotated on the website should surface now and not at "Start Streaming".
	 *
	 * Safe to call when signed out; it then just shows the dock and asks for a sign-in.
	 */
	void runStartupCheck();

private slots:
	void onConnectClicked();
	void onApplyClicked();
	void onRotateClicked();

	/* Fetch the overlay link and put a browser source on it into the current scene. Safe to
	 * press twice — see XiveOverlaySource::addToCurrentScene(). */
	void onAddOverlayClicked();

	/* Show the update notice. Also called from the constructor, for the case where the check
	 * answered before this dock existed. */
	void showUpdate(const XiveRelease &release);
	/* The command that installs it, ready to copy — plus, on Windows, the button that runs it.
	 * See the note at the top of xive-update.h for what this is and is not allowed to do. */
	void onUpdateHowClicked();
	void onUpdateSkipClicked();

private:
	void refreshUi();

	/* Open the account dock because the launch check has something to act on. Never closes it:
	 * the creator's layout, and OBS's memory of it, own that. OBS's frontend API has no call
	 * for this, so the widget is found by the object name obs_frontend_add_dock_by_id gave it. */
	void revealDock();

	/* Second half of runStartupCheck(), once a usable access token is in hand. */
	void fetchCredentialsOnStartup();

	/* End the startup run: drop out of startup mode, and open the dock if it did NOT succeed. */
	void finishStartupCheck(bool succeeded);

	void setBusy(bool busy);
	void showMessage(const QString &message, bool isError);

#ifdef _WIN32
	/*
	 * Hand the update to the published installer and close OBS so it can do its work: spawn a
	 * detached PowerShell that waits for this process to exit, installs, and starts OBS again.
	 *
	 * Returns false — having said why — when it refuses or could not start anything, in which
	 * case OBS is left running and untouched and the copy-the-command path is still there.
	 */
	bool startWindowsUpdate();
#endif

	/* Applies whatever credentials a setup/refresh response carried. Kept in one place so the
	 * stream key has exactly one path from the network into OBS and is never stored. */
	void applyFromResponse(const QJsonObject &body);

	/* Header status pair — the popout's live dot and LIVE/OFFLINE word. */
	QLabel *m_headerDot = nullptr;
	QLabel *m_headerState = nullptr;
	QLabel *m_accountLabel = nullptr;
	QLabel *m_channelLabel = nullptr;
	QLabel *m_obsStateLabel = nullptr;
	QLabel *m_messageLabel = nullptr;
	QPushButton *m_connectButton = nullptr;
	QPushButton *m_applyButton = nullptr;
	QPushButton *m_rotateButton = nullptr;
	QPushButton *m_overlayButton = nullptr;
	QCheckBox *m_startupCheckBox = nullptr;

	/* The update notice. The card is hidden until a newer version is published. */
	QFrame *m_updateCard = nullptr;
	QLabel *m_updateLabel = nullptr;
	QPushButton *m_updateHowButton = nullptr;
	QPushButton *m_updateSkipButton = nullptr;
	QCheckBox *m_updateCheckBox = nullptr;
	QPushButton *m_updateNowButton = nullptr;
	XiveRelease m_update;

	bool m_busy = false;
	/* One updater per OBS run. The dialog can be reopened while the spawned PowerShell is
	 * already waiting for this process to exit, and a second waiter would install over the
	 * first one's work. */
	bool m_updateLaunched = false;
	/* True only while runStartupCheck() is in flight, so applyFromResponse() knows whether the
	 * credentials it just wrote were asked for by a person or by the launch check. */
	bool m_inStartupCheck = false;
};
