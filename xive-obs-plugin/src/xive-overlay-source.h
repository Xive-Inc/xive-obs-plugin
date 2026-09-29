#pragma once

#include <QString>

/*
 * The gift overlay: a full-canvas browser source, in the scene the creator is editing.
 *
 * ── WHAT THIS IS ────────────────────────────────────────────────────────────────────────────────
 * `/live-overlay/gifts?t=<token>` renders gift celebrations on a transparent page. A browser source
 * pointed at it composites them into the encoded video, so they reach the viewers, the Cloudflare
 * recording and the creator's own preview from one render. Doing it by hand means copying a link
 * out of the website, creating a source, and typing the canvas size in twice; this does all three.
 *
 * ⚠️ THIS IS THE FIRST AND ONLY PLACE THE PLUGIN CREATES AN OBS SOURCE. Everything else it does to
 * OBS is a settings write (the service catalog, the stream service). Scene contents belong to the
 * creator, so the rules this module holds itself to are: add one thing, name it recognisably, never
 * add a second, and never remove or reorder anything that was already there.
 *
 * 🔴 THE LINK IS A BEARER CREDENTIAL. The `?t=` grants read access to one creator's gift feed. It
 * is never logged, and it lives only in the source's settings — the same place OBS already keeps a
 * stream key.
 */
namespace XiveOverlaySource {

struct Result {
	bool ok = false;
	/** True when an existing overlay source was refreshed rather than a new one created. */
	bool reused = false;
	/** Ready to show a creator, whether it worked or not. Never empty on failure. */
	QString message;
};

/** The name a freshly created overlay source is given. Creators may rename it; see the .cpp. */
const char *sourceName();

/**
 * Compose the absolute overlay URL from the origin-relative `path` the API returns.
 *
 * 🔴 Composed against the SITE origin, never the API origin — they are different hosts. Returns an
 * empty string for anything that is not a genuinely relative path, which is a refusal and must be
 * treated as one.
 */
QString linkUrl(const QString &path);

/**
 * Put the overlay in the scene the creator is editing, at the canvas size.
 *
 * Idempotent: an overlay source that already exists anywhere in this OBS is updated with the new
 * link and placed in the scene if it is not already there. A creator who clicks twice ends up with
 * one overlay, not two drawing every gift on top of each other.
 *
 * Must be called on the UI thread — it talks to the frontend API.
 */
Result addToCurrentScene(const QString &url);

} // namespace XiveOverlaySource
