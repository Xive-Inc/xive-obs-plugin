#include "xive-activity-feed.h"

#include "xive-api.h"
#include "xive-auth.h"
#include "xive-channel.h"
#include "xive-config.h"
#include "xive-realtime.h"

#include <QDateTime>
#include <QJsonArray>
#include <QTimer>

#include <algorithm>

namespace {

XiveActivityFeed *g_instance = nullptr;

/*
 * Bounded for the same reason as the chat buffer: a long broadcast on a busy channel would
 * otherwise grow this without limit. 500 is far more than the dock scrolls through, and the
 * route itself caps a single response at 200.
 */
constexpr int kMaxRetained = 500;

/*
 * Deliberately slower than chat.
 *
 * A follow or a gift is a notable event, not a conversation — the dock is glanced at, not read, so
 * a few seconds of latency is invisible. Chat polls at 3 s by default and this is a second request
 * against the same API for one broadcaster, so it is derived from the chat cadence rather than
 * fixed: whatever the operator has configured, activity polls a good deal less often than chat.
 */
int pollIntervalMs()
{
	return qBound(6000, XiveConfig::instance().chatPollMs() * 4, 60000);
}

/*
 * The interval used INSTEAD of the above while the realtime channel is delivering.
 *
 * Events arrive as they are written once the socket is up, so the poll stops being how the dock is
 * populated. It is left running only to catch a `Realtime::publish` that failed on its way out of
 * PHP and therefore never reached Centrifugo at all — `streamact` has no history, so nothing
 * replays it.
 *
 * 120s is picked against evidence: across 13 days of retained logs there is not one
 * `realtime_publish_*` failure over several thousand publishes. This insures against something not
 * yet observed on this deployment, so it should be cheap rather than fast. The dock is also a
 * scrollback the broadcaster reads after the fact — unlike Lumia, nothing here fires lights.
 */
constexpr int kRealtimeBackstopPollMs = 120000;

} // namespace

XiveActivityFeed::XiveActivityFeed(QObject *parent) : QObject(parent)
{
	m_timer = new QTimer(this);
	m_timer->setInterval(pollIntervalMs());
	connect(m_timer, &QTimer::timeout, this, &XiveActivityFeed::tick);
}

XiveActivityFeed *XiveActivityFeed::instance()
{
	return g_instance;
}

void XiveActivityFeed::createInstance()
{
	if (!g_instance)
		g_instance = new XiveActivityFeed();
}

void XiveActivityFeed::destroyInstance()
{
	delete g_instance;
	g_instance = nullptr;
}

void XiveActivityFeed::addViewer()
{
	++m_viewers;
	if (m_viewers == 1) {
		/* Open the realtime connection first, so it is `active` before the tick below hands
		 * it a stream id — the same ordering XiveChatFeed uses.
		 *
		 * This feed holds its OWN reference rather than relying on the chat feed's. The
		 * activity dock happens to register a chat viewer too (it needs the chat buffer for
		 * super chats), so the socket would usually be up anyway — but that is a coincidence
		 * of what one dock needs, not a guarantee, and events must not depend on it. start()
		 * and stop() are refcounted precisely so both feeds can ask independently. */
		if (XiveRealtime *rt = XiveRealtime::instance())
			rt->start();
		/* Honour the socket state we may already be in: the chat dock can have had this
		 * connection up for hours before the activity dock was opened. */
		m_timer->setInterval(m_realtimeConnected ? kRealtimeBackstopPollMs : pollIntervalMs());
		m_timer->start();
		tick();
	}
}

void XiveActivityFeed::removeViewer()
{
	m_viewers = qMax(0, m_viewers - 1);
	if (m_viewers == 0) {
		m_timer->stop();
		if (XiveRealtime *rt = XiveRealtime::instance())
			rt->stop();
	}
}

void XiveActivityFeed::pollNow()
{
	tick();
}

void XiveActivityFeed::tick()
{
	XiveApi *api = XiveApi::instance();
	XiveAuth *auth = XiveAuth::instance();
	XiveChannel *ch = XiveChannel::instance();
	if (!api || !auth || !ch)
		return;

	if (!auth->isSignedIn()) {
		emit notice(tr("Connect to Xive in the Xive dock."));
		return;
	}

	const QString streamId = ch->streamId();
	if (streamId.isEmpty()) {
		emit notice(tr("No Xive channel yet. Use “Set up OBS to stream to Xive” first."));
		return;
	}

	/* A different channel is a different broadcast; the cursor goes with the events it came
	 * from, or the first poll on the new stream would ask for everything after the old one's
	 * newest row and silently return nothing. */
	if (streamId != m_streamId) {
		m_streamId = streamId;
		m_events.clear();
		m_seen.clear();
		m_cursor.clear();
		emit changed();
	}

	/* Keep the realtime subscription pointed at the broadcast we are showing (idempotent). The
	 * chat feed does the same, but it only ticks while a chat dock is open — with this dock alone
	 * on screen, nothing else would ever tell the connection which stream to follow. */
	if (XiveRealtime *rt = XiveRealtime::instance())
		rt->setStreamId(streamId);

	if (m_inFlight)
		return;
	m_inFlight = true;

	api->streamActivity(streamId, m_cursor,
			    [this](bool ok, const QJsonObject &body, int status,
				   const QString &error) {
		m_inFlight = false;

		if (!ok) {
			/*
			 * 403 is the ordinary answer for anyone who is not the stream's owner — the
			 * route is creator-only. Saying "forbidden" to a broadcaster whose session is
			 * still settling is noise, so it reads as an empty feed instead.
			 */
			if (status == 403) {
				emit notice(tr("Activity is only visible to the channel owner."));
				return;
			}
			emit notice(error);
			return;
		}

		if (!body.value(QStringLiteral("success")).toBool(true)) {
			emit notice(tr("Xive could not load activity just now. Retrying…"));
			return;
		}

		const QJsonArray incoming = body.value(QStringLiteral("events")).toArray();

		QList<QJsonObject> rows;
		rows.reserve(incoming.size());
		for (const QJsonValue &v : incoming)
			rows.append(v.toObject());

		const bool added = insertEvents(XiveActivity::fromServerRows(rows));

		/*
		 * Advance the cursor from the RAW RESPONSE, not from what survived into the buffer,
		 * and do it whether or not anything was new.
		 *
		 * ⚠️ BOTH HALVES OF THAT MATTER, and neither used to. This advanced only when a row
		 * was actually added, which was fine while the poll was the only source — `since` is
		 * strictly-greater-than, so every row it returned was new by construction. It is not
		 * fine now: an event pushed over the socket is already in m_seen by the time the poll
		 * fetches it, so nothing is added, and a cursor that only moves on an addition would
		 * sit still and re-request the same rows for the rest of the broadcast. Deriving it
		 * from the raw rows also fixes the same stall for an event type this build does not
		 * know, which fromServerRows() drops on the floor.
		 *
		 * Still the newest row the SERVER gave us and never "now": a clock skew between OBS
		 * and the database would otherwise skip events written in the gap, and a strict `>`
		 * bound never fetches them again.
		 */
		QString newest;
		qint64 newestMs = -1;
		for (const QJsonObject &row : rows) {
			const QString iso = row.value(QStringLiteral("created_at")).toString();
			const QDateTime ts = QDateTime::fromString(iso, Qt::ISODateWithMs);
			if (!ts.isValid())
				continue;
			const qint64 ms = ts.toMSecsSinceEpoch();
			if (ms > newestMs) {
				newestMs = ms;
				newest = iso;
			}
		}
		if (!newest.isEmpty()) {
			const QDateTime current =
				QDateTime::fromString(m_cursor, Qt::ISODateWithMs);
			/* Never backwards: an out-of-order or malformed response must not re-open a
			 * window we have already closed. */
			if (!current.isValid() || newestMs > current.toMSecsSinceEpoch())
				m_cursor = newest;
		}

		emit notice(QString());

		if (added)
			emit changed();
	});
}

bool XiveActivityFeed::insertEvents(const QList<XiveActivity::Event> &incoming)
{
	bool added = false;
	for (const XiveActivity::Event &e : incoming) {
		if (e.id.isEmpty() || m_seen.contains(e.id))
			continue;
		m_seen.insert(e.id);
		m_events.append(e);
		added = true;
	}
	if (!added)
		return false;

	/* Newest first — the order the dock draws, and the order the trim below assumes. */
	std::sort(m_events.begin(), m_events.end(),
		  [](const XiveActivity::Event &a, const XiveActivity::Event &b) {
			  return QDateTime::fromString(a.createdAtIso, Qt::ISODateWithMs) >
				 QDateTime::fromString(b.createdAtIso, Qt::ISODateWithMs);
		  });

	if (m_events.size() > kMaxRetained) {
		for (int i = kMaxRetained; i < m_events.size(); ++i)
			m_seen.remove(m_events.at(i).id);
		m_events.erase(m_events.begin() + kMaxRetained, m_events.end());
	}

	return true;
}

void XiveActivityFeed::setRealtimeConnected(bool connected)
{
	if (connected == m_realtimeConnected)
		return;
	m_realtimeConnected = connected;

	/* Restart a running timer so the new cadence takes effect now rather than after one more
	 * interval at the old rate — same handling as XiveChatFeed. */
	const int ms = connected ? kRealtimeBackstopPollMs : pollIntervalMs();
	m_timer->setInterval(ms);
	if (m_timer->isActive()) {
		m_timer->stop();
		m_timer->start();
	}

	/* The socket just dropped: read immediately rather than waiting out a fresh interval, so the
	 * gap the disconnect left is closed now. Only when a dock is actually watching. */
	if (!connected && m_viewers > 0)
		tick();
}

void XiveActivityFeed::ingestRealtimeEvent(const QJsonObject &row)
{
	/*
	 * Nothing to merge into yet — the first tick has not run, so it has no broadcast to attribute
	 * this to and would clear the buffer anyway. Dropping it costs nothing: that first poll reads
	 * the whole window and will carry this event with it.
	 *
	 * ⚠️ THIS ALSO DROPS A TEST EVENT, and that is correct rather than an oversight. XiveRealtime
	 * exempts `metadata.test` from the stream match so an off-air test still reaches us, but an
	 * empty m_streamId here means the dock has no channel AT ALL — tick() has not got past its
	 * "No Xive channel yet" notice — and there is no feed for a test to appear in. Being off-air
	 * with a channel set up is the case that matters, and m_streamId is populated there.
	 */
	if (m_streamId.isEmpty())
		return;

	QList<QJsonObject> rows;
	rows.append(row);

	/* Mapped by the same fromServerRows() the poll uses, so a pushed row and a polled row become
	 * the same Event — including being DROPPED identically when the type is one this build does
	 * not know. The cursor is deliberately untouched; see the header. */
	if (insertEvents(XiveActivity::fromServerRows(rows)))
		emit changed();
}
