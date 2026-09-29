#pragma once

#include <QString>

class QWidget;

/*
 * The Xive docks, drawn by the website instead of by Qt.
 *
 * ⚠️ THIS IS THE SAME UI THE BROWSER SHOWS, NOT A COPY OF IT. Each dock is a CEF panel pointed at
 * one of the studio's popout routes — `/popout/chat`, `/popout/activity`, `/popout/actions`,
 * `/popout/gifts` — which src/components/streaming/studio/StudioPopout.tsx was written for and
 * says so in its own comments. A creator's dock and their studio tab therefore cannot drift: there
 * is one implementation of a chat row, one gift leaderboard, one set of quick actions, and it
 * lives on the web.
 *
 * WHY THIS REPLACED THE HAND-DRAWN DOCKS. Qt Style Sheets are a subset of CSS2 — no box-shadow,
 * no letter-spacing, no gradients on text — so a Qt rebuild could only ever approach the site's
 * look, never reach it. It also had to re-implement every feature: the native quick-actions card
 * offered 2 of the web's 10 actions, because clips, markers, goals, battles, co-host invites and
 * giveaways would each have needed their own C++ port. A panel gets all ten the day the web ships
 * them.
 *
 * ── HOW IT SIGNS IN ──────────────────────────────────────────────────────────────────────────
 *
 * It does not, by itself: the creator signs in INSIDE the dock, once, exactly as the popout's own
 * copy already tells them to ("In an OBS dock, sign in inside the dock window — it keeps its own
 * session"). All four panels share one persistent CEF store, so one sign-in covers all of them and
 * survives restarts.
 *
 * The plugin's own OAuth session is deliberately NOT forged into the panel's localStorage. The web
 * session object (`xive-session`, see packages/core/src/utils/sessionManager.ts) carries a profile
 * id, admin flags and a database-authorisation result that AuthContext_v2 re-validates on load;
 * hand-building one in C++ would be a second, silently diverging definition of what a signed-in
 * Xive user is. Two sign-ins is the cheaper honesty.
 *
 * ── WHERE IT DOES NOT WORK ───────────────────────────────────────────────────────────────────
 *
 * CEF panels need the obs-browser module, and obs-browser refuses to create them on Wayland (see
 * `get_browser_lib()` in browser-panel.hpp). `available()` reports that up front so the caller can
 * fall back rather than register four blank docks.
 */
namespace XiveBrowserDock {

/**
 * Is obs-browser present and willing to give us panels?
 *
 * First call initialises CEF, which can take a moment; every call after that is cached. Must be
 * called from the UI thread.
 */
bool available();

/** Why `available()` said no, as something a person can read. Empty when it said yes. */
QString unavailableReason();

/**
 * A dock for one popout kind — `chat`, `activity`, `actions`, `gifts`, `multichat`.
 *
 * Returns null when `available()` is false. The widget is parentless; the caller hands it to
 * `obs_frontend_add_dock_by_id`, which takes ownership.
 *
 * The URL is the parameterless form (`/popout/chat`, no stream id) on purpose — see the warning at
 * the top of src/utils/streaming/openStudioPopout.ts. It resolves whatever is on air each time it
 * loads, so a dock placed today still works on next month's broadcast.
 */
QWidget *create(const QString &kind);

/**
 * Close every panel's browser. Call once, from OBS_FRONTEND_EVENT_EXIT, before anything else.
 *
 * ⚠️ OMITTING THIS CRASHES OBS ON EVERY SHUTDOWN. Not a leak — a hard abort:
 *
 *     Unhandled exception: 80000003          (STATUS_BREAKPOINT — a Chromium CHECK)
 *     Thread CrBrowserMain (Crashed)          libcef.dll
 *     Thread (main)                           obs-browser!obs_module_unload → Thrd_join
 *                                             obs!free_module → obs_shutdown
 *
 * CEF asserts at shutdown that every browser has been closed. Our panels are inside docks that
 * teardownUi() deliberately never removes — that is what keeps a creator's layout across restarts,
 * see the long note there — so OBS destroys them with the main window, which is LATER than
 * obs_shutdown() unloading obs-browser. By then CEF is tearing down with four live browsers and
 * aborts. Qt widgets tolerate that ordering; CEF ones do not.
 *
 * So the docks still stay attached, and only the browsers inside them are closed. The layout is
 * saved from the QDockWidgets, which are untouched.
 */
void shutdown();

} // namespace XiveBrowserDock
