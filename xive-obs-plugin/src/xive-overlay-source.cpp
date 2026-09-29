#include "xive-overlay-source.h"

#include "xive-config.h"
#include "xive-log.h"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QObject>

#include <cstring>

namespace XiveOverlaySource {

namespace {

/*
 * obs-browser's input id, identical on every platform. There is no second source type that can
 * render a web page, so an OBS built without obs-browser simply cannot host this overlay — that is
 * reported rather than worked around.
 */
const char *const kBrowserSourceId = "browser_source";

/*
 * How an existing overlay is recognised, and why it is the URL rather than the name.
 *
 * A creator may rename the source; OBS lets them and the name is theirs. What they cannot quietly
 * change is what it points at, so the path half of the link is the durable marker.
 *
 * ⚠️ THE TOKEN IS DELIBERATELY NOT PART OF THE MATCH. Rotating the overlay link changes the token,
 * and a source carrying a rotated-away token is precisely the one that has to be found and
 * refreshed rather than duplicated. Matching on the whole URL would have made every rotation
 * produce a second overlay.
 *
 * Mirrors LIVE_GIFT_OVERLAY_PATH in src/utils/routing/liveOverlayRoute.ts and OVERLAY_LINK_PATH in
 * api/app/streaming/overlay_link.php. All three have to agree; the server's is authoritative.
 */
const char *const kLinkPathMarker = "/live-overlay/gifts";

struct FindContext {
	obs_source_t *found = nullptr;
};

/** A browser source pointed at an overlay link. */
bool looksLikeOverlay(obs_source_t *source)
{
	const char *id = obs_source_get_id(source);
	if (!id || std::strcmp(id, kBrowserSourceId) != 0)
		return false;

	/* A NEW reference — released on both paths out of this function. */
	obs_data_t *settings = obs_source_get_settings(source);
	if (!settings)
		return false;

	const char *url = obs_data_get_string(settings, "url");
	const bool match =
		url && QString::fromUtf8(url).contains(QLatin1StringView(kLinkPathMarker));

	obs_data_release(settings);
	return match;
}

/*
 * ⚠️ RUNS WHILE LIBOBS HOLDS ITS SOURCE LIST LOCKED. Take a reference and get out; anything that
 * mutates a scene belongs after the enumeration, never inside it.
 *
 * Returning false ends the enumeration. The REF is what decides that, not the match:
 * obs_source_get_ref answers null for a source already on its way out, and stopping there
 * would mean missing a perfectly good overlay further down the list and creating a duplicate.
 */
bool findOverlayProc(void *param, obs_source_t *source)
{
	auto *ctx = static_cast<FindContext *>(param);

	if (!looksLikeOverlay(source))
		return true;

	/* obs_enum_sources' own contract: the pointer handed to a callback is valid only for the
	 * duration of the call, so retaining it means taking a reference. */
	ctx->found = obs_source_get_ref(source);
	return ctx->found == nullptr;
}

/** The overlay source anywhere in this OBS, or null. An OWNED reference — release it. */
obs_source_t *findExistingOverlay()
{
	FindContext ctx;
	obs_enum_sources(findOverlayProc, &ctx);
	return ctx.found;
}

/*
 * A source name nothing else is using.
 *
 * OBS's own UI refuses two sources with the same name and libobs does not, so reusing a taken name
 * would leave the creator with two entries they cannot tell apart. The plain name is the normal
 * answer — the numbered fallbacks only matter when something unrelated already holds it, which is
 * rare enough that giving up after 20 tries costs nothing.
 */
QString availableSourceName()
{
	const QString base = QString::fromUtf8(sourceName());

	for (int attempt = 1; attempt <= 20; ++attempt) {
		const QString candidate =
			attempt == 1 ? base : QStringLiteral("%1 %2").arg(base).arg(attempt);

		/* A new reference when the name is taken; nothing when it is free. */
		obs_source_t *taken = obs_get_source_by_name(candidate.toUtf8().constData());
		if (!taken)
			return candidate;
		obs_source_release(taken);
	}

	return QString();
}

/*
 * The scene the creator is editing. A NEW reference — release it.
 *
 * 🔴 THE PREVIEW SCENE FIRST, AND THAT IS THE WHOLE POINT. In Studio Mode
 * obs_frontend_get_current_scene() answers the PROGRAM scene — what is being broadcast right now —
 * so adding to it would drop a new source straight into a live stream with no chance to look at it
 * first. obs_frontend_get_current_preview_scene() answers the scene in the left-hand preview, which
 * is the one OBS's own "add a source" acts on, and answers null outside Studio Mode, where the two
 * are the same scene anyway.
 */
obs_source_t *currentEditingScene()
{
	if (obs_source_t *preview = obs_frontend_get_current_preview_scene())
		return preview;
	return obs_frontend_get_current_scene();
}

/*
 * Is this exact source already an item of this scene?
 *
 * By name and then by pointer: obs_scene_find_source_recursive looks inside groups as well as at
 * the top level, which obs_scene_enum_items would not, and the pointer comparison is what makes the
 * answer about THIS source rather than about anything that happens to share its name. A false
 * negative only costs a second scene item of the same source; a false positive would leave the
 * overlay not added at all, which is why the comparison is there.
 *
 * The returned item is BORROWED — obs_scene_find_source_recursive takes no reference.
 */
bool sceneContains(obs_scene_t *scene, obs_source_t *source)
{
	const char *name = obs_source_get_name(source);
	if (!name)
		return false;

	obs_sceneitem_t *item = obs_scene_find_source_recursive(scene, name);
	return item && obs_sceneitem_get_source(item) == source;
}

} // namespace

const char *sourceName()
{
	return "Xive Gift Overlay";
}

QString linkUrl(const QString &path)
{
	/*
	 * 🔴 THE SITE ORIGIN, NEVER THE API ORIGIN. They are separate deployments on separate
	 * hosts (thexive.com against api.thexive.com) and the API returns an origin-relative path
	 * precisely because it cannot know which one the caller is looking at. Composing against
	 * apiBase() would produce a source that loads a 404 and draws nothing, with no error
	 * anywhere to say why.
	 *
	 * ⚠️ `path` ARRIVES OVER THE NETWORK AND IS VALIDATED HERE. A value beginning "https://"
	 * or "//" would compose into a URL pointing at some other host entirely — a page this
	 * plugin would then burn into a live broadcast. Anything that is not a genuinely relative
	 * path is refused, and an empty return IS that refusal.
	 */
	const QString relative = path.trimmed();
	if (!relative.startsWith(QLatin1Char('/')) || relative.startsWith(QLatin1String("//")))
		return QString();

	QString origin = XiveConfig::siteBase().trimmed();
	while (origin.endsWith(QLatin1Char('/')))
		origin.chop(1);
	if (origin.isEmpty())
		return QString();

	return origin + relative;
}

Result addToCurrentScene(const QString &url)
{
	Result result;

	if (url.trimmed().isEmpty()) {
		result.message = QObject::tr("Xive did not return an overlay link.");
		return result;
	}

	/*
	 * The CANVAS, not the output. base_width/base_height is what a scene is composed against;
	 * output_width/height is the encode, which is smaller on every install that downscales — a
	 * source sized to that would cover part of the frame and look like a rendering bug.
	 */
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0) {
		result.message = QObject::tr(
			"Video output has not started yet, so the overlay could not be sized. Try "
			"again in a moment.");
		xive_warn("overlay: no video info; nothing was created");
		return result;
	}

	/* An owned reference from here down — every exit below releases it. */
	obs_source_t *sceneSource = currentEditingScene();
	if (!sceneSource) {
		result.message = QObject::tr(
			"There is no scene to add the overlay to. Add a scene first, then try "
			"again.");
		xive_warn("overlay: no current scene");
		return result;
	}

	/* BORROWED — obs_scene_from_source takes no reference; the scene is valid for as long as
	 * the source reference held above. */
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		result.message =
			QObject::tr("The current scene could not be read, so nothing was changed.");
		xive_warn("overlay: the current scene source is not a scene");
		return result;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "url", url.toUtf8().constData());
	obs_data_set_int(settings, "width", static_cast<long long>(ovi.base_width));
	obs_data_set_int(settings, "height", static_cast<long long>(ovi.base_height));

	/* Owned when non-null. */
	obs_source_t *overlay = findExistingOverlay();
	const bool reused = overlay != nullptr;

	if (reused) {
		/*
		 * 🔴 UPDATE, NEVER A SECOND SOURCE. Two browser sources on the same link both render
		 * every gift, so a creator who clicked twice would burn each celebration into the
		 * broadcast twice over — and would have no way to tell which of the two identical
		 * sources to delete. Finding the existing one is the entire reason for the search.
		 *
		 * obs_source_update carries only the three keys set above; every other
		 * browser-source setting the creator has touched is left exactly as they left it.
		 */
		obs_source_update(overlay, settings);
	} else {
		const QString name = availableSourceName();
		if (name.isEmpty()) {
			xive_warn("overlay: no free source name; nothing was created");
		} else {
			/* A new reference we own. Public, not private: it has to appear in the
			 * creator's source list and be saved with the scene collection. */
			overlay = obs_source_create(kBrowserSourceId, name.toUtf8().constData(),
						    settings, nullptr);
			if (!overlay)
				xive_error("overlay: obs_source_create(%s) failed",
					   kBrowserSourceId);
		}
	}

	obs_data_release(settings);

	if (!overlay) {
		obs_source_release(sceneSource);
		result.message = QObject::tr(
			"A browser source could not be created. Browser Source has to be installed "
			"and enabled for the gift overlay to work.");
		return result;
	}

	const bool alreadyPlaced = sceneContains(scene, overlay);
	if (!alreadyPlaced && !obs_scene_add(scene, overlay)) {
		/* A newly created source that never reached a scene is dropped here rather than
		 * left behind as an orphan in the creator's source list. */
		obs_source_release(overlay);
		obs_source_release(sceneSource);
		result.message =
			QObject::tr("The overlay could not be added to the scene, so nothing was "
				    "changed.");
		xive_error("overlay: obs_scene_add failed");
		return result;
	}
	/*
	 * ⚠️ THE SCENE ITEM obs_scene_add RETURNS IS NOT OURS TO RELEASE. It starts life at one
	 * reference and that one belongs to the scene, which drops it when the item is removed.
	 * Releasing it here would destroy the item the creator just asked for. The SOURCE is
	 * ref-counted separately and the scene took its own; ours still goes, below.
	 */

	/* Read the names while both references are still held. */
	const char *overlayNameRaw = obs_source_get_name(overlay);
	const char *sceneNameRaw = obs_source_get_name(sceneSource);
	const QString overlayName = overlayNameRaw ? QString::fromUtf8(overlayNameRaw)
						   : QString::fromUtf8(sourceName());
	const QString sceneName =
		sceneNameRaw ? QString::fromUtf8(sceneNameRaw) : QObject::tr("your scene");

	obs_source_release(overlay);
	obs_source_release(sceneSource);

	/* The link is a credential (see the header) — the fact that one arrived is logged, never
	 * the value. */
	xive_info("overlay: %s '%s' at %ux%u; %s scene '%s' (link not logged)",
		  reused ? "refreshed" : "created", qUtf8Printable(overlayName),
		  static_cast<unsigned>(ovi.base_width), static_cast<unsigned>(ovi.base_height),
		  alreadyPlaced ? "already in" : "added to", qUtf8Printable(sceneName));

	result.ok = true;
	result.reused = reused;

	if (!reused)
		result.message = QObject::tr("Added “%1” to “%2”, sized %3×%4.")
					 .arg(overlayName, sceneName)
					 .arg(static_cast<int>(ovi.base_width))
					 .arg(static_cast<int>(ovi.base_height));
	else if (alreadyPlaced)
		result.message = QObject::tr("Your gift overlay is already in “%1”. Its link has "
					     "been refreshed.")
					 .arg(sceneName);
	else
		result.message = QObject::tr("You already had a gift overlay, so it was refreshed "
					     "and added to “%1” rather than duplicated.")
					 .arg(sceneName);

	return result;
}

} // namespace XiveOverlaySource
