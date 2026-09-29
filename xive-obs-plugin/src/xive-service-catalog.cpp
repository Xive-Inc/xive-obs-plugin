#include "xive-service-catalog.h"
#include "xive-log.h"

#include <obs.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace XiveServiceCatalog {

namespace {

/*
 * Recommended encoder settings advertised to OBS.
 *
 * 🔴 THESE ARE STARTING VALUES AND SHOULD BE CONFIRMED AGAINST PLATFORM POLICY.
 *
 * - 6000 kbps sits well under Cloudflare's "typically well under 12 Mbps" guidance, and is also
 *   what the legacy MediaMTX ingest accepted.
 * - h264 only: Cloudflare Stream accepts H.264 video and AAC audio ONLY, and refuses anything else
 *   outright (ERR_UNSUPPORTED_VIDEO_CODEC / ERR_UNSUPPORTED_AUDIO_CODEC).
 * - 🔴 keyint 2s is not cosmetic. Cloudflare requires a GOP of 2-8s and answers
 *   ERR_GOP_OUT_OF_RANGE outside it — OBS's "auto" default is 250 frames (8.33s at 30fps), which
 *   is out of range and was observed killing a real broadcast. 2s is also the low-latency setting.
 * - bframes 0: B-frames are incompatible with Cloudflare's Low-Latency HLS and cause jitter and
 *   sporadic buffering.
 * - Both 1080p and 720p are listed because the PRO tier allows up to 2160 while the free tier is
 *   capped at 720 server-side. OBS treats this list as guidance in the settings UI, not
 *   enforcement, and the server is the thing that actually enforces the tier cap.
 */
constexpr int kMaxVideoBitrate = 6000;
constexpr int kMaxAudioBitrate = 160;
constexpr int kKeyintSeconds = 2;
constexpr int kMaxFps = 60;

const char *kRtmpServicesModule = "rtmp-services";
const char *kServicesFile = "services.json";

/* Reads a JSON file into an object. Returns an empty object when it cannot be read or parsed. */
QJsonObject readJsonObject(const QString &path, bool *parsed = nullptr)
{
	if (parsed)
		*parsed = false;

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return QJsonObject();

	const QByteArray raw = f.readAll();
	f.close();

	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject()) {
		xive_warn("could not parse %s (%s)", qUtf8Printable(QFileInfo(path).fileName()),
			  qUtf8Printable(err.errorString()));
		return QJsonObject();
	}

	if (parsed)
		*parsed = true;
	return doc.object();
}

QString rtmpServicesConfigPath()
{
	obs_module_t *mod = obs_get_module(kRtmpServicesModule);
	if (!mod)
		return QString();

	char *path = obs_module_get_config_path(mod, kServicesFile);
	if (!path)
		return QString();

	const QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

QString rtmpServicesBundledPath()
{
	obs_module_t *mod = obs_get_module(kRtmpServicesModule);
	if (!mod)
		return QString();

	const char *dataPath = obs_get_module_data_path(mod);
	if (!dataPath)
		return QString();

	/* QStringLiteral needs an actual literal, not a const char * — QLatin1StringView is the
	 * right wrapper for a compile-time ASCII constant held in a variable. */
	return QDir(QString::fromUtf8(dataPath)).filePath(QLatin1StringView(kServicesFile));
}

/* The effective services.json OBS will read: the user copy if it parses, else the bundled one. */
QJsonObject loadEffectiveServices(QString *sourceDescription = nullptr)
{
	bool parsed = false;

	const QString configPath = rtmpServicesConfigPath();
	if (!configPath.isEmpty()) {
		const QJsonObject o = readJsonObject(configPath, &parsed);
		if (parsed) {
			if (sourceDescription)
				*sourceDescription = QStringLiteral("user copy");
			return o;
		}
	}

	const QString bundledPath = rtmpServicesBundledPath();
	if (!bundledPath.isEmpty()) {
		const QJsonObject o = readJsonObject(bundledPath, &parsed);
		if (parsed) {
			if (sourceDescription)
				*sourceDescription = QStringLiteral("bundled copy");
			return o;
		}
	}

	return QJsonObject();
}

QJsonObject buildXiveEntry(const QString &serverUrl)
{
	QJsonObject server;
	server[QStringLiteral("name")] = QStringLiteral("Primary");
	server[QStringLiteral("url")] = serverUrl;

	QJsonArray servers;
	servers.append(server);

	QJsonArray resolutions;
	resolutions.append(QStringLiteral("1920x1080"));
	resolutions.append(QStringLiteral("1280x720"));

	QJsonObject recommended;
	recommended[QStringLiteral("keyint")] = kKeyintSeconds;
	/* LL-HLS is incompatible with B-frames; OBS honours this hint in its settings UI. */
	recommended[QStringLiteral("bframes")] = 0;
	recommended[QStringLiteral("profile")] = QStringLiteral("main");
	recommended[QStringLiteral("max video bitrate")] = kMaxVideoBitrate;
	recommended[QStringLiteral("max audio bitrate")] = kMaxAudioBitrate;
	recommended[QStringLiteral("supported resolutions")] = resolutions;
	recommended[QStringLiteral("max fps")] = kMaxFps;

	QJsonArray videoCodecs;
	videoCodecs.append(QStringLiteral("h264"));

	QJsonObject entry;
	entry[QStringLiteral("name")] = QString::fromUtf8(serviceName());
	/* `common: true` puts Xive in the short default list rather than behind "Show all".
	 * Reasonable: this file is only edited on a machine where the Xive plugin is installed. */
	entry[QStringLiteral("common")] = true;
	entry[QStringLiteral("stream_key_link")] = QStringLiteral("https://thexive.com/live-dashboard");
	entry[QStringLiteral("more_info_link")] = QStringLiteral("https://thexive.com");
	entry[QStringLiteral("supported video codecs")] = videoCodecs;
	entry[QStringLiteral("servers")] = servers;
	entry[QStringLiteral("recommended")] = recommended;

	return entry;
}

} // namespace

const char *serviceName()
{
	return "Xive";
}

QString defaultIngestUrl()
{
	/* Matches CLOUDFLARE_STREAM_INGEST_HOST on the server. Only used before a live credential
	 * fetch has told us the real value; obs/setup is authoritative and creators still on the
	 * legacy MediaMTX ingest are handed that URL by the server, not by this default. */
	return QStringLiteral("rtmps://ingest.xivecdn.com:443/live/");
}

bool entryPresent()
{
	const QJsonObject root = loadEffectiveServices();
	const QJsonArray services = root.value(QStringLiteral("services")).toArray();

	for (const QJsonValue &v : services) {
		if (v.toObject().value(QStringLiteral("name")).toString() ==
		    QString::fromUtf8(serviceName()))
			return true;
	}
	return false;
}

Result ensureEntry(const QString &serverUrl)
{
	Result result;

	/*
	 * Canonicalise here, not at the call site, so every path into this function advertises the
	 * same URL.
	 *
	 * The trailing slash must go: OBS joins the server and the stream key with a "/" of its own,
	 * so a slash left on produces `…/live//<id>`. This ran the risk of being inconsistent —
	 * module load passes no URL and got the slashed default, while the apply path passed a
	 * stripped one — which would have shown one server URL in the dropdown and used another.
	 */
	QString url = (serverUrl.isEmpty() ? defaultIngestUrl() : serverUrl).trimmed();
	while (url.endsWith(QLatin1Char('/')))
		url.chop(1);

	const QString configPath = rtmpServicesConfigPath();
	if (configPath.isEmpty()) {
		result.message = QObject::tr(
			"OBS's stream-service list is unavailable, so Xive cannot be added to the "
			"Service menu.");
		xive_warn("could not resolve the rtmp-services config path");
		return result;
	}

	QString source;
	QJsonObject root = loadEffectiveServices(&source);
	if (root.isEmpty() || !root.contains(QStringLiteral("services"))) {
		/*
		 * Refuse rather than write a file built from nothing. Replacing services.json with
		 * a document containing only Xive would delete Twitch, YouTube and every other
		 * service from this OBS install.
		 */
		result.message = QObject::tr(
			"OBS's stream-service list could not be read, so it was left untouched.");
		xive_warn("services.json unreadable from both locations; not writing");
		return result;
	}

	QJsonArray services = root.value(QStringLiteral("services")).toArray();
	const QJsonObject entry = buildXiveEntry(url);

	int existingIndex = -1;
	for (int i = 0; i < services.size(); ++i) {
		if (services.at(i).toObject().value(QStringLiteral("name")).toString() ==
		    QString::fromUtf8(serviceName())) {
			existingIndex = i;
			break;
		}
	}

	if (existingIndex >= 0) {
		if (services.at(existingIndex).toObject() == entry) {
			result.ok = true;
			result.message = QObject::tr("Xive is already in the Service menu.");
			return result;
		}
		services.replace(existingIndex, entry);
	} else {
		services.prepend(entry);
	}

	root[QStringLiteral("services")] = services;

	const QByteArray serialised = QJsonDocument(root).toJson(QJsonDocument::Indented);

	/* Parse back what is about to be written. Committing a file that cannot be re-read would
	 * take out every service in OBS, so this is worth the extra parse. */
	QJsonParseError verify{};
	const QJsonDocument check = QJsonDocument::fromJson(serialised, &verify);
	if (verify.error != QJsonParseError::NoError || !check.isObject() ||
	    check.object().value(QStringLiteral("services")).toArray().isEmpty()) {
		result.message = QObject::tr("Refusing to write an invalid stream-service list.");
		xive_error("self-check of the generated services.json failed; not writing");
		return result;
	}

	const QString dir = QFileInfo(configPath).absolutePath();
	if (os_mkdirs(dir.toUtf8().constData()) == MKDIR_ERROR) {
		result.message = QObject::tr("Could not create OBS's service config directory.");
		return result;
	}

	/* QSaveFile writes to a temporary and renames on commit, so an interrupted write cannot
	 * leave a half-written services.json behind. */
	QSaveFile out(configPath);
	if (!out.open(QIODevice::WriteOnly)) {
		result.message = QObject::tr("Could not write OBS's stream-service list.");
		xive_warn("could not open services.json for writing");
		return result;
	}

	out.write(serialised);
	if (!out.commit()) {
		result.message = QObject::tr("Could not save OBS's stream-service list.");
		xive_warn("commit of services.json failed");
		return result;
	}

	xive_info("added/refreshed the Xive entry in OBS's service list (from the %s, %d services)",
		  qUtf8Printable(source), static_cast<int>(services.size()));

	result.ok = true;
	result.message = existingIndex >= 0
				 ? QObject::tr("Updated Xive in OBS's Service menu.")
				 : QObject::tr("Added Xive to OBS's Service menu.");
	return result;
}

} // namespace XiveServiceCatalog
