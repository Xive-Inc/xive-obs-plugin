#include "xive-realtime.h"

#include "xive-api.h"
#include "xive-config.h"
#include "xive-log.h"

#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>

namespace {

XiveRealtime *g_instance = nullptr;

/*
 * Centrifugo's unidirectional SSE endpoint, reached through the same Cloudflare hostname the web
 * client's WebSocket uses (the tunnel forwards the whole host to Centrifugo's HTTP server, so any
 * path under it works). Hardcoded like the `wss://…/connection/websocket` URL in token.php rather
 * than derived from apiBase(), because it is a different service on a different host.
 */
const char *const kUniSseUrl = "https://ws.xivecdn.com/connection/uni_sse";

/* A reconnect starts here and doubles up to this cap, so a server blip does not become a hot loop. */
constexpr int kReconnectMaxMs = 30000;

} // namespace

XiveRealtime::XiveRealtime(QObject *parent) : QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
	m_reconnect = new QTimer(this);
	m_reconnect->setSingleShot(true);
	connect(m_reconnect, &QTimer::timeout, this, &XiveRealtime::openConnection);
}

XiveRealtime::~XiveRealtime()
{
	closeConnection();
}

XiveRealtime *XiveRealtime::instance()
{
	return g_instance;
}

void XiveRealtime::createInstance()
{
	if (!g_instance)
		g_instance = new XiveRealtime();
}

void XiveRealtime::destroyInstance()
{
	delete g_instance;
	g_instance = nullptr;
}

void XiveRealtime::setStreamId(const QString &streamId)
{
	const QString id = streamId.trimmed();
	if (id == m_streamId)
		return;

	m_streamId = id;
	m_channel = id.isEmpty() ? QString() : (QStringLiteral("stream:") + id);

	/*
	 * A different channel is a different conversation. Drop the current stream and reopen on the
	 * new one if we are meant to be connected.
	 *
	 * ⚠️ REOPENED EVEN WHEN THE NEW ID IS EMPTY. Going off-air used to close this and leave it
	 * closed, which also took `streamact#` down with it — see openConnection(). That channel is
	 * the creator's, not the broadcast's, and it stays useful with no broadcast at all: it is how
	 * a test alert arrives, and how the next go-live is heard without waiting for a poll.
	 */
	if (m_active) {
		/* A pending backoff retry would open a SECOND connection alongside this one; this reopen
		 * supersedes it. XiveChannel now repoints the socket on every go-live, so the overlap is
		 * no longer rare. */
		m_reconnect->stop();
		closeConnection();
		openConnection();
	}
}

void XiveRealtime::start()
{
	/* Refcounted — see the header. Only the FIRST caller opens anything. */
	++m_users;
	if (m_users > 1)
		return;

	m_active = true;
	/* No m_streamId condition: openConnection() subscribes to whatever it can, and the creator's
	 * activity channel needs no broadcast. See setStreamId() above. */
	if (m_reply == nullptr && !m_reconnect->isActive())
		openConnection();
}

void XiveRealtime::stop()
{
	/* qMax so an unbalanced stop() — teardown racing a dock close — cannot drive the count
	 * negative and leave the connection permanently unopenable. */
	m_users = qMax(0, m_users - 1);
	if (m_users > 0)
		return; /* another dock still wants it */

	m_active = false;
	m_reconnect->stop();
	closeConnection();
	setConnected(false);
}

void XiveRealtime::setConnected(bool connected)
{
	if (connected == m_connected)
		return;
	m_connected = connected;
	emit connectedChanged(connected);
}

void XiveRealtime::openConnection()
{
	if (!m_active || m_reply != nullptr)
		return;

	XiveApi *api = XiveApi::instance();
	if (!api) {
		/* No API client (teardown, or not built yet). Try again later; the poll covers us. */
		scheduleReconnect();
		return;
	}

	/*
	 * Fetch a fresh connection token every time we (re)connect. It is short-lived (1h) and the
	 * endpoint refreshes the session on a 401, so a reconnect after a long idle self-heals rather
	 * than presenting a dead token. Guarded with a QPointer: this callback outlives the stack
	 * frame and could fire after teardown destroyed us.
	 */
	QPointer<XiveRealtime> self(this);
	const QString channel = m_channel;
	api->get(QStringLiteral("/api/realtime/token"),
		 [self, channel](bool ok, const QJsonObject &body, int, const QString &error) {
			 if (!self || !self->m_active || self->m_channel != channel)
				 return; /* torn down, stopped, or the stream changed under us */
			 /* Two token fetches for the same channel can overlap (a reopen racing a retry).
			  * Whichever lands second must not open a second stream and orphan the first. */
			 if (self->m_reply != nullptr)
				 return;
			 if (!ok || body.value(QStringLiteral("success")).toBool(false) == false) {
				 xive_debug("realtime token fetch failed: %s", qUtf8Printable(error));
				 self->scheduleReconnect();
				 return;
			 }
			 const QString token = body.value(QStringLiteral("token")).toString();
			 if (token.isEmpty()) {
				 self->scheduleReconnect();
				 return;
			 }
			 self->m_buffer.clear();

			 /*
			  * The creator's own profile id, returned beside the token, is what addresses
			  * their personal activity channel. An API that does not return it (or a
			  * caller with no profile) simply gets chat, and the activity dock stays on
			  * its poll exactly as it did before this existed.
			  */
			 const QString userId =
				 body.value(QStringLiteral("userId")).toString().trimmed();
			 self->m_activityChannel =
				 userId.isEmpty() ? QString()
						  : (QStringLiteral("streamact#") + userId);

			 /* The unidirectional connect command: the token authenticates, `subs` asks
			  * to subscribe to both channels on connect — one socket, not two.
			  *
			  * ⚠️ EITHER MAY BE ABSENT. `stream:<id>` needs a broadcast and there may be
			  * none; inserting an empty key would ask Centrifugo to subscribe to "".
			  * With neither there is nothing to carry, so the socket is not opened at
			  * all — retried rather than abandoned, because both inputs come from calls
			  * that can start succeeding. */
			 QJsonObject subOpts;
			 QJsonObject subs;
			 if (!channel.isEmpty())
				 subs.insert(channel, subOpts);
			 if (!self->m_activityChannel.isEmpty())
				 subs.insert(self->m_activityChannel, subOpts);
			 if (subs.isEmpty()) {
				 xive_debug("realtime: nothing to subscribe to yet");
				 self->scheduleReconnect();
				 return;
			 }
			 QJsonObject connectCmd;
			 connectCmd.insert(QStringLiteral("token"), token);
			 connectCmd.insert(QStringLiteral("subs"), subs);
			 const QByteArray payload =
				 QJsonDocument(connectCmd).toJson(QJsonDocument::Compact);

			 QNetworkRequest req((QUrl(QString::fromUtf8(kUniSseUrl))));
			 req.setHeader(QNetworkRequest::ContentTypeHeader,
				       QStringLiteral("application/json"));
			 req.setRawHeader("Accept", "text/event-stream");
			 req.setRawHeader("User-Agent",
					  QStringLiteral("xive-obs-plugin/%1")
						  .arg(QStringLiteral(XIVE_PLUGIN_VERSION))
						  .toUtf8());
			 /* No transfer timeout: this response is meant to stay open indefinitely. */

			 QNetworkReply *reply = self->m_nam->post(req, payload);
			 self->m_reply = reply;
			 connect(reply, &QNetworkReply::readyRead, self, &XiveRealtime::onReadyRead);
			 connect(reply, &QNetworkReply::finished, self, &XiveRealtime::onFinished);
			 xive_debug("realtime connecting to %s", qUtf8Printable(channel));
		 });
}

void XiveRealtime::closeConnection()
{
	if (m_reply) {
		/* Drop our slots first so the abort's `finished` does not schedule a reconnect. */
		m_reply->disconnect(this);
		m_reply->abort();
		m_reply->deleteLater();
		m_reply = nullptr;
	}
	m_buffer.clear();
}

void XiveRealtime::scheduleReconnect()
{
	/*
	 * ⚠️ NOT GATED ON m_streamId. It used to be, which meant a creator who was OFF AIR — no stream
	 * id — lost the socket on the first drop and never got it back, and that is exactly when it
	 * matters: `streamact#` is how the go-live push arrives. No hot loop: every retry path
	 * (including "nothing to subscribe to yet") comes back through here with the doubling backoff.
	 */
	if (!m_active)
		return;
	if (m_reconnect->isActive())
		return;
	m_reconnect->start(m_backoffMs);
	m_backoffMs = qMin(m_backoffMs * 2, kReconnectMaxMs);
}

void XiveRealtime::onFinished()
{
	setConnected(false);
	if (m_reply) {
		m_reply->deleteLater();
		m_reply = nullptr;
	}
	/* The stream ended — a server restart, a network drop, an expired token. Reconnect (with a
	 * fresh token) unless we were told to stop. The chat poll has been carrying reconciliation
	 * the whole time, so nothing is lost in the gap. */
	scheduleReconnect();
}

void XiveRealtime::onReadyRead()
{
	if (!m_reply)
		return;
	m_buffer.append(m_reply->readAll());

	/*
	 * SSE frames are separated by a blank line. Process every COMPLETE frame and keep the tail —
	 * a frame split across two TCP reads must not be parsed until its terminator arrives.
	 */
	int sep;
	while ((sep = m_buffer.indexOf("\n\n")) != -1) {
		const QByteArray frame = m_buffer.left(sep);
		m_buffer.remove(0, sep + 2);

		/* Collect the `data:` lines of this frame (SSE allows several; join with \n). Centrifugo
		 * sends exactly one, but be faithful to the format rather than assume. */
		QByteArray data;
		for (const QByteArray &line : frame.split('\n')) {
			if (!line.startsWith("data:"))
				continue; /* comments (":") and other SSE fields are not ours */
			QByteArray value = line.mid(5);
			if (value.startsWith(' '))
				value.remove(0, 1);
			if (!data.isEmpty())
				data.append('\n');
			data.append(value);
		}
		if (!data.isEmpty())
			handleFrame(data);
	}
}

void XiveRealtime::handleFrame(const QByteArray &json)
{
	const QJsonDocument doc = QJsonDocument::fromJson(json);
	if (!doc.isObject())
		return;
	handlePush(doc.object());
}

void XiveRealtime::handlePush(const QJsonObject &push)
{
	/* The connect reply confirms the session is up and the subscription was accepted. */
	if (push.contains(QStringLiteral("connect"))) {
		m_backoffMs = 1000; /* healthy connection — reset the backoff */
		setConnected(true);
		xive_debug("realtime connected on %s", qUtf8Printable(m_channel));
		return;
	}

	/* A publication on one of our channels. `{}` pings and other frame types fall through. */
	if (!push.contains(QStringLiteral("pub")))
		return;

	const QString channel = push.value(QStringLiteral("channel")).toString();
	const QJsonObject data =
		push.value(QStringLiteral("pub")).toObject().value(QStringLiteral("data")).toObject();
	const QString type = data.value(QStringLiteral("type")).toString();

	/*
	 * The creator's activity channel. Handled before the m_channel check because it is a
	 * DIFFERENT channel — falling through to that comparison would drop every event.
	 *
	 * `stream_id` is what makes the row usable: this channel carries the events of every stream
	 * the creator owns, and the dock draws exactly one broadcast. A publication for another of
	 * their streams is dropped here rather than merged into the wrong feed, and one that names no
	 * stream at all (an older server) is left to the poll, which is scoped by its route.
	 *
	 * ⚠️ ONE EXEMPTION: `metadata.test`. The studio's "Send test alert" button
	 * (POST /api/streaming/integrations/{id}/test) writes no row anywhere, so this socket is the
	 * only way it can reach the dock, and off-air it names no stream because there is none. That
	 * flag is set by that route and by nothing else — a real event is built by
	 * StreamEvents::record() from the settlement that produced it — so a viewer cannot reach it.
	 * Kept narrow: a REAL event with a mismatched stream_id is still dropped, or a creator running
	 * two broadcasts sees one's gifts in the other's feed.
	 */
	if (!m_activityChannel.isEmpty() && channel == m_activityChannel) {
		if (type == QStringLiteral("activity")) {
			const QJsonObject event = data.value(QStringLiteral("event")).toObject();
			const bool isTest = event.value(QStringLiteral("metadata"))
						    .toObject()
						    .value(QStringLiteral("test"))
						    .toBool(false);
			if (!isTest &&
			    (m_streamId.isEmpty() ||
			     data.value(QStringLiteral("stream_id")).toString() != m_streamId))
				return;
			if (!event.isEmpty())
				emit activityEvent(event);
			return;
		}

		/*
		 * The broadcast went live, ended, or was retitled — what XiveChannel used to poll
		 * my-active every 10s to find out. See api/app/helpers/stream_lifecycle.php.
		 *
		 * ⚠️ DELIBERATELY NOT FILTERED ON m_streamId, unlike activity above. A go-live for an
		 * id we have never seen is precisely the event this exists to deliver — native/WHIP
		 * broadcasts mint a new row every time — so filtering here would drop the only one
		 * that matters. XiveChannel decides what to do with an id it does not recognise.
		 */
		if (type == QStringLiteral("stream")) {
			const QJsonObject s = data.value(QStringLiteral("stream")).toObject();
			if (!s.isEmpty())
				emit streamState(s, data.value(QStringLiteral("live")).toBool());
			return;
		}
		return;
	}

	if (channel != m_channel)
		return;

	if (type == QStringLiteral("message")) {
		const QJsonObject message = data.value(QStringLiteral("message")).toObject();
		if (!message.isEmpty())
			emit chatMessage(message);
	} else if (type == QStringLiteral("delete")) {
		const QString messageId = data.value(QStringLiteral("messageId")).toString();
		if (!messageId.isEmpty())
			emit chatDeleted(messageId);
	} else if (type == QStringLiteral("viewers")) {
		emit viewers(data.value(QStringLiteral("count")).toInt());
	} else if (type == QStringLiteral("surge")) {
		/*
		 * ⚠️ AN INACTIVE SURGE IS EMITTED TOO, and must be. The server publishes the state on
		 * every contribution including the ones that do not (or no longer) qualify, and that
		 * inactive publication is how a dock which saw a surge start learns it has ended.
		 * Dropping it because it looks empty would leave the banner up until the poll caught up.
		 *
		 * `toObject()` on a missing key gives an empty object, which XiveSurge::parse decodes
		 * to exactly that inactive state — so a malformed payload fails in the same direction.
		 */
		emit surgeState(data.value(QStringLiteral("surge")).toObject());
	}
	/* timeout / ban / moderators are not surfaced here: this is the creator's own dock, and the
	 * poll's dedupe already covers the rare moderation case without a purge map. */
}
