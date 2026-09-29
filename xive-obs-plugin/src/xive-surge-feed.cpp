#include "xive-surge-feed.h"

#include "xive-api.h"
#include "xive-auth.h"
#include "xive-channel.h"
#include "xive-realtime.h"

#include <QDateTime>
#include <QTimer>

namespace {

XiveSurgeFeed *g_instance = nullptr;

/*
 * How often a socket-less dock re-reads the surge.
 *
 * The window is five minutes and a surge that matters is being extended constantly, so 30s is
 * plenty to catch one starting and to watch the level climb. Same figure as the web's
 * SURGE_POLL_MS, and deliberately slower than chat: this is a garnish, chat is the dock.
 */
constexpr int kPollMs = 30000;

/*
 * The backstop cadence while the socket IS delivering.
 *
 * Not zero, unlike the chat feed's treatment of the same situation, and for a reason: a surge that
 * ENDS produces one publication and no more. If that single push is the one dropped by a
 * best-effort publish, the dock's own expiry clock still takes the banner down (see
 * XiveSurge::State::expiresAtMs) — but a surge that then STARTS again would be invisible until the
 * socket happened to carry a later contribution. Five minutes costs one request an hour per dock.
 */
constexpr int kLivePollMs = 300000;

qint64 nowMs()
{
	return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

XiveSurgeFeed::XiveSurgeFeed(QObject *parent) : QObject(parent)
{
	m_poll = new QTimer(this);
	m_poll->setInterval(kPollMs);
	connect(m_poll, &QTimer::timeout, this, &XiveSurgeFeed::tick);

	m_clock = new QTimer(this);
	m_clock->setInterval(1000);
	connect(m_clock, &QTimer::timeout, this, [this]() {
		/*
		 * The expiry, enforced locally. The server announces a surge STARTING and CLIMBING and
		 * says nothing at all when one lapses, so this is the only thing that ever takes the
		 * banner down on a quiet room.
		 */
		if (m_state.active && secondsLeft() <= 0) {
			applyState(XiveSurge::State());
			return;
		}

		const int left = secondsLeft();
		if (left == m_lastSeconds)
			return;
		m_lastSeconds = left;
		emit changed();
	});
}

XiveSurgeFeed *XiveSurgeFeed::instance()
{
	return g_instance;
}

void XiveSurgeFeed::createInstance()
{
	if (!g_instance)
		g_instance = new XiveSurgeFeed();
}

void XiveSurgeFeed::destroyInstance()
{
	delete g_instance;
	g_instance = nullptr;
}

void XiveSurgeFeed::addViewer()
{
	++m_viewers;
	if (m_viewers == 1) {
		/* The socket first, so it is `active` before the read below hands it a stream id —
		 * same ordering as XiveChatFeed::addViewer, and for the same reason. */
		if (XiveRealtime *rt = XiveRealtime::instance())
			rt->start();
		m_poll->start();
		tick();
	}
}

void XiveSurgeFeed::removeViewer()
{
	m_viewers = qMax(0, m_viewers - 1);
	if (m_viewers == 0) {
		m_poll->stop();
		m_clock->stop();
		m_lastSeconds = -1;
		if (XiveRealtime *rt = XiveRealtime::instance())
			rt->stop();
	}
}

int XiveSurgeFeed::secondsLeft() const
{
	return XiveSurge::secondsLeft(m_state, nowMs());
}

void XiveSurgeFeed::pollNow()
{
	tick();
}

void XiveSurgeFeed::tick()
{
	XiveApi *api = XiveApi::instance();
	XiveAuth *auth = XiveAuth::instance();
	XiveChannel *ch = XiveChannel::instance();
	if (!api || !auth || !ch)
		return;

	/*
	 * Signed out is not an error here and says nothing on the panel. The banner is a garnish over
	 * chat; chat's own dock already carries the "connect to Xive" notice, and a second copy of it
	 * stacked above the feed would be noise.
	 */
	if (!auth->isSignedIn())
		return;

	const QString streamId = ch->streamId();
	if (streamId.isEmpty())
		return;

	/* A different channel is a different room. Clear before the read lands, or the new
	 * broadcast's first seconds show the previous one's surge. */
	if (streamId != m_streamId) {
		m_streamId = streamId;
		applyState(XiveSurge::State());
	}

	if (m_inFlight)
		return;
	m_inFlight = true;

	api->streamSurge(streamId, [this, streamId](bool ok, const QJsonObject &body, int,
						    const QString &) {
		m_inFlight = false;

		/*
		 * A failed read changes NOTHING — not even to inactive. The dock keeps the state it
		 * has and its own expiry clock still retires it on time, so a blip mid-surge does not
		 * blank a banner the room is watching and then bring it back on the next tick.
		 */
		if (!ok || !body.value(QStringLiteral("success")).toBool(false))
			return;

		/* The channel may have changed while this was in flight. */
		if (streamId != m_streamId)
			return;

		applyState(XiveSurge::parse(body.value(QStringLiteral("surge")).toObject()));
	});
}

void XiveSurgeFeed::ingestRealtimeSurge(const QJsonObject &surge)
{
	/*
	 * ⚠️ NOT FILTERED ON m_streamId, because the publication carries no stream id — the channel
	 * it arrived on IS the filter, and XiveRealtime only ever subscribes to the channel of the
	 * stream this dock is showing (it re-points on every change). Filtering here would need a
	 * field that is not on the wire.
	 */
	applyState(XiveSurge::parse(surge));
}

void XiveSurgeFeed::setRealtimeConnected(bool connected)
{
	if (connected == m_realtimeConnected)
		return;
	m_realtimeConnected = connected;

	const int ms = connected ? kLivePollMs : kPollMs;
	m_poll->setInterval(ms);
	if (m_poll->isActive()) {
		m_poll->stop();
		m_poll->start();
	}

	/* The socket just dropped: re-read now, so a surge that started during the gap is not held
	 * back until the next slow tick. */
	if (!connected && m_viewers > 0)
		pollNow();
}

void XiveSurgeFeed::applyState(const XiveSurge::State &next)
{
	/*
	 * An arriving state is checked against the clock before it is stored. The server computes
	 * `active` at the moment it answers, and a read that spent four seconds in flight — or a
	 * publication delivered late after a reconnect — can describe a window that has already
	 * closed. Storing it would put the banner up for the one second it takes the clock to
	 * notice.
	 */
	XiveSurge::State applied = next;
	if (applied.active && XiveSurge::secondsLeft(applied, nowMs()) <= 0)
		applied = XiveSurge::State();

	const bool same = applied.active == m_state.active && applied.level == m_state.level &&
			  applied.points == m_state.points && applied.events == m_state.events &&
			  applied.participants == m_state.participants &&
			  applied.expiresAtMs == m_state.expiresAtMs &&
			  applied.contributors == m_state.contributors &&
			  applied.igniterName == m_state.igniterName;

	m_state = applied;
	syncClock();

	if (same)
		return;

	m_lastSeconds = secondsLeft();
	emit changed();
}

void XiveSurgeFeed::syncClock()
{
	/* The countdown runs ONLY while a surge is live — keyed on `active` rather than on arriving
	 * events, so it both starts when one begins and stops when one lapses. Keyed on events it
	 * would tick once a second forever after the last surge of the broadcast ended. */
	const bool want = m_state.active && m_viewers > 0;
	if (want && !m_clock->isActive())
		m_clock->start();
	else if (!want && m_clock->isActive())
		m_clock->stop();
}
