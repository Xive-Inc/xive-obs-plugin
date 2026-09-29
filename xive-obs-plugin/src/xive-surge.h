#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

/*
 * Xive Surge — the momentum of a live broadcast, as the dock needs it.
 *
 * All of it is pure: a wire object in, a struct out, and formatting on top of that. No widget and
 * no network, so scripts/test-surge.cpp can assert the level maths, the tier names and the
 * countdown without an OBS, and the dock is left as wiring. Same rule as xive-chat-render.
 *
 * ── What a surge IS, because the shape does not say it ───────────────────────────────────────
 *
 * A CHAIN of contributions with no gap longer than five minutes — NOT a sliding window. Gifts,
 * Sparks, subs and gifted subs each score points and each buys another five minutes; a silence
 * longer than that breaks the chain and the surge is over. So the score only ever goes UP while
 * people are still giving, and the countdown is "how long this room has to keep it alive" rather
 * than "time remaining in an event". That is why the meter here refills on every contribution
 * instead of draining towards a finish line.
 *
 * 🔴 THE CONSTANTS BELOW MIRROR api/app/helpers/stream_surge.php AND MUST MOVE WITH IT. The server
 * is the only thing that computes a level — nothing here scores an event — but the dock still needs
 * the thresholds to say "3 more to level 4", and a stale copy would make the dock disagree with the
 * banner every viewer is looking at. The web keeps a third copy in
 * src/components/streaming/studio/sections/studioXiveSurge.ts for the same reason.
 *
 * ⚠️ THE SERVER PUBLISHES INACTIVE STATES TOO, and they must be rendered rather than ignored: an
 * inactive push is how a dock that saw a surge start learns it has ended. `parse()` therefore never
 * returns "nothing" — it returns a state whose `active` is false.
 */
namespace XiveSurge {

/** The silence that ends a surge, and the extension every contribution buys. */
constexpr int kWindowSeconds = 300;
/** Points to start a surge. Level 1 costs exactly this. */
constexpr int kStartThreshold = 12;
/** Extra points per level after the first. */
constexpr int kLevelStep = 8;
/** The ceiling. Level 20 is 164 points — rare rather than decorative. */
constexpr int kMaxLevel = 20;

struct State {
	bool active = false;
	int level = 0;
	int points = 0;
	/** The honest COUNT of contributions. `points` is a weighted score and is not one. */
	int events = 0;
	int participants = 0;
	/**
	 * Epoch MILLISECONDS, matching the server's `expires_at`. Zero when there is no surge.
	 *
	 * ⚠️ NOTHING IS PUBLISHED WHEN A SURGE LAPSES — the last contribution already decided when
	 * the window closes, and no request arrives at that moment to announce it. Every client
	 * enforces the expiry against its own clock or it sits looking at a dead surge forever.
	 */
	qint64 expiresAtMs = 0;
	QStringList contributors;
	/** The Surge Igniter: whoever has put the most into this chain, by points. Empty when none. */
	QString igniterName;
	int igniterPoints = 0;
};

/**
 * Decode the `surge` object — from `GET /api/streaming/{id}/surge`, from the `stream:<id>`
 * publication, or from `stream.surge` on a detail read. All three are the same function's output
 * server-side, so one decoder covers them.
 *
 * A missing or malformed object decodes to an inactive state, which is the safe direction: the
 * banner goes away rather than freezing on whatever it last showed.
 */
State parse(const QJsonObject &surge);

/** Cumulative points needed to REACH a level — StreamSurge::levelThreshold. */
int levelThreshold(int level);

/**
 * Points still needed for the next level, or 0 at the ceiling.
 *
 * The wire carries the level and the points but not this: the gap is arithmetic both sides already
 * have the constants for, and one fewer field is one fewer thing to keep in step.
 */
int toNextLevel(const State &state);

/** Whole seconds before the surge lapses, clamped at 0. */
int secondsLeft(const State &state, qint64 nowMs);

/** "4:32" — `formatSurgeCountdown` in useStreamSurge.ts. */
QString formatCountdown(int seconds);

/**
 * How full the window meter is, 0..100.
 *
 * The remaining time as a proportion of kWindowSeconds — NOT progress towards the next level.
 * A level bar would sit still for minutes at a time; the clock is the thing a room can act on.
 */
int windowPercent(const State &state, qint64 nowMs);

/**
 * Level → the name the bar carries — `surgeTier` in StreamSurgeBanner.tsx.
 *
 * Five bands rather than one name per level: there are twenty levels, and twenty adjectives would
 * each be less believable than the last. Bands change rarely enough that the room notices.
 */
QString tier(int level);

} // namespace XiveSurge
