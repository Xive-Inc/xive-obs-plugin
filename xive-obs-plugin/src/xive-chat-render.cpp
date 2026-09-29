#include "xive-chat-render.h"

#include "xive-quick-actions.h"

#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QObject>
#include <QRegularExpression>
#include <QUrl>

namespace XiveChatRender {

namespace {

/*
 * The eight fallback name colours, in the order chatIdentityDisplay.ts lists them — the hash
 * indexes this array, so reordering it would repaint every name in chat.
 *
 * Tailwind's `text-fuchsia-400` and friends, resolved from oklch to sRGB.
 */
const char *const kNameColours[] = {
	"#ed6aff", /* fuchsia-400 */
	"#00d492", /* emerald-400 */
	"#ff637e", /* rose-400    */
	"#00d3f2", /* cyan-400    */
	"#ffb900", /* amber-400   */
	"#a684ff", /* violet-400  */
	"#ff8904", /* orange-400  */
	"#00bcff", /* sky-400     */
};
constexpr int kNameColourCount = int(sizeof(kNameColours) / sizeof(kNameColours[0]));

/*
 * Gift messages are recognised by their rendered text. The middle dot in the "with total" form is
 * U+00B7.
 */
const QRegularExpression &giftWithTotalPattern()
{
	static const QRegularExpression re(
		QStringLiteral("^(.+?)\\s+sent\\s+(.+?)\\s+\\((\\d+)\\s*credits\\)\\s+·\\s+"
			       "Stream total:\\s+(\\d+)\\s+credits$"));
	return re;
}

const QRegularExpression &giftPattern()
{
	static const QRegularExpression re(
		QStringLiteral("^(.+?)\\s+sent\\s+(.+?)\\s+\\((\\d+)\\s*credits\\)$"));
	return re;
}

/* GIFT_EMOJI_MAP, verbatim. Exact, case-sensitive lookup, exactly as the object index is. */
const QHash<QString, QString> &giftEmojiMap()
{
	static const QHash<QString, QString> map = {
		/* Automated tier catalog (2025+ default) */
		{QStringLiteral("Nudge"), QStringLiteral("👋")},
		{QStringLiteral("Thanks"), QStringLiteral("🙏")},
		{QStringLiteral("Hype"), QStringLiteral("🔥")},
		{QStringLiteral("Big love"), QStringLiteral("💜")},
		{QStringLiteral("Shoutout"), QStringLiteral("📣")},
		{QStringLiteral("Producer"), QStringLiteral("🎬")},
		{QStringLiteral("Patron"), QStringLiteral("⭐")},
		/* v2 animated catalog */
		{QStringLiteral("Pocket tap"), QStringLiteral("👋")},
		{QStringLiteral("Spark toss"), QStringLiteral("✨")},
		{QStringLiteral("Tiny cheer"), QStringLiteral("🎉")},
		{QStringLiteral("Thanks burst"), QStringLiteral("🙏")},
		{QStringLiteral("Wave hello"), QStringLiteral("👋")},
		{QStringLiteral("Encore snap"), QStringLiteral("👏")},
		{QStringLiteral("Boom clap"), QStringLiteral("👏")},
		{QStringLiteral("Heart bloom"), QStringLiteral("❤️")},
		{QStringLiteral("Laugh beam"), QStringLiteral("😂")},
		{QStringLiteral("Star sprinkle"), QStringLiteral("⭐")},
		{QStringLiteral("Beat drop"), QStringLiteral("🎵")},
		{QStringLiteral("Glow stick"), QStringLiteral("🌟")},
		{QStringLiteral("Mic shimmy"), QStringLiteral("🎤")},
		{QStringLiteral("Confetti pop"), QStringLiteral("🎊")},
		{QStringLiteral("Spotlight"), QStringLiteral("🔦")},
		{QStringLiteral("Hype train"), QStringLiteral("🚂")},
		{QStringLiteral("Trophy toss"), QStringLiteral("🏆")},
		{QStringLiteral("Rainbow arc"), QStringLiteral("🌈")},
		{QStringLiteral("Laser sketch"), QStringLiteral("⚡")},
		{QStringLiteral("Bass boost"), QStringLiteral("🔊")},
		{QStringLiteral("Crystal burst"), QStringLiteral("💎")},
		{QStringLiteral("Neon pulse"), QStringLiteral("💜")},
		{QStringLiteral("Phoenix flare"), QStringLiteral("🔥")},
		{QStringLiteral("Aurora ribbon"), QStringLiteral("🌌")},
		{QStringLiteral("Comet trail"), QStringLiteral("☄️")},
		{QStringLiteral("Galaxy spin"), QStringLiteral("🌌")},
		{QStringLiteral("Thunder cheer"), QStringLiteral("⚡")},
		{QStringLiteral("Meteor shower"), QStringLiteral("🌠")},
		{QStringLiteral("Solar crown"), QStringLiteral("👑")},
		{QStringLiteral("Eclipse beam"), QStringLiteral("🌑")},
		{QStringLiteral("Nova bloom"), QStringLiteral("💫")},
		{QStringLiteral("Quasar wave"), QStringLiteral("🌊")},
		{QStringLiteral("Stellar bow"), QStringLiteral("🙇")},
		{QStringLiteral("Cosmic salute"), QStringLiteral("🫡")},
		{QStringLiteral("Orbit hug"), QStringLiteral("🤗")},
		{QStringLiteral("Rocket heart"), QStringLiteral("🚀")},
		{QStringLiteral("Supernova wink"), QStringLiteral("😉")},
		{QStringLiteral("Nebula bow"), QStringLiteral("🌌")},
		{QStringLiteral("Pulsar cheer"), QStringLiteral("📡")},
		{QStringLiteral("Starfall"), QStringLiteral("⭐")},
		/* Wave 3 animated gifts */
		{QStringLiteral("Coffee boost"), QStringLiteral("☕")},
		{QStringLiteral("Fist bump"), QStringLiteral("👊")},
		{QStringLiteral("Nice one"), QStringLiteral("👍")},
		{QStringLiteral("Party pop"), QStringLiteral("🎉")},
		{QStringLiteral("DJ drop"), QStringLiteral("🎧")},
		{QStringLiteral("Wiggle wave"), QStringLiteral("👋")},
		{QStringLiteral("Fire streak"), QStringLiteral("🔥")},
		{QStringLiteral("Gold mic"), QStringLiteral("🎤")},
		{QStringLiteral("Fireworks"), QStringLiteral("🎆")},
		{QStringLiteral("Curtain call"), QStringLiteral("🎭")},
		{QStringLiteral("Main stage"), QStringLiteral("🎤")},
		{QStringLiteral("Diamond mic"), QStringLiteral("💎")},
		{QStringLiteral("Spotlight encore"), QStringLiteral("🔦")},
		{QStringLiteral("Galaxy gift"), QStringLiteral("🌌")},
		{QStringLiteral("Creator legend"), QStringLiteral("🏆")},
		{QStringLiteral("Epic finale"), QStringLiteral("✨")},
		{QStringLiteral("Cosmic crown"), QStringLiteral("👑")},
		/* Legacy catalog */
		{QStringLiteral("Rose"), QStringLiteral("🌹")},
		{QStringLiteral("Thumbs Up"), QStringLiteral("👍")},
		{QStringLiteral("Star"), QStringLiteral("⭐")},
		{QStringLiteral("Heart"), QStringLiteral("❤️")},
		{QStringLiteral("Clap"), QStringLiteral("👏")},
		{QStringLiteral("Fire"), QStringLiteral("🔥")},
		{QStringLiteral("Smile"), QStringLiteral("😊")},
		{QStringLiteral("Gem"), QStringLiteral("💎")},
		{QStringLiteral("Crown"), QStringLiteral("👑")},
		{QStringLiteral("Diamond"), QStringLiteral("💎")},
		{QStringLiteral("Rocket"), QStringLiteral("🚀")},
		{QStringLiteral("Trophy"), QStringLiteral("🏆")},
		{QStringLiteral("Medal"), QStringLiteral("🏅")},
		{QStringLiteral("Gift Box"), QStringLiteral("🎁")},
		{QStringLiteral("Rainbow"), QStringLiteral("🌈")},
		{QStringLiteral("Dragon"), QStringLiteral("🐉")},
		{QStringLiteral("Phoenix"), QStringLiteral("🔥")},
		{QStringLiteral("Unicorn"), QStringLiteral("🦄")},
		{QStringLiteral("Castle"), QStringLiteral("🏰")},
		{QStringLiteral("Spaceship"), QStringLiteral("🚀")},
		{QStringLiteral("Super Star"), QStringLiteral("⭐")},
		{QStringLiteral("Galaxy"), QStringLiteral("🌌")},
		{QStringLiteral("Golden Crown"), QStringLiteral("👑")},
		{QStringLiteral("Diamond Ring"), QStringLiteral("💍")},
		{QStringLiteral("Treasure Chest"), QStringLiteral("📦")},
		{QStringLiteral("Mega Heart"), QStringLiteral("❤️")},
		{QStringLiteral("Legendary Sword"), QStringLiteral("⚔️")},
		{QStringLiteral("Royal Crown"), QStringLiteral("👑")},
		{QStringLiteral("Infinity"), QStringLiteral("♾️")},
		{QStringLiteral("Cosmic"), QStringLiteral("🌌")},
		{QStringLiteral("Ultimate Gift"), QStringLiteral("🎁")},
		{QStringLiteral("Platinum"), QStringLiteral("💎")},
		{QStringLiteral("Supreme"), QStringLiteral("👑")},
		{QStringLiteral("Spark"), QStringLiteral("✨")},
		{QStringLiteral("Wave"), QStringLiteral("🌊")},
		{QStringLiteral("Sun"), QStringLiteral("☀️")},
		{QStringLiteral("Moon"), QStringLiteral("🌙")},
		{QStringLiteral("Comet"), QStringLiteral("☄️")},
		{QStringLiteral("Aurora"), QStringLiteral("🌅")},
		{QStringLiteral("Storm"), QStringLiteral("⛈️")},
		{QStringLiteral("Meteor"), QStringLiteral("☄️")},
		{QStringLiteral("Nebula"), QStringLiteral("🌌")},
		{QStringLiteral("Supernova"), QStringLiteral("💫")},
		{QStringLiteral("Black Hole"), QStringLiteral("🕳️")},
		{QStringLiteral("Empire"), QStringLiteral("🏛️")},
		{QStringLiteral("Legacy"), QStringLiteral("📜")},
		{QStringLiteral("Eternal"), QStringLiteral("♾️")},
		{QStringLiteral("Champion"), QStringLiteral("🏆")},
	};
	return map;
}

} // namespace

QString nameColour(const QString &userId, const QString &chatNameColor)
{
	static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));

	const QString trimmed = chatNameColor.trimmed();
	if (!trimmed.isEmpty() && hex.match(trimmed).hasMatch())
		return trimmed.toLower();

	/*
	 * hashFallbackNameColorClass(). JavaScript does `h * 31` in a double and only then truncates
	 * with `>>> 0`; the intermediate never exceeds 2^37, which a double holds exactly, so plain
	 * wrapping 32-bit arithmetic gives the identical index. quint32 to make the wrap defined —
	 * signed overflow would be UB.
	 */
	quint32 h = 0;
	for (const QChar c : userId)
		h = h * 31u + c.unicode();

	return QString::fromUtf8(kNameColours[h % quint32(kNameColourCount)]);
}

bool parseGift(const QString &content, Gift *out)
{
	if (!out || content.isEmpty())
		return false;

	const QString text = content.trimmed();

	auto m = giftWithTotalPattern().match(text);
	if (m.hasMatch()) {
		out->sender = m.captured(1).trimmed();
		out->giftName = m.captured(2).trimmed();
		out->credits = m.captured(3).toLongLong();
		out->streamTotalCredits = m.captured(4).toLongLong();
		out->hasStreamTotal = true;
		return true;
	}

	m = giftPattern().match(text);
	if (!m.hasMatch())
		return false;

	out->sender = m.captured(1).trimmed();
	out->giftName = m.captured(2).trimmed();
	out->credits = m.captured(3).toLongLong();
	out->streamTotalCredits = 0;
	out->hasStreamTotal = false;
	return true;
}

QString giftEmoji(const QString &giftName)
{
	const auto &map = giftEmojiMap();
	const auto it = map.constFind(giftName);
	if (it != map.constEnd())
		return it.value();

	/* The keyword fallbacks, in the order giftEmoji() tests them — first match wins. */
	const QString lower = giftName.toLower();
	if (lower.startsWith(QStringLiteral("pro:")))
		return QStringLiteral("👑");
	if (lower.contains(QStringLiteral("heart")))
		return QStringLiteral("❤️");
	if (lower.contains(QStringLiteral("star")) || lower.contains(QStringLiteral("nova")) ||
	    lower.contains(QStringLiteral("stellar")))
		return QStringLiteral("⭐");
	if (lower.contains(QStringLiteral("crown")) || lower.contains(QStringLiteral("royal")) ||
	    lower.contains(QStringLiteral("imperial")))
		return QStringLiteral("👑");
	if (lower.contains(QStringLiteral("fire")) || lower.contains(QStringLiteral("phoenix")) ||
	    lower.contains(QStringLiteral("flare")))
		return QStringLiteral("🔥");
	if (lower.contains(QStringLiteral("confetti")) || lower.contains(QStringLiteral("cheer")))
		return QStringLiteral("🎉");
	return QStringLiteral("🎁");
}

bool badgeVisual(const QString &badgeId, const QString &label, Badge *out)
{
	if (!out)
		return false;

	/*
	 * Badges are MARKS, not words. A row of pills reading "Verified" / "Creator" beside every
	 * name eats the width the message needs, and at chat density the reader is scanning for a
	 * shape. So each badge is a glyph in its own colour and the noun moves to the tooltip.
	 *
	 * The glyphs mirror the icons ChatIdentityBadge draws on the web (Sparkles, Clapperboard,
	 * Star, Trophy) and the icons the badge catalog itself ships in `availableBadges`. They are
	 * deliberately glyphs the dock already depends on elsewhere — giftEmoji() renders 🎁🔥⭐👑🏆
	 * into this same QTextBrowser — rather than SVG, which Qt rich text cannot inline.
	 *
	 * The web's real verified mark is a multi-colour SVG. A QTextBrowser cannot lay that out
	 * inline, so verified takes the sky-blue check: the same colour, the same meaning, the
	 * closest thing this surface can draw.
	 */
	Badge b;
	b.fg = QStringLiteral("#e4e4e7");     /* zinc-200 */
	b.bg = QStringLiteral("#282830");     /* zinc-700 @ 50% */
	b.border = QStringLiteral("#383841"); /* zinc-500 @ 40% */

	/* An empty or "none" badge draws nothing, whatever the label says — the same early return
	 * ChatIdentityBadge makes on a falsy `badgeId`. */
	if (badgeId.isEmpty() || badgeId == QStringLiteral("none"))
		return false;

	if (badgeId == QStringLiteral("pro")) {
		/* The web paints a pink→purple→blue gradient behind Sparkles; Qt rich text has no
		 * gradient, so the glyph takes the middle stop. */
		b.label = QStringLiteral("✦");
		b.icon = true;
		b.fg = QStringLiteral("#d8b4fe"); /* purple-300 */
		b.title = QStringLiteral("Xive Pro");
	} else if (badgeId == QStringLiteral("xive500")) {
		/* Numeral, not glyph: nothing draws "500". Keeps its chip. */
		b.label = QStringLiteral("500");
		b.fg = QStringLiteral("#fef3c6");     /* amber-100          */
		b.bg = QStringLiteral("#593b13");     /* amber-500 @ 30%    */
		b.border = QStringLiteral("#88660e"); /* amber-400 @ 50%    */
		b.title = QStringLiteral("Xive 500 founding member");
	} else if (badgeId == QStringLiteral("verified")) {
		b.label = QStringLiteral("✓");
		b.icon = true;
		b.fg = QStringLiteral("#74d4ff"); /* sky-300 */
		b.title = QStringLiteral("Verified");
	} else if (badgeId == QStringLiteral("creator")) {
		b.label = QStringLiteral("🎬");
		b.icon = true;
		b.fg = QStringLiteral("#ffd230"); /* amber-300 */
		b.title = QStringLiteral("Creator");
	} else if (badgeId == QStringLiteral("subscriber")) {
		b.label = QStringLiteral("★");
		b.icon = true;
		b.fg = QStringLiteral("#5ee9b5"); /* emerald-300 */
		b.title = QStringLiteral("Subscriber");
	} else if (badgeId == QStringLiteral("staff_ceo")) {
		/*
		 * CEO, EXEC, DEV, STAFF — the four platform-identity badges, and the loudest marks in
		 * the set. A staff mark is a claim about who is speaking FOR THE PLATFORM, so each has
		 * to be impossible to mistake for a subscriber or a level chip at chat density, and
		 * they have to be told apart at a glance: CEO fuchsia, Exec gold, Dev red, Staff cyan.
		 *
		 * 🔴 THEY KEEP THEIR CHIPS AND THEIR WORDS, unlike pro / verified / creator above.
		 * Those three take a bare glyph because the mark IS the badge and the web draws the
		 * same mark. There is no glyph for "this person works at Xive" — a lone crown reads as
		 * a gift or a level to anyone who has not been told otherwise — so these render the
		 * web's actual label ("CEO", "Exec", "Dev", "Staff") inside a coloured chip, which is
		 * exactly what badgeVisual() in packages/core/src/utils/chatIdentityDisplay.ts does.
		 *
		 * ⚠️ NONE OF THEM CAN BE SELF-GRANTED, so drawing them at face value is safe: the badge
		 * catalog only offers a staff tier to a caller whose own profile carries admin_role /
		 * is_admin, exactly one tier is offered per account, and the save endpoint refuses any
		 * badge that caller's catalog did not unlock (403 "Badge not unlocked"). See
		 * api/app/streaming/chat_identity_routes.php.
		 */
		b.label = QStringLiteral("CEO");
		b.fg = QStringLiteral("#f9e0ff");     /* fuchsia-100        */
		b.bg = QStringLiteral("#3a1a44");     /* fuchsia-500 @ 30%  */
		b.border = QStringLiteral("#7d4d92"); /* fuchsia-300 @ 50%  */
		b.title = QStringLiteral("Xive CEO");
	} else if (badgeId == QStringLiteral("staff_exec")) {
		b.label = QStringLiteral("Exec");
		b.fg = QStringLiteral("#fef3c6");     /* amber-100          */
		b.bg = QStringLiteral("#3f321a");     /* amber-400 @ 30%    */
		b.border = QStringLiteral("#8a7237"); /* amber-300 @ 50%    */
		b.title = QStringLiteral("Xive Exec");
	} else if (badgeId == QStringLiteral("staff_dev")) {
		b.label = QStringLiteral("Dev");
		b.fg = QStringLiteral("#ffc9c9");     /* red-200            */
		b.bg = QStringLiteral("#331a1e");     /* red-500 @ 20%      */
		b.border = QStringLiteral("#7a3238"); /* red-400 @ 45%      */
		b.title = QStringLiteral("Xive Dev");
	} else if (badgeId == QStringLiteral("staff")) {
		b.label = QStringLiteral("Staff");
		b.fg = QStringLiteral("#a2f4fd");     /* cyan-200           */
		b.bg = QStringLiteral("#12303a");     /* cyan-500 @ 20%     */
		b.border = QStringLiteral("#1f5f74"); /* cyan-400 @ 45%     */
		b.title = QStringLiteral("Xive Staff");
	} else if (badgeId.startsWith(QStringLiteral("level_"))) {
		/* Numeral, not glyph — the level IS the number. */
		b.label = badgeId.mid(6);
		b.fg = QStringLiteral("#ddd6ff");     /* violet-200         */
		b.bg = QStringLiteral("#2b1f49");     /* violet-500 @ 20%   */
		b.border = QStringLiteral("#463a6b"); /* violet-400 @ 35%   */
		b.title = QStringLiteral("Level badge");
	} else if (badgeId.startsWith(QStringLiteral("achievement:"))) {
		/*
		 * RETIRED FAMILY, kept only as a fallback that can no longer fire. Achievements and
		 * milestones left chat identity by product decision; `ChatIdentity::sanitizeGlobalBadge`
		 * (api/app/helpers/chat_identity.php) nulls the whole `achievement:` prefix out on the
		 * way through, which is the one place both the settings routes and the message
		 * enrichment read a stored badge, so no message can carry one any more. The web's
		 * badgeVisual() still has the same dead arm; they come out together or not at all.
		 */
		b.label = QStringLiteral("🏆");
		b.icon = true;
		b.fg = QStringLiteral("#f6cfff"); /* fuchsia-200 */
		b.title = QStringLiteral("Achievement");
	} else {
		b.label = QStringLiteral("★");
		b.icon = true;
		b.title = badgeId;
	}

	/* `chat_badge_label` is the server's WORD for this badge. The catalog's title wins where it
	 * has one — matching the web's `visual.title ?? label` — so this only fills a gap. */
	if (b.title.isEmpty() && !label.isEmpty())
		b.title = label;

	if (b.label.isEmpty())
		return false;

	*out = b;
	return true;
}

/* ──────────────────────────────────────────────────────────────────────────────────────────────
 * Message HTML
 * ────────────────────────────────────────────────────────────────────────────────────────────── */

namespace {

/* Chat is untrusted user input rendered into a rich-text view, so every field that reaches the
 * HTML must be escaped. A display name containing markup would otherwise be interpreted. */
QString esc(const QString &s)
{
	return s.toHtmlEscaped();
}

/*
 * A badge.
 *
 * An icon badge is a bare glyph in its own colour: no background run and no padding spaces, because
 * the mark is the badge and a box around it is the thing that made these read as words.
 *
 * A numeral badge still needs its chip to read as a badge rather than as stray digits in the
 * message. Qt's rich text applies neither padding nor a border to an inline run — a QTextCharFormat
 * has a background and a foreground and nothing else — so the chip is a coloured background run and
 * the breathing room the web gets from `px-1` comes from a non-breaking space at each end. The
 * border colour badgeVisual() computes goes unused here; it is still right for anything that can
 * draw it.
 */
QString badgeHtml(const Badge &badge)
{
	if (badge.icon)
		return QStringLiteral("<span class=\"badge\" style=\"color:%1;\">%2</span>")
			.arg(badge.fg, esc(badge.label));

	return QStringLiteral("<span class=\"chip\" style=\"background-color:%1;color:%2;\">"
			      "&nbsp;%3&nbsp;</span>")
		.arg(badge.bg, badge.fg, esc(badge.label));
}

/* formatMessageTime(): 24-hour HH:MM in the viewer's own zone, or empty if the stamp is unusable. */
QString messageTime(const QString &iso)
{
	QDateTime when = QDateTime::fromString(iso, Qt::ISODateWithMs);
	if (!when.isValid())
		when = QDateTime::fromString(iso, Qt::ISODate);
	if (!when.isValid())
		return QString();
	return when.toLocalTime().toString(QStringLiteral("HH:mm"));
}

/* toLocaleString() on the web; QLocale gives the same thousands grouping. */
QString grouped(qint64 value)
{
	return QLocale::system().toString(value);
}

/*
 * Gap between two rows in the feed — the popout's `space-y-2.5` (0.625rem).
 *
 * Applied as a bottom margin only, never `margin: Npx 0`. Qt's rich text does not collapse
 * adjacent block margins the way a browser does, so a symmetric margin would double into 20px
 * between every pair of rows and space the feed out to nothing like the page it copies.
 */
constexpr int kRowGapPx = 10;

/*
 * The shell the gift row, the poll card and the ad-break pill all sit in.
 *
 * A table and not a styled <div>: Qt paints a border on a table and ignores one on a block, which
 * is also why a priority message's left rule is a 2px table cell rather than the `border-left` the
 * web uses. `border-color` is repeated after the shorthand because Qt's CSS parser takes the
 * colour from the longhand more reliably than from the shorthand.
 */
QString cardOpen(const char *background, const char *border, int padding)
{
	/* The trailing margin is the same one an ordinary row carries: the popout's `space-y-2.5`
	 * separates EVERY pair of children, and a card with no margin of its own sat flush against
	 * the next one. */
	return QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"%1\" "
			      "bgcolor=\"%2\" "
			      "style=\"background-color:%2;border:1px solid %3;border-color:%3;"
			      "margin:0 0 %4px 0;\">"
			      "<tr>")
		.arg(QString::number(padding), QString::fromUtf8(background),
		     QString::fromUtf8(border), QString::number(kRowGapPx));
}

const char *const kCardClose = "</tr></table>";

/* ---- surfaces, pre-composited over the feed's own fill; see the note at the top of the header ---- */

const char *const kGiftBg = "#171722";
const char *const kGiftBorder = "#473360";
const char *const kPollBg = "#12121b";
const char *const kPollBorder = "#2a2a32";
const char *const kPollOptionBg = "#0d0d13";
const char *const kPollOptionBorder = "#25252b";
const char *const kAdBreakBg = "#2a2318";
const char *const kAdBreakBorder = "#4d3c14";
/* The platform's own notices — purple-500/10 on purple-400/20, the web's system-message card. */
/* amber-400 — the rule and the wash on a line addressed to you. */
const char *const kMentionRule = "#ffb900";
const char *const kMentionBg = "#2a2418";
const char *const kSystemBg = "#181528";
const char *const kSystemBorder = "#3b3159";
const char *const kSystemAccent = "#c4b4fe"; /* purple-300 */
/* purple-500 at 50%, the rule the web puts down the left of a priority message. */
const char *const kPriorityRule = "#602c8d";
const char *const kAmber = "#ffb900";


struct PollTally {
	QList<int> counts;
	int total = 0;
};

/*
 * How a poll stands right now — one vote per person, their LAST one counting, exactly as
 * StreamChatPanel tallies it. A vote for an option that no longer exists is ignored rather than
 * counted into the total.
 */
PollTally tallyPoll(const QString &pollMessageId, int optionCount, const QList<QJsonObject> &all)
{
	QHash<QString, int> lastVote;
	for (const QJsonObject &message : all) {
		QString votedPoll;
		int option = 0;
		if (!XiveQuickActions::parsePollVote(
			    message.value(QStringLiteral("content")).toString(), &votedPoll, &option))
			continue;
		if (votedPoll != pollMessageId)
			continue;
		if (option < 0 || option >= optionCount)
			continue;
		lastVote.insert(message.value(QStringLiteral("user_id")).toString(), option);
	}

	PollTally tally;
	tally.counts = QList<int>(optionCount, 0);
	for (const int option : lastVote) {
		tally.counts[option] += 1;
		tally.total += 1;
	}
	return tally;
}

} // namespace

QString emptyStateHtml()
{
	return QStringLiteral(
		       "<div align=\"center\" style=\"margin:12px 4px;\"><span class=\"empty\">%1"
		       "</span></div>")
		.arg(esc(QObject::tr("Be the first to say hello. Messages appear here in real "
				     "time.")));
}

bool mentionsUser(const QString &content, const QString &username)
{
	QString name = username.trimmed();
	while (name.startsWith(QLatin1Char('@')))
		name.remove(0, 1);
	if (name.isEmpty() || content.isEmpty())
		return false;

	/*
	 * The name is escaped before it becomes a pattern. A handle is server-supplied today, which
	 * is not a reason to build a regex out of it unescaped — one containing "." or "+" would
	 * otherwise match things it should not, and the failure would be a highlight on somebody
	 * else's message rather than an error.
	 *
	 * `(^|[^A-Za-z0-9_])@name(?![A-Za-z0-9_])` — the same expression as the web's, so "@ada"
	 * matches "hey @ada!" and not "@adamant" or "x@ada".
	 */
	const QString pattern = QStringLiteral("(^|[^A-Za-z0-9_])@%1(?![A-Za-z0-9_])")
					.arg(QRegularExpression::escape(name));
	const QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption);
	return re.match(content).hasMatch();
}

QString messageHtml(const QJsonObject &message, const QList<QJsonObject> &all,
		    const QString &selfUsername)
{
	const QString id = message.value(QStringLiteral("id")).toString();
	const QString userId = message.value(QStringLiteral("user_id")).toString();
	const QString name = message.value(QStringLiteral("display_name")).toString();
	const QString content = message.value(QStringLiteral("content")).toString();
	const bool fromCreator = message.value(QStringLiteral("is_creator")).toBool();

	const QString time = messageTime(message.value(QStringLiteral("created_at")).toString());
	const QString stamp = time.isEmpty()
				      ? QString()
				      : QStringLiteral("<span class=\"ts\">%1</span> ").arg(time);

	/*
	 * ⚠️ SYSTEM NOTICES FIRST, AHEAD OF EVERY BRANCH BELOW — and that ordering is the point, not
	 * tidiness. Every branch under this one decides what a message IS by parsing its content: a
	 * gift, a poll, a poll vote, an ad break. A notice is free text the PLATFORM wrote — Xive
	 * Surge announcements, and whatever an administrator posts — so text that happened to parse
	 * as one of those would be drawn as a gift card, or (worse, parsePollVote) swallowed into
	 * nothing and never shown at all. "⚡ XIVE SURGE STARTED — 7 people are behind this stream"
	 * is one bad regex away from being invisible in a broadcaster's dock.
	 *
	 * `is_system` is set by the server and cannot be produced from the chat box, so it is the
	 * only claim here that a viewer cannot forge. Same rule, same position, as StreamChatPanel.
	 *
	 * No name, no badge, and NO ANCHOR: the author is the string "system", which matches no
	 * profile, and offering the moderation menu on it would hand the broadcaster a set of
	 * actions that can only 400. You cannot time out the platform.
	 */
	if (message.value(QStringLiteral("is_system")).toBool()) {
		return cardOpen(kSystemBg, kSystemBorder, 6) +
		       QStringLiteral("<td width=\"18\" valign=\"top\" style=\"font-size:12px;"
				      "color:%1;\">%2</td>"
				      "<td>%3<span style=\"color:%1;font-size:9px;"
				      "font-weight:700;\">%4</span><br>"
				      "<span class=\"body\">%5</span></td>")
			       .arg(QString::fromUtf8(kSystemAccent), QStringLiteral("ⓘ"), stamp,
				    esc(QObject::tr("XIVE")), esc(content)) +
		       QString::fromUtf8(kCardClose);
	}

	/* A vote is bookkeeping, not conversation. The web renders an empty fragment for it. */
	if (XiveQuickActions::parsePollVote(content, nullptr, nullptr))
		return QString();

	/*
	 * Gifts first — always, for the same reason the activity classifier checks them first: a
	 * gift's own text contains words the later branches would match.
	 */
	Gift gift;
	if (parseGift(content, &gift)) {
		return cardOpen(kGiftBg, kGiftBorder, 6) +
		       QStringLiteral("<td width=\"26\" valign=\"middle\" style=\"font-size:15px;\">"
				      "%1</td><td valign=\"middle\">%2"
				      "<span class=\"gift\">%3</span>"
				      "<span class=\"muted\"> sent </span>"
				      "<span class=\"body\">%4</span><br>"
				      "<span class=\"muted\">%5</span></td>")
			       .arg(giftEmoji(gift.giftName), stamp, esc(gift.sender),
				    esc(gift.giftName),
				    gift.credits == 1
					    ? QObject::tr("1 credit")
					    : QObject::tr("%1 credits").arg(grouped(gift.credits))) +
		       QString::fromUtf8(kCardClose);
	}

	/*
	 * A poll and an ad break are only real when the creator sent them. Anyone else typing the
	 * marker gets it rendered as the text it is — the web gates both branches on `is_creator`
	 * the same way, so a viewer cannot fake a poll into a broadcaster's dock.
	 */
	XiveQuickActions::Poll poll;
	if (fromCreator && XiveQuickActions::parsePoll(content, &poll)) {
		const PollTally tally = tallyPoll(id, int(poll.options.size()), all);

		/*
		 * Each option is its own single-row table. One table with a row per option would be
		 * cheaper, but Qt draws a continuous grid across a multi-row table's cells and there
		 * is no `border-collapse` to turn that off — separate tables give the web's stack of
		 * discrete buttons instead of a spreadsheet.
		 */
		QString options;
		for (int i = 0; i < poll.options.size(); ++i) {
			options += QStringLiteral(
					   "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"4\" "
					   "bgcolor=\"%1\" style=\"background-color:%1;"
					   "border:1px solid %2;border-color:%2;\"><tr>"
					   "<td><span class=\"body\">%3</span></td>"
					   "<td width=\"34\" align=\"right\">"
					   "<span class=\"muted\">%4</span></td></tr></table>")
					   .arg(QString::fromUtf8(kPollOptionBg),
						QString::fromUtf8(kPollOptionBorder),
						esc(poll.options.at(i)),
						QString::number(tally.counts.at(i)));
		}

		return cardOpen(kPollBg, kPollBorder, 6) +
		       QStringLiteral(
			       "<td>%1<span class=\"body\" style=\"font-weight:600;\">📊 %2</span>"
			       "<br><span class=\"muted\">%3</span>%4"
			       "</td>")
			       .arg(stamp, esc(poll.question),
				    tally.total == 1 ? QObject::tr("Poll · 1 vote")
						     : QObject::tr("Poll · %1 votes")
							       .arg(grouped(tally.total)),
				    options) +
		       QString::fromUtf8(kCardClose);
	}

	if (fromCreator && XiveQuickActions::isAdBreak(content)) {
		/* The web puts the time at the far right of this row. A narrow right-hand cell wraps
		 * "20:20" onto two lines in a docked panel, so the stamp leads here as it does on
		 * every other row. */
		return cardOpen(kAdBreakBg, kAdBreakBorder, 5) +
		       QStringLiteral("<td>%1<span style=\"color:%2;font-weight:700;\">%3</span> "
				      "<span class=\"muted\">%4</span></td>")
			       .arg(stamp, QString::fromUtf8(kAmber), esc(QObject::tr("AD BREAK")),
				    esc(QObject::tr("— short pause for ads"))) +
		       QString::fromUtf8(kCardClose);
	}

	/*
	 * An ordinary message: timestamp, the name in its own colour, then the role labels in the
	 * order StreamChatMessageMeta writes them — badge, Host, Mod, Priority.
	 *
	 * `is_pro` and `is_subscriber` are deliberately NOT drawn. The web renders neither in a
	 * message row; supporter standing reaches chat only through `chat_badge`, which is the badge.
	 */
	QString meta;

	Badge badge;
	if (badgeVisual(message.value(QStringLiteral("chat_badge")).toString(),
			message.value(QStringLiteral("chat_badge_label")).toString(), &badge))
		meta += QStringLiteral(" ") + badgeHtml(badge);

	/*
	 * "Host", not "Creator". `is_creator` means the person broadcasting THIS stream, while
	 * `creator` is an equippable chat badge any platform creator wears everywhere — two
	 * different claims that both read as "Creator". The stream role takes the distinct word.
	 * The CSS class stays `creator`: it is the styling hook, not the label.
	 */
	if (fromCreator)
		meta += QStringLiteral(" <span class=\"creator\">%1</span>")
				.arg(QObject::tr("Host"));

	/*
	 * The web ORs this flag with the live moderator roster it polls for its moderation menu.
	 * The dock has no such menu and so no roster; a moderator the server did not flag on the
	 * message reads as an ordinary viewer here.
	 */
	if (message.value(QStringLiteral("is_moderator")).toBool())
		meta += QStringLiteral(" <span class=\"mod\">%1</span>").arg(QObject::tr("Mod"));

	const bool priority = message.value(QStringLiteral("is_priority")).toBool();
	if (priority)
		meta += QStringLiteral(" <span class=\"prio\">%1</span>")
				.arg(QObject::tr("Priority"));

	/*
	 * chat_name_color now arrives populated: the server's identity enrichment
	 * (ChatIdentity::ENRICH_CHAT_IDENTITY) was gated off while the Deno original threw and
	 * swallowed the error, and is on since that stack was retired. When a viewer has set no
	 * colour the field is still null and nameColour() hashes the user id exactly as the web
	 * does, so everyone gets a colour of their own either way.
	 */
	const QString colour =
		nameColour(userId, message.value(QStringLiteral("chat_name_color")).toString());

	/*
	 * The name is an anchor so the dock can moderate the person who said this.
	 *
	 * A QTextBrowser cannot host the hover "⋮" button the web reveals on a message row — rich
	 * text has no widgets in it — but it does hit-test anchors, so the name itself is the
	 * target. The href is a private scheme carrying everything the menu needs; the dock has
	 * setOpenLinks(false), so nothing is ever navigated to and no browser is ever opened.
	 *
	 * It carries the MESSAGE id and nothing else. The dock looks the message back up in the
	 * feed's buffer, which is where the user id, the display name and the role flags already
	 * live — encoding them into the href as well would be a second copy to keep honest.
	 *
	 * Percent-encoded because an id lands inside a URL, where a stray '&' or '#' would truncate
	 * it. It is a server-issued uuid today; that is not a reason to write a parser that breaks
	 * if it ever stops being one.
	 */
	const QString href = QStringLiteral("xive-msg:%1")
				     .arg(QString::fromUtf8(QUrl::toPercentEncoding(id)));

	const QString line =
		QStringLiteral("%1<a href=\"%2\" class=\"nm\" style=\"color:%3;"
			       "text-decoration:none;\">%4</a>%5"
			       "<span class=\"body\">: %6</span>")
			.arg(stamp, href, colour, esc(name), meta, esc(content));

	/*
	 * A priority message gets the web's purple rule down its left edge. Qt ignores `border-left`
	 * on a block — it is a table cell of solid colour instead, which is the only way to get a
	 * rule on one side only.
	 *
	 * 2px of rule and 8px of gap, matching `border-l-2 pl-2`. The web's `-ml-0.5` pulls the rule
	 * 2px into the list's own padding so the text stays aligned with unmarked rows; there is no
	 * negative margin in Qt's rich text, so that half is simply left out — the text on a priority
	 * row sits 10px further right than its neighbours, which is the whole point of the marker.
	 */
	/*
	 * Is this line addressed to ME?
	 *
	 * Suppressed on your own messages — the creator says their own handle constantly ("follow
	 * @ada"), and lighting up every one of those would make the highlight worthless exactly when
	 * a real mention arrives. The web suppresses it the same way and for the same reason.
	 */
	const bool mentioned = !fromCreator && mentionsUser(content, selfUsername);

	/*
	 * The web applies BOTH classes to a priority message that also mentions you, and the later
	 * one wins on the properties they share — so amber beats purple on the rule. Mirrored here by
	 * testing the mention first: one rule, one colour, and the louder claim takes it.
	 */
	if (mentioned || priority) {
		const char *rule = mentioned ? kMentionRule : kPriorityRule;
		/* The wash is the mention's alone. A priority row keeps the feed's own background,
		 * exactly as `border-l-2 pl-2` with no `bg-` does on the web. */
		const QString cellBg =
			mentioned ? QStringLiteral(" bgcolor=\"%1\" style=\"background-color:%1;\"")
					    .arg(QString::fromUtf8(kMentionBg))
				  : QString();

		return QStringLiteral(
			       "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\" "
			       "style=\"margin:0 0 %3 0;\"><tr>"
			       "<td width=\"2\" bgcolor=\"%1\" style=\"background-color:%1;\">"
			       "&nbsp;</td>"
			       "<td width=\"8\"%4>&nbsp;</td><td%4>%2</td></tr></table>")
			.arg(QString::fromUtf8(rule), line,
			     QString::number(kRowGapPx) + QStringLiteral("px"), cellBg);
	}

	return QStringLiteral("<div style=\"margin:0 0 %1px 0;\">%2</div>")
		.arg(QString::number(kRowGapPx), line);
}

} // namespace XiveChatRender
