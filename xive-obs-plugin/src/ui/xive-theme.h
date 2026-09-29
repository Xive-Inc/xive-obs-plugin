#pragma once

/*
 * Xive look for the OBS docks — the web Creator Studio popout, rebuilt in Qt.
 *
 * Every value here is lifted from the web app rather than invented, so the docks and a popout
 * browser source sitting next to them read as the same product:
 *
 *   src/styles/theme.css          --image-stream-brand-gradient, --color-stream-live
 *   src/styles/stream-studio.css  .stream-studio-popout* (shell, header, card, input, buttons)
 *
 * WHAT DOES NOT CARRY OVER. Qt Style Sheets are a subset of CSS2: no box-shadow, no text-transform,
 * no letter-spacing. The popout's glow and its uppercased header are done the Qt way instead —
 * shadows dropped, labels written in caps at the call site. Anything else that has no QSS equivalent
 * is simply left out rather than approximated badly.
 */

#include <QString>
#include <QWidget>

class QFrame;
class QLabel;

namespace XiveTheme {

/* ---- palette (dark; OBS has no light mode worth matching) ---- */

/** Popout shell background — `.stream-studio-popout`. */
inline constexpr const char *kBg = "#0a0a0f";
/** Card/panel fill — `.stream-studio-popout-poll`. */
inline constexpr const char *kSurface = "rgba(18, 18, 27, 0.92)";
/** Hairline used for every border and rule in the popout. */
inline constexpr const char *kBorder = "rgba(255, 255, 255, 0.08)";
inline constexpr const char *kBorderStrong = "rgba(255, 255, 255, 0.16)";
/** `--studio-text` / `--studio-text-muted`. */
inline constexpr const char *kInk = "#f4f4f5";
inline constexpr const char *kInkSoft = "#e4e4e7";
inline constexpr const char *kInkMuted = "#a1a1aa";
inline constexpr const char *kInkFaint = "#71717a";
/** Brand purple, used for focus rings — matches the popout input's focus border. */
inline constexpr const char *kFocus = "rgba(162, 75, 255, 0.55)";
/** The live dot in `.stream-studio-popout-dot--live`. */
inline constexpr const char *kLive = "#f87171";
inline constexpr const char *kDanger = "#ff315e";
inline constexpr const char *kSuccess = "#24e28a";
inline constexpr const char *kAccentGold = "#ffd166";

/**
 * The whole dock stylesheet. Applied once to a dock root; Qt cascades it to the children.
 *
 * Widgets opt into the two button weights by object name — `xivePrimary` for the brand-gradient
 * call to action, `xiveSecondary` (or nothing) for the quiet ones — because QSS has no class
 * attribute to select on.
 */
QString styleSheet();

/** Paint a dock root: object name, stylesheet, and the opaque background OBS docks need. */
void applyDockTheme(QWidget *dock);

/**
 * The popout's header row: an uppercase title, then a status dot and word pushed to the right.
 * Returns the row; `dotOut`/`stateOut` hand back the two live-updating pieces so a dock can call
 * `setLiveState()` on them as its channel changes.
 *
 * `metaOut`, when asked for, adds a muted label between the title and the dot — the studio rail's
 * "1,234 watching". It starts empty and hidden; give it text to show it.
 */
QWidget *makeHeader(QWidget *parent, const QString &title, QLabel **dotOut, QLabel **stateOut,
		    QLabel **metaOut = nullptr);

/** Drive a header built by `makeHeader`: red dot + "LIVE", or grey + "OFFLINE". */
void setLiveState(QLabel *dot, QLabel *state, bool live);

/** A rounded, hairline-bordered panel — the popout card. Children go in a layout on the frame. */
QFrame *makeCard(QWidget *parent);

/** Muted/danger colouring for the status lines every dock keeps at the bottom. */
QString noticeCss(bool isError);

/**
 * Stylesheet for a popup menu — the chat dock's moderation menu.
 *
 * Its own sheet rather than a rule inside `styleSheet()`: a QMenu opened from a dock is a separate
 * top-level window, so it does not inherit the dock's stylesheet and would otherwise appear in the
 * desktop theme, white-on-white against these docks.
 */
QString menuStyleSheet();

/**
 * Stylesheet for a completer popup — the category picker's suggestion list.
 *
 * Separate from `styleSheet()` for the same reason `menuStyleSheet()` is: the popup a QCompleter
 * shows is a top-level window, not a child of the dock, so the dock's sheet never reaches it and
 * it would otherwise open in the desktop theme.
 */
QString popupListStyleSheet();

/** Section heading inside a card — small, uppercase, muted. Text is uppercased by the caller. */
QLabel *makeSectionLabel(QWidget *parent, const QString &text);

/**
 * Document-level CSS for the QTextBrowser feeds (chat, activity, gifts).
 *
 * Rich text in Qt does NOT inherit the widget stylesheet, so the feeds need their own copy of the
 * type ramp or they render as black-on-white paragraphs inside a dark dock.
 */
QString documentStyleSheet();

/**
 * `documentStyleSheet()` plus the classes a chat message is built from — the roles, the badge and
 * its numeral chip, the gift and poll cards. Kept apart from the base sheet because only the chat
 * dock draws them.
 *
 * Everything that varies per message (a name's colour, a badge's palette) stays inline at the call
 * site; everything shared lives here, so the message HTML reads as structure rather than styling.
 */
QString chatDocumentStyleSheet();

} // namespace XiveTheme
