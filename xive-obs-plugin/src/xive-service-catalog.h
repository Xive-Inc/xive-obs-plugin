#pragma once

#include <QString>

/*
 * Getting "Xive" into OBS's Service dropdown.
 *
 * ── Why this is the only way ──────────────────────────────────────────────────────────────────
 *
 * OBS's Stream settings page is hardcoded to exactly two service types:
 * `OBSBasicSettings_Stream.cpp` compares `obs_service_get_type()` against `"rtmp_common"` and
 * `"rtmp_custom"` and queries properties from `"rtmp_common"` literally. A service type registered
 * by a plugin through obs_register_service() therefore CANNOT appear in that dropdown — checked
 * against the OBS 32.2.1 source, not assumed.
 *
 * The dropdown's contents come from the `rtmp-services` plugin's `services.json`, and
 * `open_services_file()` (rtmp-common.c) looks in the USER CONFIG copy first and only falls back to
 * the bundled one. So the supported way to add a service locally is to put it in that user copy.
 *
 * ── Why it re-runs on every launch ────────────────────────────────────────────────────────────
 *
 * That same user copy is what OBS's service updater overwrites when obsproject.com publishes a new
 * revision. Rather than pin the version (which would freeze every other service's settings), this
 * MERGES the Xive entry into whatever is currently there and re-applies it at each startup. If an
 * update drops our entry, the next launch puts it back, and official service updates keep flowing.
 *
 * ── The real fix ──────────────────────────────────────────────────────────────────────────────
 *
 * A pull request adding Xive to obsproject/obs-studio's `plugins/rtmp-services/data/services.json`.
 * Then every OBS install has it with no plugin involved. This is the local stand-in until then.
 */
namespace XiveServiceCatalog {

struct Result {
	bool ok = false;
	QString message;
};

/* The ingest URL used when no live value is known yet (module load, before any API call). */
QString defaultIngestUrl();

/*
 * Merge or refresh the "Xive" entry in the rtmp-services user services.json.
 *
 * `serverUrl` is the ingest URL to advertise; empty means defaultIngestUrl(). Passing the URL that
 * came back from obs/setup keeps the catalog entry and the credentials consistent by construction,
 * which is what lets the dock select the named service rather than a custom one.
 *
 * Never writes anything if the existing file cannot be parsed — a corrupt services.json would
 * break every service in OBS, not just Xive.
 */
Result ensureEntry(const QString &serverUrl = QString());

/* Whether a "Xive" entry is currently visible to OBS. */
bool entryPresent();

/* The service name as it appears in the dropdown, and as rtmp_common's "service" setting. */
const char *serviceName();

} // namespace XiveServiceCatalog
