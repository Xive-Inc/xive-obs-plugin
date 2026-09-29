#pragma once

#include <QString>

/*
 * Writing Xive's ingest credentials into OBS's own stream settings.
 *
 * This is the feature the plugin exists for: without it a creator copies a server URL and a
 * stream key out of a web page by hand every time something changes.
 */
namespace XiveService {

struct ApplyResult {
	bool ok = false;
	QString message;
	/* True when OBS's Service menu ended up showing "Xive" (rtmp_common) rather than
	 * "Custom..." (rtmp_custom). */
	bool namedService = false;
};

/*
 * Point OBS at Xive by replacing the configured streaming service with an rtmp_custom one.
 *
 * Refuses while a broadcast is active. Repointing the service mid-stream does not move the live
 * broadcast — OBS keeps using the output it started with — so the only thing it would accomplish
 * is silently changing where the NEXT "Start Streaming" goes, with no visible feedback. Worse, on
 * the Xive side the OBS stream row is permanent and reused per creator, so an operator who has
 * just repointed has no way to tell which channel they are about to publish to.
 *
 * Must be called on the Qt UI thread — obs_frontend_* is not thread-safe.
 *
 * Prefers the NAMED service: it first tries to get a "Xive" entry into OBS's service list, and if
 * that works it configures an `rtmp_common` service so the Stream settings page reads
 * "Service: Xive" with a Get Stream Key link, instead of "Custom...". It falls back to
 * `rtmp_custom` when the list cannot be edited, because a working custom setup beats a pretty
 * failure.
 *
 * 🔴 TAKES THE STREAM ID, AND THAT IS NOT OPTIONAL.
 *
 * MediaMTX authorises a publish by reading the **last path segment as a stream UUID** and the
 * ingest token from the **query string** (`api/app/streaming/mediamtx_auth.php`:
 * `mediamtxStreamIdFromPath` / `mediamtxIngestKeyFromPayload`). So what belongs in OBS's stream-key
 * field is `<streamId>?key=<token>`, NOT the bare token.
 *
 * Putting the bare token there makes the path `live/<token>`, which is not a UUID, and the server
 * answers 401 `path_not_stream_id` — while OBS reports a connection failure with no useful detail.
 * `obs/setup` returns `streamKey` as a bare token while `native/create` returns it pre-wrapped, so
 * the composition happens here, from the two halves, and both server shapes end up correct.
 */
ApplyResult applyCredentials(const QString &serverUrl, const QString &streamId,
			     const QString &streamKey);

/* True when OBS is currently streaming. */
bool streamingActive();

/* The server URL OBS is configured with right now, or an empty string. Shown to the creator when
 * OBS is set up for somebody else. Never returns the key. */
QString currentServerUrl();

/*
 * Whether OBS's streaming service is Xive's.
 *
 * 🔴 The obvious test — does the server URL contain "thexive.com" — is WRONG, and was the bug that
 * had the dock telling correctly-configured creators they were set up for another service.
 * thexive.com is the API host; the ingest host is ingest.xivecdn.com or Cloudflare Stream. Ask this
 * instead of comparing URLs at the call site.
 */
bool pointedAtXive();

/* Whether OBS currently holds a non-empty stream key. The key itself is deliberately not
 * returned — nothing in the UI needs to display it, and not fetching it means it cannot leak
 * into a log or a screenshot. */
bool hasStreamKey();

} // namespace XiveService
