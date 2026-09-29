#pragma once

#include "xive-release.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/*
 * The update check.
 *
 * There is no auto-update and deliberately so: an OBS module cannot replace its own binary while
 * OBS has it loaded, and a plugin that installs code onto a creator's machine without being asked
 * is not something this should do quietly in the background. What it does is notice — once a
 * launch and once a day after that — that a newer version has been published, and offer the one
 * command that installs it.
 *
 * 🔴 THE RULE IS "NEVER WITHOUT BEING ASKED", NOT "NEVER". On Windows the notice also carries an
 * "Update and restart OBS" button (XiveDock::startWindowsUpdate), which spawns the SAME published
 * installer the copied command names, waits for OBS to exit, and starts OBS again afterwards. A
 * person clicks it, having been shown the exact command; nothing runs on a timer, and no other
 * platform has it. What that button does NOT do is take anything out of the manifest and run it:
 * the command it spawns is composed here from a URL this build already knows, so a compromised
 * manifest is exactly as (un)dangerous as it is for the copy-and-paste path, where the same
 * script verifies the same SHA-256.
 *
 * The manifest is the same obs/latest.json the shell installers read, fetched unauthenticated
 * from the CDN. That means the check works signed out, and that a creator whose session has
 * expired still finds out their plugin is old.
 *
 * SILENT unless there is something to say. `upToDate` and `checkFailed` are emitted ONLY for a
 * check a person asked for — a background check that fails (laptop offline, CDN blip) writes a
 * log line and nothing else, because the alternative is a panel that reports its own plumbing
 * over whatever the creator was reading.
 */
class XiveUpdate : public QObject {
	Q_OBJECT

public:
	static XiveUpdate *instance();
	static void createInstance();
	static void destroyInstance();

	explicit XiveUpdate(QObject *parent = nullptr);

	/** First check (after a delay, so it never competes with the launch check) and the daily
	 * timer. A no-op when the preference is off. Safe to call again: a second call re-checks
	 * rather than stacking a second timer, which is what makes it the "turned the preference
	 * back on" path too. */
	void start();

	/** `userInitiated` distinguishes the button from the timer: it re-shows a version the
	 * creator skipped, and it is the only mode that reports success or failure to the UI. */
	void checkNow(bool userInitiated);

	/** The newest published release, when it is newer than this build. Invalid otherwise —
	 * including after a check that found nothing new, which clears it. */
	XiveRelease pending() const { return m_pending; }
	bool hasPending() const { return m_pending.isValid(); }

	/**
	 * The one command that installs the current release on `platformKey`, or empty where no
	 * installer is published for it.
	 *
	 * Built from the manifest URL rather than hardcoded, so XIVE_OBS_MANIFEST points the whole
	 * mechanism — check, command and download — at a staging bucket in one move.
	 */
	static QString installCommand(const QString &platformKey);

	/**
	 * The installer script itself for `platformKey`, or empty where none is published.
	 *
	 * Separate from installCommand() because the Windows one-click path spawns the script with
	 * arguments rather than running the piped command, and BOTH have to point at the same
	 * place: a staging XIVE_OBS_MANIFEST must move the button as well as the copied line.
	 */
	static QString installerUrl(const QString &platformKey);

signals:
	void updateAvailable(const XiveRelease &release);
	void upToDate(const QString &version);
	void checkFailed(const QString &message);

private:
	QNetworkAccessManager *m_nam = nullptr;
	QTimer *m_timer = nullptr;
	/* In-flight guard. A second check while one is running is dropped rather than queued —
	 * the answer would be the same. */
	QNetworkReply *m_reply = nullptr;
	bool m_started = false;
	XiveRelease m_pending;
};
