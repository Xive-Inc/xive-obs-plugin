#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

/*
 * Drawing chat the way the website draws it.
 *
 * All of it is pure: a message in, HTML out, no widget and no network. That is on purpose — the
 * dock is then only wiring, this can be unit-tested from a headless binary (scripts/run-tests.sh),
 * and scripts/preview-chat.cpp can render the result to a PNG so a change to the look can be
 * LOOKED at without a running OBS and a live broadcast to talk into.
 *
 * Ported value for value from:
 *   packages/core/src/utils/chatIdentityDisplay.ts   name colour, badge chips
 *   packages/core/src/utils/giftEmoji.ts             gift text parsing, gift emoji
 *
 * ── Why every colour here is an opaque hex ───────────────────────────────────────────────────
 *
 * Tailwind 4 writes these as an oklch token at a fractional alpha (`bg-purple-500/25`). Qt's rich
 * text is an HTML 3.2 subset with a CSS2 colour parser: it knows neither oklch nor a reliable
 * alpha on a block background. Each value below is therefore that same colour converted to sRGB
 * and pre-composited over the panel fill it sits on (#12121b, the chat feed's surface) — which is
 * the pixel the browser ends up painting anyway. Change the surface and these need recomputing.
 */
namespace XiveChatRender {

/**
 * The colour a display name is drawn in — `resolveChatNameStyle()`.
 *
 * A valid `#rrggbb` in `chat_name_color` wins outright. Otherwise the name takes one of eight
 * colours chosen by hashing the user id, so a given person is the same colour in the dock as on
 * the site.
 *
 * ⚠️ BOTH ARMS ARE LIVE (corrected 2026-08-19). `chat_name_color` USED to be null on every message
 * the platform served, because the server's identity enrichment was gated off for parity with a
 * Deno original that silently threw. Deno is retired and `ChatIdentity::ENRICH_CHAT_IDENTITY` is
 * on, so a viewer who has set a colour now arrives with one. The hash is the fallback for everyone
 * who has not — which is still most people, so it is not a dead path either.
 */
QString nameColour(const QString &userId, const QString &chatNameColor);

struct Gift {
	QString sender;
	QString giftName;
	qint64 credits = 0;
	/* Only present when the backend appended a running total to the message. */
	qint64 streamTotalCredits = 0;
	bool hasStreamTotal = false;
};

/**
 * `parseGiftMessage()` — "Mia sent Hype (10 credits)", with or without the
 * " · Stream total: N credits" suffix. False when the text is not a gift.
 */
bool parseGift(const QString &content, Gift *out);

/**
 * `giftEmoji()` — the catalog map, then the keyword fallbacks, then the generic present.
 *
 * The web draws the gift's animated GIF here and falls back to this emoji when it has no URL. A
 * QTextBrowser cannot play a remote GIF without hand-driving a QMovie per message, so the dock
 * always takes the fallback.
 */
QString giftEmoji(const QString &giftName);

/** A rendered badge — what `ChatIdentityBadge` draws for `chat_badge`. */
struct Badge {
	/** The glyph when `icon`, otherwise the numeral drawn inside a coloured chip. */
	QString label;
	QString fg;
	QString bg;
	QString border;
	QString title;
	/**
	 * True for the badges the web draws as an icon — the mark is the badge, so it renders as a
	 * bare glyph in `fg` with no background run. False only for `xive500` and `level_N`, where
	 * the NUMBER is the badge and no glyph can carry it; those keep a chip.
	 */
	bool icon = false;
};

/**
 * `badgeVisual()`. Recognised ids draw their own mark and IGNORE `label` — the web's
 * `visual.title ?? label` means the catalog wins, and `chat_badge_label` ("Verified", "Creator")
 * is the tooltip word, not the drawn text. `label` is consulted only when the catalog left the
 * title empty. False when there is no badge id to draw.
 */
bool badgeVisual(const QString &badgeId, const QString &label, Badge *out);

/**
 * One message as the HTML a QTextBrowser will lay out — or an empty string when the message must
 * not appear at all, which is exactly the poll votes StreamChatPanel renders as an empty fragment.
 *
 * `all` is the whole retained buffer. It is needed only to count the votes standing against a poll
 * card; every other message ignores it.
 *
 * The six shapes, in the order they are tested — the same order the web tests them, which matters
 * because a gift's own text would otherwise match a later branch, and because a platform notice
 * must never be parsed as any of them at all:
 *
 *   system      card: ⓘ, "XIVE", the platform's own text          (is_system, never parsed)
 *   poll vote   nothing
 *   gift        card: emoji, "sender sent Gift", credits
 *   poll        card: question, tally, one row per option        (creator's messages only)
 *   ad break    amber pill                                        (creator's messages only)
 *   anything    HH:MM · name in its own colour · badge · Host / Mod / Priority · text
 */
/**
 * `contentMentionsUser()` — is this line addressed to `username`?
 *
 * Decided per viewer at RENDER time and never sent down as a flag: every viewer receives the same
 * chat rows over the same socket, so the answer has to be computed locally or everybody would see
 * everybody else's highlight.
 *
 * A leading "@" on either side is ignored, matching is case-insensitive, and the name must not be
 * bounded by a word character — so "@ada" matches "hey @ada!" and does NOT match "@adamant".
 * Ported value for value from useStreamViewerRoster.ts; the regex there is the specification.
 */
bool mentionsUser(const QString &content, const QString &username);

/**
 * One message as HTML.
 *
 * `selfUsername` is the handle of whoever is signed in — the CREATOR, in this dock — and lights up
 * lines addressed to them. Empty disables the highlight entirely, which is what the preview and the
 * tests want. Never applied to your own messages: mentioning yourself should not light up your chat.
 */
QString messageHtml(const QJsonObject &message, const QList<QJsonObject> &all,
		    const QString &selfUsername = QString());

/**
 * The placeholder shown when the feed is empty — the studio rail's "be the first to say hello".
 * Here rather than in the dock so the preview renders the same empty state the dock does.
 */
QString emptyStateHtml();

} // namespace XiveChatRender
