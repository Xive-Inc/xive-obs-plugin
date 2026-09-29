#pragma once

#include "xive-activity.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

class QTimer;

/*
 * The current broadcast's activity events: pushed over the realtime connection, polled as a
 * backstop.
 *
 * WHY IT IS SEPARATE FROM XiveChatFeed: the activity dock used to be a pure view over the chat
 * buffer, because the feed was reconstructed by classifying chat text. It is not any more — events
 * come from `GET /api/streaming/{id}/activity`, a different route with a different cadence and a
 * different auth gate (creator-only; chat is readable by any viewer). Bolting it onto the chat feed
 * would mean the chat dock's poll dragged a creator-only request along with it for every viewer.
 *
 * Super chats are still taken from the chat buffer, because `is_priority` is a flag on the message
 * itself. The dock merges the two — see XiveActivity::mergeEvents().
 *
 * INCREMENTAL: after the first load each poll sends `?since=<newest created_at>`. The feed is
 * backfilled from every historical gift and spark on the stream, so a full re-read every few
 * seconds would re-ship a list that grows with the channel's whole history.
 *
 * ── Pushed and polled, and why both ──────────────────────────────────────────────────────────
 *
 * XiveRealtime subscribes to the creator's `streamact#<userId>` channel and hands each event here
 * through ingestRealtimeEvent(), already rendered by the same server helper the route uses. A gift
 * therefore lands in the dock as it happens rather than up to a poll interval later.
 *
 * ⚠️ THE POLL IS THE BACKSTOP AND ITS CADENCE IS NOT REDUCED. Unlike the chat feed, which slows its
 * poll right down while the socket is delivering, this one keeps its normal interval:
 * `Realtime::publish` on the server is deliberately best-effort and never throws
 * (api/app/helpers/realtime.php), and `streamact` has no history for a reconnect to recover from —
 * so a publication CAN be lost with nothing to replay it. A missed chat line is a gap in a
 * conversation; a missed gift is a broadcaster wondering why their alert never fired.
 *
 * ⚠️ A PUSHED EVENT DOES NOT ADVANCE THE CURSOR. Only the poll may, and only from what the server
 * actually returned — see the note in tick(). The cursor is a strict `created_at >` bound, so
 * advancing it past an event that was pushed but never written to the poll's answer would hide a
 * dropped neighbour for good.
 */
class XiveActivityFeed : public QObject {
	Q_OBJECT

public:
	explicit XiveActivityFeed(QObject *parent = nullptr);

	static XiveActivityFeed *instance();
	static void createInstance();
	static void destroyInstance();

	/* Poll while at least one dock is interested, same contract as XiveChatFeed. */
	void addViewer();
	void removeViewer();

	const QList<XiveActivity::Event> &events() const { return m_events; }

	void pollNow();

	/*
	 * One event pushed over the realtime connection, in the same wire shape a polled row has.
	 * Deduped against what is already held, so the poll re-fetching it costs nothing.
	 */
	void ingestRealtimeEvent(const QJsonObject &row);

	/*
	 * The realtime channel came up or went down (wired to XiveRealtime in plugin-main).
	 *
	 * While it is up, events arrive as they are written and the poll below drops to a slow
	 * backstop; while it is down, the poll is the only source and runs at its normal rate. Exactly
	 * what XiveChatFeed::setRealtimeConnected() does for chat, and for the same reason.
	 */
	void setRealtimeConnected(bool connected);

signals:
	/* New events arrived, or the buffer was cleared for a new broadcast. Either way the view
	 * redraws from events(); there is no append fast-path because the list is sorted by time
	 * and a late-arriving row can land anywhere in it. */
	void changed();
	/* Human-readable status, or empty when things are fine. */
	void notice(const QString &message);

private:
	void tick();
	/*
	 * Fold rows into the buffer: dedupe by id, sort newest first, trim to the retention cap.
	 * Returns true when anything was actually added. Shared by the poll and the push so the two
	 * cannot order or bound the list differently. Does NOT touch the cursor — see the header.
	 */
	bool insertEvents(const QList<XiveActivity::Event> &incoming);

	QTimer *m_timer = nullptr;
	QList<XiveActivity::Event> m_events;
	QSet<QString> m_seen;
	QString m_streamId;
	/* The newest created_at held, used as the next poll's cursor. */
	QString m_cursor;
	int m_viewers = 0;
	bool m_inFlight = false;
	bool m_realtimeConnected = false;
};
