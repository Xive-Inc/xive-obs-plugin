#pragma once

#include <QString>

/*
 * Persisted plugin state, in a JSON file under the OBS module config dir
 * (~/.config/obs-studio/plugin_config/xive-obs-plugin/config.json on Linux).
 *
 * The refresh token lives here in plaintext with 0600 permissions. That is the same exposure
 * OBS itself accepts for the service credentials in service.json, and there is no cross-platform
 * secret store available to a plugin without dragging in a dependency per OS. It is written down
 * rather than hidden: a reader of this file who can also read the user's home dir can obtain a
 * 30-day Xive refresh token.
 */
class XiveConfig {
public:
	static XiveConfig &instance();

	void load();
	void save() const;

	QString refreshToken() const { return m_refreshToken; }
	void setRefreshToken(const QString &token);

	/* The creator's permanent OBS channel id, cached so the dock has something to show
	 * before the first network round trip. Never authoritative — refresh-credentials
	 * resolves by creator precisely because this cache goes stale. */
	QString streamId() const { return m_streamId; }
	void setStreamId(const QString &id);

	/* Display only: "Name (email)". Never used for authorization. */
	QString accountLabel() const { return m_accountLabel; }
	void setAccountLabel(const QString &label);

	/*
	 * The signed-in account's email on its own, rather than folded into accountLabel().
	 *
	 * It exists because a browser dock cannot otherwise tell WHOSE session it is holding. The
	 * panels keep their own web session in CEF localStorage, and that session survives a plugin
	 * sign-out; without an identity to compare it against, a creator who signs out and back in
	 * as a different account gets four docks still showing the previous one. Matched against the
	 * address `/api/profiles/me` returns, which is the same one Authentik puts in `email`.
	 *
	 * Identity only, never authorization — the token does that.
	 *
	 * Persisted because userinfo is only fetched after an interactive sign-in and NOT after a
	 * token refresh, so a relaunched OBS would otherwise hold a session and not know whose.
	 */
	QString accountEmail() const { return m_accountEmail; }
	void setAccountEmail(const QString &email);

	/*
	 * The creator's Xive handle, cached so the shareable channel link is on screen before the
	 * first stream-detail round trip answers.
	 *
	 * Stored without a leading "@". Display only: the link is rebuilt from it, nothing is ever
	 * authorised with it. Cleared on sign-out — it names a person, so the next person to sign in
	 * must not inherit it.
	 */
	QString username() const { return m_username; }
	void setUsername(const QString &username);

	/*
	 * The creator's Xive display name, cached for the same reason the handle is: so the dock can
	 * say who is signed in before the first round trip answers, instead of showing the identity
	 * provider's spelling of them and then swapping it out a second later.
	 *
	 * Cleared on sign-out — it names a person. See XiveChannel::displayName().
	 */
	QString displayName() const { return m_displayName; }
	void setDisplayName(const QString &displayName);

	void clearSession();

	/*
	 * Whether the account dock opens itself when OBS starts, to check the session and pull the
	 * ingest credentials down again.
	 *
	 * On by default: a stream key that has been rotated on the website — or a sign-in that
	 * quietly expired while OBS was closed — otherwise only comes to light when "Start
	 * Streaming" fails. Deliberately a stored preference and not a constant, because a dock that
	 * opens and closes itself is alarming if you cannot turn it off.
	 *
	 * Survives clearSession(): it is a preference, not part of the sign-in.
	 */
	bool startupCheck() const { return m_startupCheck; }
	void setStartupCheck(bool enabled);

	/*
	 * Whether the plugin looks for a newer published version — once a launch, once a day
	 * after that. Nothing is ever downloaded or installed automatically; see xive-update.h.
	 *
	 * On by default, and survives clearSession() for the same reason startupCheck() does: it
	 * is a preference, not part of the sign-in. A creator running a months-old build against a
	 * moving API is the failure this exists to prevent, and it is silent unless it has news.
	 */
	bool updateCheck() const { return m_updateCheck; }
	void setUpdateCheck(bool enabled);

	/*
	 * A version the creator pressed "Skip" on. Exact string match, so the next release
	 * supersedes it on its own and nothing has to be cleared.
	 */
	QString skippedUpdateVersion() const { return m_skippedUpdateVersion; }
	void setSkippedUpdateVersion(const QString &version);

	int chatPollMs() const { return m_chatPollMs; }
	int statusPollMs() const { return m_statusPollMs; }

	static QString apiBase();
	/* The website, not the API — the origin the creator's public links are built on. */
	static QString siteBase();
	static QString authIssuer();
	static QString authorizeEndpoint();
	static QString tokenEndpoint();
	static QString userInfoEndpoint();
	static QString clientId();

	/*
	 * The release manifest the update check reads — the same object the shell installers read,
	 * and the same XIVE_OBS_MANIFEST override they honour, so a staging bucket can be pointed
	 * at without rebuilding anything.
	 */
	static QString updateManifestUrl();

private:
	XiveConfig() = default;

	QString filePath() const;

	QString m_refreshToken;
	QString m_streamId;
	QString m_accountLabel;
	QString m_accountEmail;
	QString m_username;
	QString m_displayName;
	bool m_startupCheck = true;
	bool m_updateCheck = true;
	QString m_skippedUpdateVersion;

	/* Chat is a conversation, so it needs to feel immediate; the status/viewer poll is
	 * background information and does not. Both are deliberately gentle — a broadcaster
	 * leaves OBS open for hours. */
	int m_chatPollMs = 3000;
	/*
	 * my-active, and A BACKSTOP RATHER THAN A DISCOVERY MECHANISM since 0.3.0 — hence 60s where
	 * it used to be 10s.
	 *
	 * Going live, ending and retitling are pushed now (helpers/stream_lifecycle.php →
	 * XiveChannel::applyRealtimeStreamState), so this no longer decides how quickly the docks
	 * notice. It exists because `Realtime::publish` is best-effort and the channel has no
	 * history: a dropped publication would otherwise leave the dock reading "offline" for an
	 * entire broadcast. 60s bounds that, and costs one request a minute instead of six.
	 *
	 * ⚠️ DO NOT RAISE IT MUCH FURTHER without giving the channel recovery. This is the only
	 * thing that corrects a missed lifecycle event.
	 */
	int m_statusPollMs = 60000;
};
