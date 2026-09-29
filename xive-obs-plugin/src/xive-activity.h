#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>

/*
 * The studio activity feed: follows, gifts, Sparks, supporter unlocks, subs, super chats.
 *
 * ── This file used to be a regex table. It is not one any more ───────────────────────────────
 *
 * There was no activity endpoint, so Xive derived the feed from the CHAT MESSAGE STREAM,
 * client-side, and this file was a deliberately faithful port of the website's pattern list
 * (`/spark/i`, `/clip/i`, `/followed|new follower/i`, …) so that a message classifying as a gift on
 * the website classified as a gift here too.
 *
 * Faithful, and wrong in the same three ways on both sides:
 *
 *   1. ONLY GIFTS EVER WORKED. gift_send.php happens to write a parseable chat line. Follows and
 *      Sparks wrote no chat line at all, so no amount of pattern matching could recover them — the
 *      "Follows" filter was empty by construction, not because nobody followed.
 *   2. A VIEWER TYPING "clip" MINTED A CLIP EVENT. Every pattern ran against arbitrary viewer
 *      text, so the feed reported things that had not happened.
 *   3. Adding an event type meant editing two languages and shipping a new plugin binary.
 *
 * Events now come from `GET /api/streaming/{id}/activity` (api/app/streaming/activity.php) with
 * their `description`, `amount_label` and `filter` already rendered by the server, precisely so the
 * dock and the website cannot drift again. This file maps that payload; it does not classify.
 *
 * ── The one thing still derived from chat ────────────────────────────────────────────────────
 *
 * Super chats. `is_priority` is a FLAG ON THE MESSAGE, not a guess about its text — the message is
 * the event, so there is nothing for the server to record separately. Same rule as the web's
 * sections/studioActivityEvents.ts.
 */
namespace XiveActivity {

struct Event {
	QString id;
	QString type;    /* follow, gift, spark, subscription, gifted-sub, supporter-tier,
			  * super-chat, raid, marker */
	QString filter;  /* the filter bucket: follows, gifts, sparks, subs, super-chats, raids,
			  * markers */
	QString displayName;
	QString description;
	QString amountLabel; /* empty unless the event carries a quantity ("50 credits") */
	QString createdAtIso;
};

/*
 * Map wire rows from the activity route. Rows whose `type` this build does not know are DROPPED
 * rather than shown as "other": a plugin older than the server would otherwise draw a row with no
 * colour, no description and no filter that can reach it, and the broadcaster would have no way to
 * tell it was a version gap.
 */
QList<Event> fromServerRows(const QList<QJsonObject> &rows);

/* Super chats out of the chat buffer — the one remaining chat-derived type. See the header. */
QList<Event> superChatsFromMessages(const QList<QJsonObject> &messages);

/*
 * The feed the dock draws: server events plus chat-derived super chats, deduped by id and sorted
 * newest first. Server rows win a collision, because they carry the canonical rendering.
 */
QList<Event> mergeEvents(const QList<Event> &serverEvents, const QList<QJsonObject> &messages);

/* "all" returns everything; any other value keeps only events in that bucket. */
QList<Event> filterEvents(const QList<Event> &events, const QString &filter);

/* Counts per bucket, for the filter labels. `all` is the total. */
QMap<QString, int> countByFilter(const QList<Event> &events);

/* "just now", "5m ago", "2h ago", "3d ago" — mirrors formatActivityRelativeTime(). */
QString relativeTime(const QString &iso);

/* The filter buckets offered in the UI, in order, as (id, label) pairs. */
QList<QPair<QString, QString>> filterOptions();

} // namespace XiveActivity
