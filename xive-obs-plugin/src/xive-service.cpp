#include "xive-service.h"
#include "xive-log.h"
#include "xive-service-catalog.h"

#include <obs-frontend-api.h>
#include <obs.h>

#include <QObject>
#include <QUrl>

namespace XiveService {

bool streamingActive()
{
	return obs_frontend_streaming_active();
}

/*
 * Strip the wrapper off a stream key that already carries one.
 *
 * `obs/setup` hands back a bare token; `native/create` hands back `<streamId>?key=<token>`. Mirrors
 * extractStreamKey() in mobile/app/src/features/broadcastSelection.ts so there is one rule.
 */
static QString ingestTokenFrom(const QString &rawKey)
{
	const QString raw = rawKey.trimmed();
	const int marker = raw.indexOf(QStringLiteral("?key="));
	if (marker < 0)
		return raw;
	return raw.mid(marker + 5).trimmed();
}

/*
 * Is this ingest URL Cloudflare Stream?
 *
 * A custom ingest domain is a DNS alias for live.cloudflare.com, so it cannot be recognised from
 * the URL alone — 🔴 ADD NEW CUSTOM INGEST HOSTS HERE, or their keys are composed into the MediaMTX
 * shape and every publish is refused. Mirrors isCloudflareIngestUrl() in
 * packages/core/src/utils/streaming/streamApiContracts.ts; the two must agree.
 */
static bool isCloudflareIngestUrl(const QString &serverUrl)
{
	const QString u = serverUrl.trimmed().toLower();
	if (u.isEmpty())
		return false;
	return u.contains(QStringLiteral("://live.cloudflare.com")) ||
	       u.contains(QStringLiteral(".cloudflare.com/")) ||
	       u.contains(QStringLiteral("://ingest.xivecdn.com"));
}

/*
 * The ingest URL OBS should hold.
 *
 * The trailing slash is removed deliberately. OBS joins the server and the stream key with a "/"
 * itself, so leaving one on would publish to `live//<id>` — and while the server's path parser
 * discards empty segments, MediaMTX's own path matching is what sees it first. The shape known to
 * work from real OBS publishes is exactly `…/live/<id>?key=…`.
 */
static QString canonicalServerUrl(const QString &serverUrl)
{
	QString url = serverUrl.trimmed();
	while (url.endsWith(QLatin1Char('/')))
		url.chop(1);
	return url;
}

ApplyResult applyCredentials(const QString &serverUrl, const QString &streamId,
			     const QString &streamKey)
{
	ApplyResult result;

	const bool cloudflare = isCloudflareIngestUrl(serverUrl);
	const QString token = cloudflare ? streamKey.trimmed() : ingestTokenFrom(streamKey);
	const QString id = streamId.trimmed();

	if (serverUrl.isEmpty() || token.isEmpty()) {
		result.message = QObject::tr("Xive did not return usable ingest credentials.");
		return result;
	}

	/*
	 * 🔴 The stream id is required for MediaMTX and MEANINGLESS for Cloudflare.
	 *
	 * MediaMTX reads the last path segment as the stream UUID, so the id has to be in the
	 * publish path. Cloudflare reads the last path segment AS THE KEY, so the same composition
	 * offers it a key it never issued and the publish is refused. Refusing to proceed without an
	 * id would therefore block exactly the ingest that does not need one.
	 */
	if (!cloudflare && id.isEmpty()) {
		/* Without the id the publish path cannot be built, and a bare token in the key
		 * field is precisely the configuration the ingest refuses. Better to say so than to
		 * write settings that fail at "Start Streaming" with no explanation. */
		result.message = QObject::tr(
			"Xive did not say which channel these credentials are for, so OBS was not "
			"changed. Try again in a moment.");
		return result;
	}

	if (streamingActive()) {
		result.message = QObject::tr(
			"OBS is streaming right now. Stop the broadcast before changing where it "
			"publishes.");
		return result;
	}

	/*
	 * Try for the named service first. The entry is built from the ingest URL we just received,
	 * so the catalog entry and the credentials always agree — which matters because rtmp_common
	 * resolves the server against that entry's server list.
	 */
	const QString url = canonicalServerUrl(serverUrl);
	const XiveServiceCatalog::Result catalog = XiveServiceCatalog::ensureEntry(url);

	/*
	 * MediaMTX: `<streamId>?key=<token>` — see the header comment. The token is percent-encoded
	 * because it lands in a query string; the id is a UUID and needs none.
	 *
	 * Cloudflare: the key IS the whole credential and travels verbatim. It is not percent-encoded
	 * and nothing is prepended — it becomes the last path segment, which is what Cloudflare reads
	 * as the key. Composing anything around it is the same defect that broke the phone
	 * (rtmpTargetFor) and the website (composeObsStreamKey).
	 */
	const QString publishKey =
		cloudflare ? token
			   : id + QStringLiteral("?key=") +
				     QString::fromUtf8(QUrl::toPercentEncoding(token));

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "server", url.toUtf8().constData());
	obs_data_set_string(settings, "key", publishKey.toUtf8().constData());

	const char *serviceType = "rtmp_custom";
	if (catalog.ok) {
		/* rtmp_common identifies the service by name out of services.json. */
		obs_data_set_string(settings, "service", XiveServiceCatalog::serviceName());
		serviceType = "rtmp_common";
	} else {
		xive_warn("falling back to a custom service: %s", qUtf8Printable(catalog.message));
	}

	obs_service_t *service =
		obs_service_create(serviceType, "xive_stream_service", settings, nullptr);
	obs_data_release(settings);

	if (!service) {
		xive_error("obs_service_create failed for %s", serviceType);
		result.message = QObject::tr("OBS refused to create the stream service.");
		return result;
	}

	obs_frontend_set_streaming_service(service);
	obs_frontend_save_streaming_service();

	/* set_streaming_service takes its own reference, so drop ours. */
	obs_service_release(service);

	xive_info("stream service repointed at Xive as %s (key not logged)",
		  catalog.ok ? "the named Xive service" : "a custom service");

	result.ok = true;
	result.namedService = catalog.ok;
	result.message = catalog.ok
				 ? QObject::tr("OBS is set up to stream to Xive. Stream settings now "
					       "show Service: Xive.")
				 : QObject::tr("OBS is set up to stream to Xive (as a custom "
					       "service).");
	return result;
}

QString currentServerUrl()
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!service)
		return QString();

	/* obs_frontend_get_streaming_service returns a borrowed pointer — do NOT release it.
	 * The settings object, however, is a new reference. */
	obs_data_t *settings = obs_service_get_settings(service);
	if (!settings)
		return QString();

	const char *server = obs_data_get_string(settings, "server");
	const QString url = server ? QString::fromUtf8(server) : QString();

	obs_data_release(settings);
	return url;
}

/*
 * Is OBS pointed at Xive right now?
 *
 * 🔴 Do not answer this by looking for "thexive.com" in the server URL. That is the API host, not
 * the ingest host — Xive publishes to `rtmps://ingest.xivecdn.com:443/live/`, or to Cloudflare
 * Stream — so a perfectly configured OBS never matched, and both the account dock and the launch
 * check reported "configured for another service" at creators who were configured for Xive.
 *
 * Two signals, in order of how sure they are:
 *
 *   1. The NAMED service. applyCredentials() prefers `rtmp_common` with the catalog's entry, which
 *      is what makes OBS's Service dropdown read "Xive". An exact name match is unambiguous.
 *   2. The ingest host, for the `rtmp_custom` fallback, where there is no name to read. This
 *      reuses isCloudflareIngestUrl() rather than defining a second idea of which hosts are ours —
 *      that function already carries the note that new ingest hosts must be added to it.
 */
bool pointedAtXive()
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!service)
		return false;

	/* Borrowed pointer — do NOT release. The settings are a new reference. */
	obs_data_t *settings = obs_service_get_settings(service);
	if (!settings)
		return false;

	bool ours = false;

	const char *type = obs_service_get_type(service);
	if (type && QString::fromUtf8(type) == QStringLiteral("rtmp_common")) {
		const char *name = obs_data_get_string(settings, "service");
		ours = name && QString::fromUtf8(name) ==
					QString::fromUtf8(XiveServiceCatalog::serviceName());
	}

	if (!ours) {
		const char *server = obs_data_get_string(settings, "server");
		const QString url = server ? QString::fromUtf8(server).toLower() : QString();
		ours = isCloudflareIngestUrl(url) ||
		       url.contains(QStringLiteral("xivecdn.com")) ||
		       url.contains(QStringLiteral("thexive.com"));
	}

	obs_data_release(settings);
	return ours;
}

bool hasStreamKey()
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (!service)
		return false;

	obs_data_t *settings = obs_service_get_settings(service);
	if (!settings)
		return false;

	const char *key = obs_data_get_string(settings, "key");
	const bool present = key && *key;

	obs_data_release(settings);
	return present;
}

} // namespace XiveService
