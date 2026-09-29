#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

/*
 * Instant chat AND activity over Centrifugo's UNIDIRECTIONAL SSE transport.
 *
 * ## Why SSE and not a WebSocket
 *
 * OBS ships no Qt WebSockets module on any platform (the flatpak SDK and macOS OBS both carry
 * Qt Network but not Qt WebSockets), so a QWebSocket-based plugin would fail to load. `uni_sse` is a
 * plain long-lived HTTPS request over the same QNetworkAccessManager this plugin already uses for
 * every other call — no new dependency — and a receive-only transport is exactly what chat
 * consumption needs: the plugin still SENDS chat over the REST route (chat/send).
 *
 * ## How it connects
 *
 * One connection at a time, carrying TWO channels. The connection token is the same one the web
 * client uses (`GET /api/realtime/token`, minted for the signed-in creator):
 *
 *   `stream:<currentStreamId>`   chat and viewer counts. Public; the `stream` namespace has
 *                                `allow_subscribe_for_client`, so the token alone authorises it.
 *   `streamact#<userId>`         the creator's activity events — gifts, follows, Sparks, subs.
 *                                A `#` channel is PERSONAL: Centrifugo matches the part after it
 *                                against the token's own `sub` and refuses everyone else, so the
 *                                same token authorises this too and no viewer can subscribe to a
 *                                broadcaster's gift feed. `userId` comes back beside the token.
 *
 * The connect request is POSTed as JSON:
 *
 *     { "token": "<jwt>", "subs": { "stream:<id>": {}, "streamact#<userId>": {} } }
 *
 * and the server streams SSE frames back, each a Centrifugo push:
 *
 *     {"connect":{...}}                                   once, on connect
 *     {"channel":"stream:<id>","pub":{"data":{...}}}      a publication (our {type,message,...})
 *     {}                                                  a ping, every ~25s
 *
 * ## It never replaces the polls
 *
 * Everything degrades to the existing polls: if the token fetch, the connect or the stream fails,
 * chat, viewer counts and activity keep polling exactly as before. While connected, the chat poll
 * backs off to a slow reconciliation cadence (see XiveChatFeed) and the socket carries the messages.
 *
 * ⚠️ THE ACTIVITY POLL DOES NOT BACK OFF, deliberately. `Realtime::publish` on the server is
 * best-effort and never throws (api/app/helpers/realtime.php), the `streamact` channel has no
 * history to recover from, and a gift alert that silently never arrives is the worst failure this
 * dock has. See XiveActivityFeed.
 */
class XiveRealtime : public QObject {
	Q_OBJECT

public:
	explicit XiveRealtime(QObject *parent = nullptr);
	~XiveRealtime() override;

	static XiveRealtime *instance();
	static void createInstance();
	static void destroyInstance();

	/* Follow this stream's channel. Empty disconnects; a changed id reconnects. */
	void setStreamId(const QString &streamId);
	/*
	 * ⚠️ REFERENCE COUNTED. Every start() must be paired with exactly one stop().
	 *
	 * It was a plain boolean while XiveChatFeed was the only caller. XiveActivityFeed is now a
	 * second one — it needs the socket for `streamact#`, which chat has no reason to care about —
	 * and with two independent callers a boolean means whichever releases FIRST closes the
	 * connection out from under the other. That failure is invisible: both feeds keep polling and
	 * the events merely stop being instant. The connection opens on the first start() and closes
	 * on the last stop().
	 */
	void start();
	void stop();

	bool isConnected() const { return m_connected; }

signals:
	/* A new chat message row, in the same shape GET /chat/messages returns. */
	void chatMessage(const QJsonObject &message);
	/* A message was deleted (moderation). */
	void chatDeleted(const QString &messageId);
	/* The concurrent viewer count changed. */
	void viewers(int count);
	/*
	 * One activity event, in the same shape GET /api/streaming/{id}/activity returns each row —
	 * `description`, `amount_label` and `filter` already rendered by the server, from the same
	 * saRenderEvent() the route calls (api/app/helpers/stream_event_render.php). Already filtered
	 * to the current broadcast; see the note on m_activityChannel.
	 */
	void activityEvent(const QJsonObject &event);
	/*
	 * The broadcast went live, ended, or was retitled. `stream` is the shape my-active's
	 * `stream` object has minus `viewer_count` — the count has its own publication and its own
	 * poll, and a lifecycle push must not clobber it. `live` is computed by the server so the
	 * three surfaces cannot disagree about whether 'broadcasting' counts as live.
	 *
	 * NOT filtered to the current broadcast, unlike activityEvent: the id may be one this dock
	 * has never seen, which is exactly the go-live case.
	 */
	void streamState(const QJsonObject &stream, bool live);
	/*
	 * The broadcast's Xive Surge changed — the raw `surge` object, in the shape
	 * GET /api/streaming/{id}/surge returns (api/app/helpers/stream_surge.php publishes both
	 * from one function). Decoded by XiveSurge::parse, never here.
	 *
	 * ⚠️ AN INACTIVE STATE IS A REAL EVENT, not an empty one. It is the only way a client learns
	 * a surge has ENDED early — see the note where this is emitted.
	 */
	void surgeState(const QJsonObject &surge);
	/* The subscription came up or went down — lets the chat poll back off and resume. */
	void connectedChanged(bool connected);

private:
	void openConnection();
	void closeConnection();
	void scheduleReconnect();
	void onReadyRead();
	void onFinished();
	void handleFrame(const QByteArray &json);
	void handlePush(const QJsonObject &push);
	void setConnected(bool connected);

	QNetworkAccessManager *m_nam = nullptr;
	QNetworkReply *m_reply = nullptr;
	QTimer *m_reconnect = nullptr;
	QByteArray m_buffer; /* SSE bytes received but not yet split into whole frames */
	QString m_streamId;
	QString m_channel; /* "stream:" + m_streamId */
	/*
	 * "streamact#" + the caller's own profile id, learned from the token response and therefore
	 * only known once a connect has been attempted. Empty until then, and empty for good against
	 * an API old enough not to return `userId` — in which case chat still works and activity
	 * stays on its poll.
	 *
	 * ⚠️ IT IS THE CREATOR'S CHANNEL, NOT THE BROADCAST'S. One socket carries the events of every
	 * stream they own, so a publication is matched against m_streamId before it is emitted.
	 */
	QString m_activityChannel;
	bool m_connected = false;
	bool m_active = false;     /* whether we want a connection at all */
	int m_users = 0;           /* start()/stop() refcount — see those */
	int m_backoffMs = 1000;    /* grows to a cap between reconnect attempts */
};
