#pragma once

#include <QString>
#include <QStringList>

/*
 * Quick actions ride the chat channel as an event bus.
 *
 * A poll and an ad break are not API calls — they are specially-formatted chat messages that every
 * Xive client recognises. The wire format is defined in
 * packages/core/src/utils/streamChatQuickActions.ts and mirrored here EXACTLY: a mismatched prefix
 * would post visible gibberish into a live broadcast's chat rather than fail cleanly.
 *
 *   poll      XIVE_POLL:{"question":"…","options":["…","…"]}
 *   poll vote XIVE_POLL_VOTE:<pollMessageId>:<optionIndex>
 *   ad break  XIVE_AD_BREAK
 *
 * ── Not implemented, deliberately ────────────────────────────────────────────────────────────
 *
 * "Run giveaway" is a placeholder on the web too: StudioGiveawayModal.tsx renders a permanently
 * DISABLED button titled "Server-side winner selection is required (giveaway backend not
 * available)", and its own copy says Xive does not run client-side giveaways in production. There
 * is no protocol and no endpoint to port, so this plugin does not offer the action rather than
 * shipping a button that cannot work.
 *
 * "Raid channel" is excluded on Brett's instruction — the platform does not support raids. (It also
 * ends the broadcast server-side, so it is not a safe one-click dock action.)
 */
namespace XiveQuickActions {

/* Chat messages have a ~500 character limit, which a long poll can exceed. */
constexpr int kChatMessageLimit = 500;

/* Matches the web: at least 2 options, at most 8. */
constexpr int kMinPollOptions = 2;
constexpr int kMaxPollOptions = 8;

/* The literal ad-break marker. */
QString adBreakMessage();

/*
 * `XIVE_POLL:{json}` for the given question and options. Blank options are dropped, mirroring
 * buildPollMessage(). Returns an empty string when the result would be unusable — fewer than two
 * options, no question, or over the chat length limit.
 */
QString buildPollMessage(const QString &question, const QStringList &options);

/* Why buildPollMessage() would refuse, as something a person can read. Empty when it is valid. */
QString pollValidationError(const QString &question, const QStringList &options);

/*
 * ── Reading the same wire format back ────────────────────────────────────────────────────────
 *
 * Every client sees these markers in the message list, so a client that only knows how to WRITE
 * them shows the raw `XIVE_POLL_VOTE:…` text to the broadcaster while the web quietly renders a
 * poll card. These are the mirror of parsePollMessage() / parsePollVote() / isAdBreakMessage(),
 * matched at the same strictness — a marker this rejects falls through and renders as ordinary
 * chat, which is also what the web does.
 */

struct Poll {
	QString question;
	QStringList options;
};

/* `XIVE_POLL:{json}`. False unless the payload has a question and at least two options; the
 * options are capped at kMaxPollOptions, as parsePollMessage() caps them. */
bool parsePoll(const QString &content, Poll *out);

/* `XIVE_POLL_VOTE:<pollMessageId>:<optionIndex>`. The id may itself contain colons, so the split
 * is on the LAST one. */
bool parsePollVote(const QString &content, QString *pollMessageId, int *optionIndex);

/* The literal ad-break marker, trimmed — nothing else counts. */
bool isAdBreak(const QString &content);

} // namespace XiveQuickActions
