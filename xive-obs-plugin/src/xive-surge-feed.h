#pragma once

#include "xive-surge.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

class QTimer;

/*
 * The live Xive Surge for the current broadcast — one shared copy, as XiveChatFeed is for chat.
 *
 * FOUR INPUTS, ONE ANSWER, mirroring the web's useStreamSurge:
 *
 *   1. A read of GET /api/streaming/{id}/surge whenever the channel changes. A socket carries only
 *      what happens after you subscribe, so without this a dock opened MID-SURGE would show
 *      nothing until the next contribution — which on a quiet stretch is minutes away.
 *   2. The `stream:<id>` publication, emitted server-side on every contribution. This is what makes
 *      the level climb in real time.
 *   3. A slow poll, running only while the socket is NOT connected. Belt and braces for a dock left
 *      open for eight hours through a network blip, and the only path at all when the token fetch
 *      or the connect failed.
 *   4. A local clock. ⚠️ NOTHING IS PUBLISHED WHEN A SURGE LAPSES — the last contribution already
 *      decided when the window closes, and no request arrives at that moment to say so. The expiry
 *      is enforced here or the dock sits showing a surge for a room that went quiet twenty minutes
 *      ago.
 *
 * Point 4 is why this is a feed and not a value the chat dock reads off a poll: something has to
 * tick once a second while a surge is live, and only while one is live — an idle broadcast must not
 * repaint a dock every second for nothing.
 */
class XiveSurgeFeed : public QObject {
	Q_OBJECT

public:
	explicit XiveSurgeFeed(QObject *parent = nullptr);

	static XiveSurgeFeed *instance();
	static void createInstance();
	static void destroyInstance();

	/* Refcounted exactly like XiveChatFeed's: a dock registers while it is visible, and the reads
	 * and the clock stop when the last one goes away. */
	void addViewer();
	void removeViewer();

	const XiveSurge::State &state() const { return m_state; }

	/* Whole seconds left, computed against the clock right now — not a cached tick. */
	int secondsLeft() const;

	void pollNow();

	/* Wired to XiveRealtime in plugin-main. Raw wire object; decoding happens here. */
	void ingestRealtimeSurge(const QJsonObject &surge);
	void setRealtimeConnected(bool connected);

signals:
	/*
	 * The state, or the countdown, or both changed — one coarse signal, because the banner
	 * redraws half a dozen labels and there is nothing here worth optimising against.
	 *
	 * Emitted with an INACTIVE state when a surge ends, which is what takes the banner down.
	 */
	void changed();

private:
	void tick();
	void applyState(const XiveSurge::State &next);
	/* Start or stop the one-second countdown to match `m_state.active`. */
	void syncClock();

	QTimer *m_poll = nullptr;
	QTimer *m_clock = nullptr;
	XiveSurge::State m_state;
	QString m_streamId;
	int m_viewers = 0;
	bool m_inFlight = false;
	bool m_realtimeConnected = false;
	/* The last countdown emitted, so a tick that does not change the displayed m:ss is dropped
	 * rather than redrawing the banner for nothing. */
	int m_lastSeconds = -1;
};
