#include "xive-config.h"
#include "xive-log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>

#include <util/platform.h>

XiveConfig &XiveConfig::instance()
{
	static XiveConfig cfg;
	return cfg;
}

QString XiveConfig::apiBase()
{
	return QStringLiteral("https://api.thexive.com");
}

/*
 * The website origin, deliberately NOT derived from apiBase(): the API lives on its own host
 * (api.thexive.com) and stripping the subdomain to guess the site would be a guess. The two are
 * separate deployments and are written down separately.
 */
QString XiveConfig::siteBase()
{
	return QStringLiteral("https://thexive.com");
}

/*
 * The OBS plugin's OWN Authentik application, not the web/mobile one.
 *
 * Only the issuer carries the application slug. authorize/token/userinfo below are INSTANCE-wide
 * in Authentik and identical for every application — confirmed against both discovery documents
 * on 2026-08-20 — so the client id is what selects the provider there, not the URL.
 */
QString XiveConfig::authIssuer()
{
	return QStringLiteral("https://auth.thexive.com/application/o/xive-obs/");
}

QString XiveConfig::authorizeEndpoint()
{
	return QStringLiteral("https://auth.thexive.com/application/o/authorize/");
}

QString XiveConfig::tokenEndpoint()
{
	return QStringLiteral("https://auth.thexive.com/application/o/token/");
}

QString XiveConfig::userInfoEndpoint()
{
	return QStringLiteral("https://auth.thexive.com/application/o/userinfo/");
}

/*
 * The plugin's own public client, 2026-08-20. It used to borrow `xive-local` from the web and
 * mobile apps purely to keep the server unchanged, at the cost of an OBS session being
 * unrevokable without also killing that user's web and mobile sessions. It now has its own.
 *
 * That cost is paid on the server instead: a separate Authentik application means a separate
 * `iss` as well as a separate `aud`, and api/app/helpers/jwt.php checks BOTH against
 * OIDC_ISSUER/OIDC_AUDIENCE, which are now comma-separated lists carrying the web/mobile
 * application and this one. Changing this string without adding it there gets every API call
 * rejected with invalid_audience, while sign-in itself still appears to succeed.
 *
 * No secret accompanies it: this is a public client and the flow is Authorization Code + PKCE.
 */
QString XiveConfig::clientId()
{
	return QStringLiteral("UtWBT6FpIHLs72wuxmpnYLJe88UeuKug4rH6FEAI");
}

/*
 * Where the update check looks, and where the install command it shows points.
 *
 * media.xivecdn.com is the public media bucket (xive-media) — see scripts/publish-release.sh for
 * why release binaries live in the same bucket as user media, namespaced under obs/.
 *
 * XIVE_OBS_MANIFEST is the SAME override the shell installers take, so pointing a test machine at
 * a staging manifest moves the check, the download and the printed command together.
 */
QString XiveConfig::updateManifestUrl()
{
	const QByteArray override = qgetenv("XIVE_OBS_MANIFEST");
	if (!override.isEmpty())
		return QString::fromUtf8(override).trimmed();

	return QStringLiteral("https://media.xivecdn.com/obs/latest.json");
}

QString XiveConfig::filePath() const
{
	char *dir = obs_module_config_path("");
	if (!dir)
		return QString();

	const QString base = QString::fromUtf8(dir);
	bfree(dir);

	return QDir(base).filePath(QStringLiteral("config.json"));
}

void XiveConfig::load()
{
	const QString path = filePath();
	if (path.isEmpty())
		return;

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		xive_info("no stored config yet");
		return;
	}

	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
	f.close();

	if (!doc.isObject()) {
		xive_warn("stored config is not a JSON object; ignoring it");
		return;
	}

	const QJsonObject o = doc.object();
	m_refreshToken = o.value(QStringLiteral("refresh_token")).toString();
	m_streamId = o.value(QStringLiteral("stream_id")).toString();
	m_accountLabel = o.value(QStringLiteral("account_label")).toString();
	m_accountEmail = o.value(QStringLiteral("account_email")).toString();
	m_username = o.value(QStringLiteral("username")).toString();
	m_displayName = o.value(QStringLiteral("display_name")).toString();
	m_startupCheck = o.value(QStringLiteral("startup_check")).toBool(m_startupCheck);
	m_updateCheck = o.value(QStringLiteral("update_check")).toBool(m_updateCheck);
	m_skippedUpdateVersion = o.value(QStringLiteral("skipped_update_version")).toString();

	const int chat = o.value(QStringLiteral("chat_poll_ms")).toInt(m_chatPollMs);
	const int status = o.value(QStringLiteral("status_poll_ms")).toInt(m_statusPollMs);

	/* Clamp rather than trust the file: a hand-edited 10ms poll would hammer the API from
	 * every installed copy of the plugin at once. */
	m_chatPollMs = qBound(1000, chat, 60000);
	/*
	 * ⚠️ AN EXISTING INSTALL KEEPS ITS STORED 10000 AND DOES NOT PICK UP THE NEW 60000 DEFAULT.
	 * The default only applies when the key is absent, and every copy that has saved a config
	 * once has written it. So the request reduction that 0.3.0's lifecycle push buys reaches
	 * fresh installs immediately and upgrades only when `status_poll_ms` is removed from
	 * config.json.
	 *
	 * Deliberately NOT worked around by raising the lower bound: 5000 is there to let an
	 * operator debug with a fast poll, and silently overriding a value someone wrote by hand is
	 * a worse surprise than a slow rollout. If this matters at release time it wants a config
	 * schema version, not a clamp.
	 */
	m_statusPollMs = qBound(5000, status, 300000);

	xive_info("config loaded (signed in: %s)", m_refreshToken.isEmpty() ? "no" : "yes");
}

void XiveConfig::save() const
{
	const QString path = filePath();
	if (path.isEmpty()) {
		xive_error("cannot resolve the module config path; not saving");
		return;
	}

	/* obs_module_config_path only builds the string; the directory may not exist yet. */
	const QString dir = QFileInfo(path).absolutePath();
	if (os_mkdirs(dir.toUtf8().constData()) == MKDIR_ERROR) {
		xive_error("could not create the config directory");
		return;
	}

	QJsonObject o;
	o[QStringLiteral("refresh_token")] = m_refreshToken;
	o[QStringLiteral("stream_id")] = m_streamId;
	o[QStringLiteral("account_label")] = m_accountLabel;
	o[QStringLiteral("account_email")] = m_accountEmail;
	o[QStringLiteral("username")] = m_username;
	o[QStringLiteral("display_name")] = m_displayName;
	o[QStringLiteral("startup_check")] = m_startupCheck;
	o[QStringLiteral("update_check")] = m_updateCheck;
	o[QStringLiteral("skipped_update_version")] = m_skippedUpdateVersion;
	o[QStringLiteral("chat_poll_ms")] = m_chatPollMs;
	o[QStringLiteral("status_poll_ms")] = m_statusPollMs;

	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		xive_error("could not open the config file for writing");
		return;
	}

	/* Tighten permissions BEFORE the token hits the disk, so it is never briefly
	 * world-readable. */
	f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
	f.close();
}

void XiveConfig::setRefreshToken(const QString &token)
{
	if (m_refreshToken == token)
		return;
	m_refreshToken = token;
	save();
}

void XiveConfig::setStreamId(const QString &id)
{
	if (m_streamId == id)
		return;
	m_streamId = id;
	save();
}

void XiveConfig::setAccountLabel(const QString &label)
{
	if (m_accountLabel == label)
		return;
	m_accountLabel = label;
	save();
}

void XiveConfig::setAccountEmail(const QString &email)
{
	if (m_accountEmail == email)
		return;
	m_accountEmail = email;
	save();
}

void XiveConfig::setUsername(const QString &username)
{
	if (m_username == username)
		return;
	m_username = username;
	save();
}

void XiveConfig::setDisplayName(const QString &displayName)
{
	if (m_displayName == displayName)
		return;
	m_displayName = displayName;
	save();
}

void XiveConfig::setStartupCheck(bool enabled)
{
	if (m_startupCheck == enabled)
		return;
	m_startupCheck = enabled;
	save();
}

void XiveConfig::setUpdateCheck(bool enabled)
{
	if (m_updateCheck == enabled)
		return;
	m_updateCheck = enabled;
	save();
}

void XiveConfig::setSkippedUpdateVersion(const QString &version)
{
	if (m_skippedUpdateVersion == version)
		return;
	m_skippedUpdateVersion = version;
	save();
}

void XiveConfig::clearSession()
{
	m_refreshToken.clear();
	m_accountLabel.clear();
	m_accountEmail.clear();
	/* The handle names a PERSON. Leaving it behind would offer the next person to sign in a
	 * "copy your channel link" button pointing at the last one. */
	m_username.clear();
	m_displayName.clear();
	/*
	 * ⚠️ THE STREAM ID GOES TOO, though it is not a credential.
	 *
	 * It used to be kept — "it is re-resolved from the server anyway" — and that was wrong for
	 * the one case that matters. It is re-resolved only when the next creator is LIVE: my-active
	 * answers `hasActiveStream: false` for an offline channel and the cached id survives, and
	 * XiveChannel then seeds from the detail route for a stream belonging to the account that
	 * just signed out. That seed carries `profiles.username`, so the previous creator's handle
	 * was written straight back over the one just cleared above and the share link pointed at
	 * a channel the signed-in creator does not own.
	 *
	 * An id that names one account's broadcast is part of that account's session. It goes.
	 */
	m_streamId.clear();
	save();
}
