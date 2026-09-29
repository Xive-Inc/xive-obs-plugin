#include "xive-chat-feed.h"

#include "xive-api.h"
#include "xive-auth.h"
#include "xive-channel.h"
#include "xive-config.h"
#include "xive-log.h"
#include "xive-realtime.h"

#include <QJsonArray>
#include <QTimer>

namespace {

XiveChatFeed *g_instance = nullptr;

/* The route serves the last 50 messages, so anything beyond a few hundred is history nobody is
 * scrolling to in a dock this size. Bounded so a long broadcast cannot grow without limit. */
constexpr int kMaxRetained = 500;

/*
 * Poll cadence while the realtime socket is delivering chat. The poll stops being how messages
 * arrive and becomes a reconciliation backstop for the one thing the socket cannot cover — a
 * best-effort publish that failed server-side — so it runs slowly. It snaps back to the
 * configured cadence (3s default) the instant the socket drops. Mirrors the web client.
 */
constexpr int kChatLivePollMs = 30000;

} // namespace

XiveChatFeed::XiveChatFeed(QObject *parent) : QObject(parent)
{
	m_timer = new QTimer(this);
	m_timer->setInterval(XiveConfig::instance().chatPollMs());
	connect(m_timer, &QTimer::timeout, this, &XiveChatFeed::tick);
}

XiveChatFeed *XiveChatFeed::instance()
{
	return g_instance;
}

void XiveChatFeed::createInstance()
{
	if (!g_instance)
		g_instance = new XiveChatFeed();
}

void XiveChatFeed::destroyInstance()
{
	delete g_instance;
	g_instance = nullptr;
}

void XiveChatFeed::addViewer()
{
	++m_viewers;
	if (m_viewers == 1) {
		// Open the realtime stream first (it needs to be `active` before the tick below hands
		// it a stream id), then start the poll and do the first read.
		if (XiveRealtime *rt = XiveRealtime::instance())
			rt->start();
		m_timer->start();
		tick();
	}
}

void XiveChatFeed::removeViewer()
{
	m_viewers = qMax(0, m_viewers - 1);
	if (m_viewers == 0) {
		m_timer->stop();
		if (XiveRealtime *rt = XiveRealtime::instance())
			rt->stop();
	}
}

void XiveChatFeed::pollNow()
{
	tick();
}

void XiveChatFeed::tick()
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

	/* A different channel is a different conversation; keeping the old messages would mix two
	 * broadcasts together. */
	if (streamId != m_streamId) {
		m_streamId = streamId;
		m_messages.clear();
		m_seen.clear();
		/* The pin belongs to the old broadcast. Cleared with the messages, or the banner would
		 * keep showing another channel's pinned message until the first poll of this one. */
		if (!m_pinned.isEmpty()) {
			m_pinned = QJsonObject();
			emit pinChanged(m_pinned);
		}
		m_canPin = false;
		emit reset();
	}

	/* Keep the realtime subscription pointed at the channel we are showing (idempotent). */
	if (XiveRealtime *rt = XiveRealtime::instance())
		rt->setStreamId(streamId);

	if (m_inFlight)
		return;
	m_inFlight = true;

	api->chatMessages(streamId, [this](bool ok, const QJsonObject &body, int,
					   const QString &error) {
		m_inFlight = false;

		if (!ok) {
			emit notice(error);
			return;
		}

		/*
		 * The route answers 200 with success:false and an empty list when it fails
		 * internally — by design, so a chat widget shows no history rather than an error
		 * over the video. Report it as a transient problem rather than as silence.
		 */
		if (!body.value(QStringLiteral("success")).toBool(true)) {
			emit notice(tr("Xive could not load chat just now. Retrying…"));
			return;
		}

		const QJsonArray incoming = body.value(QStringLiteral("messages")).toArray();

		QList<QJsonObject> fresh;
		for (const QJsonValue &v : incoming) {
			const QJsonObject m = v.toObject();
			const QString id = m.value(QStringLiteral("id")).toString();
			if (id.isEmpty() || m_seen.contains(id))
				continue;
			m_seen.insert(id);
			m_messages.append(m);
			fresh.append(m);
		}

		if (m_messages.size() > kMaxRetained) {
			const int excess = m_messages.size() - kMaxRetained;
			for (int i = 0; i < excess; ++i)
				m_seen.remove(m_messages.at(i).value(QStringLiteral("id")).toString());
			m_messages.erase(m_messages.begin(), m_messages.begin() + excess);
		}

		/*
		 * The pin rides on every poll, so this is level-triggered: a pin set from the website
		 * or the phone shows up here within one tick without the plugin being told, and an
		 * expiry — which nothing announces, it simply stops being returned — clears itself.
		 *
		 * Compared by id so the banner is only rebuilt when it actually changed; the poll runs
		 * every 3s and redrawing on each tick would flicker the widget under the cursor.
		 */
		const QJsonObject pin = body.value(QStringLiteral("pinned_message")).toObject();
		const QString wasPinned = m_pinned.value(QStringLiteral("id")).toString();
		const QString nowPinned = pin.value(QStringLiteral("id")).toString();
		m_canPin = body.value(QStringLiteral("can_pin")).toBool(false);
		if (wasPinned != nowPinned) {
			m_pinned = pin;
			emit pinChanged(m_pinned);
		} else {
			/* Same message, but the pin's own fields (expiry) may have moved. Keep the
			 * copy current without telling anyone to redraw. */
			m_pinned = pin;
		}

		emit notice(incoming.isEmpty() ? tr("No chat messages yet.") : QString());

		if (!fresh.isEmpty())
			emit appended(fresh);
	});
}

void XiveChatFeed::send(const QString &content,
			std::function<void(bool ok, const QString &error)> cb)
{
	XiveApi *api = XiveApi::instance();
	XiveChannel *ch = XiveChannel::instance();
	if (!api || !ch) {
		cb(false, tr("The Xive plugin is not ready yet."));
		return;
	}

	const QString streamId = ch->streamId();
	if (streamId.isEmpty()) {
		cb(false, tr("No Xive channel to send to yet."));
		return;
	}

	api->chatSend(streamId, content,
		      [this, cb](bool ok, const QJsonObject &, int, const QString &error) {
			      if (!ok) {
				      /* The send route runs a blocklist and a strike ladder, so a
				       * refusal here is meaningful to the sender. */
				      cb(false, error);
				      return;
			      }
			      cb(true, QString());
			      /* Poll straight away so the sender sees their own message land. */
			      pollNow();
		      });
}

/* ------------------------------------------------------------------------- */
/* Moderation                                                                */
/* ------------------------------------------------------------------------- */

/*
 * Every action is the same four steps — check we are ready, resolve the channel, call, re-poll —
 * so they share one body and differ only in which call they make. `issue` receives the resolved
 * stream id and the callback that finishes the action.
 */
void XiveChatFeed::runAction(
	const std::function<void(const QString &streamId, XiveApi::Callback done)> &issue,
	ActionCallback cb)
{
	XiveApi *api = XiveApi::instance();
	XiveChannel *ch = XiveChannel::instance();
	if (!api || !ch) {
		cb(false, tr("The Xive plugin is not ready yet."));
		return;
	}

	const QString streamId = ch->streamId();
	if (streamId.isEmpty()) {
		cb(false, tr("No Xive channel yet."));
		return;
	}

	issue(streamId, [this, cb](bool ok, const QJsonObject &, int, const QString &error) {
		if (!ok) {
			cb(false, error);
			return;
		}
		cb(true, QString());
		/* A ban or a timeout changes nothing already on screen, but it does change what the
		 * next poll returns, and a moderator watching wants to see that promptly. */
		pollNow();
	});
}

void XiveChatFeed::deleteMessage(const QString &messageId, ActionCallback cb)
{
	runAction(
		[messageId](const QString &streamId, XiveApi::Callback done) {
			XiveApi::instance()->chatRecall(streamId, messageId, std::move(done));
		},
		[this, messageId, cb](bool ok, const QString &error) {
			if (ok) {
				/*
				 * Drop it locally. The poll only ever appends what it has not seen, so
				 * a deleted message would otherwise stay on screen until the dock was
				 * rebuilt for some other reason. The id stays in m_seen deliberately:
				 * it must not reappear.
				 */
				for (int i = 0; i < m_messages.size(); ++i) {
					if (m_messages.at(i).value(QStringLiteral("id")).toString() ==
					    messageId) {
						m_messages.removeAt(i);
						break;
					}
				}
				emit rebuilt();
			}
			cb(ok, error);
		});
}

void XiveChatFeed::editMessage(const QString &messageId, const QString &newContent,
			      EditCallback cb)
{
	const QString trimmed = newContent.trimmed();

	/*
	 * Validated BEFORE the delete, which is the whole reason this is a method and not two calls
	 * at the call site. Deleting first and then discovering the replacement is empty or too long
	 * would destroy a message for nothing — and there is no undo, because the delete is a recall
	 * and the original text now exists only in whatever the caller happened to keep.
	 *
	 * 500 is the server's own ceiling and the same one the composer enforces; the web guards it
	 * in editMessage() for this identical reason.
	 */
	if (trimmed.isEmpty()) {
		cb(false, false, tr("An edited message cannot be empty. Delete it instead."));
		return;
	}
	if (trimmed.length() > 500) {
		cb(false, false, tr("An edited message can be at most 500 characters."));
		return;
	}

	deleteMessage(messageId, [this, trimmed, cb](bool ok, const QString &error) {
		if (!ok) {
			/* Nothing has happened yet — the original is intact and the caller still has
			 * the draft. This is the harmless half of the failure space. */
			cb(false, false, error);
			return;
		}

		send(trimmed, [cb](bool sent, const QString &sendError) {
			if (sent) {
				cb(true, true, QString());
				return;
			}
			/*
			 * 🔴 THE DAMAGING CASE. The original is deleted and the replacement did not
			 * land, so the creator's line is gone from the broadcast. Reported with
			 * `originalDeleted` true, which is the whole reason that flag exists: the
			 * caller must NOT retry this as an edit — there is nothing left to delete —
			 * and must keep the text, because it is now the only copy of it anywhere.
			 */
			cb(false, true,
			   tr("Your message was deleted but the edit could not be sent: %1")
				   .arg(sendError));
		});
	});
}

void XiveChatFeed::timeoutUser(const QString &userId, int minutes, ActionCallback cb)
{
	runAction(
		[userId, minutes](const QString &streamId, XiveApi::Callback done) {
			XiveApi::instance()->chatTimeout(streamId, userId, minutes, std::move(done));
		},
		std::move(cb));
}

void XiveChatFeed::unmuteUser(const QString &userId, ActionCallback cb)
{
	runAction(
		[userId](const QString &streamId, XiveApi::Callback done) {
			XiveApi::instance()->chatUnmute(streamId, userId, std::move(done));
		},
		std::move(cb));
}

void XiveChatFeed::banUser(const QString &userId, ActionCallback cb)
{
	runAction(
		[userId](const QString &streamId, XiveApi::Callback done) {
			XiveApi::instance()->chatBan(streamId, userId, std::move(done));
		},
		std::move(cb));
}

void XiveChatFeed::unbanUser(const QString &userId, ActionCallback cb)
{
	runAction(
		[userId](const QString &streamId, XiveApi::Callback done) {
			XiveApi::instance()->chatUnban(streamId, userId, std::move(done));
		},
		std::move(cb));
}

void XiveChatFeed::addModerator(const QString &userId, ActionCallback cb)
{
	runAction(
		[userId](const QString &streamId, XiveApi::Callback done) {
			XiveApi::instance()->chatAddModerator(streamId, userId, std::move(done));
		},
		std::move(cb));
}

void XiveChatFeed::removeModerator(const QString &userId, ActionCallback cb)
{
	runAction(
		[userId](const QString &streamId, XiveApi::Callback done) {
			XiveApi::instance()->chatRemoveModerator(streamId, userId, std::move(done));
		},
		std::move(cb));
}

void XiveChatFeed::pinMessage(const QString &messageId, int durationMinutes, bool untilStreamEnd,
			     ActionCallback cb)
{
	runAction(
		[messageId, durationMinutes, untilStreamEnd](const QString &streamId,
							     XiveApi::Callback done) {
			XiveApi::instance()->chatPin(streamId, messageId, durationMinutes,
						     untilStreamEnd, std::move(done));
		},
		std::move(cb));
}

void XiveChatFeed::unpinMessage(ActionCallback cb)
{
	runAction(
		[](const QString &streamId, XiveApi::Callback done) {
			XiveApi::instance()->chatUnpin(streamId, std::move(done));
		},
		std::move(cb));
}

/* ------------------------------------------------------------------------- */
/* Realtime (uni_sse) ingestion                                              */
/* ------------------------------------------------------------------------- */

void XiveChatFeed::ingestRealtimeMessage(const QJsonObject &message)
{
	/* Only messages for the channel we are showing; a stale one from the moment the stream id
	 * changed is dropped rather than mixed in. */
	const QString sid = message.value(QStringLiteral("stream_id")).toString();
	if (!m_streamId.isEmpty() && !sid.isEmpty() && sid != m_streamId)
		return;

	const QString id = message.value(QStringLiteral("id")).toString();
	/* No id cannot be deduped; an id already seen was delivered by the poll first — either way,
	 * nothing to do. This is the same `m_seen` the poll uses, which is what stops a double-show. */
	if (id.isEmpty() || m_seen.contains(id))
		return;

	m_seen.insert(id);
	m_messages.append(message);

	/* Same retention bound as the poll in tick(). */
	if (m_messages.size() > kMaxRetained) {
		const int excess = m_messages.size() - kMaxRetained;
		for (int i = 0; i < excess; ++i)
			m_seen.remove(m_messages.at(i).value(QStringLiteral("id")).toString());
		m_messages.erase(m_messages.begin(), m_messages.begin() + excess);
	}

	/* A quiet notice reset, in case the dock was showing "No chat messages yet." */
	emit notice(QString());
	emit appended(QList<QJsonObject>{message});
}

void XiveChatFeed::applyRealtimeDelete(const QString &messageId)
{
	for (int i = 0; i < m_messages.size(); ++i) {
		if (m_messages.at(i).value(QStringLiteral("id")).toString() == messageId) {
			m_messages.removeAt(i);
			/* The id stays in m_seen deliberately: a poll already in flight when the delete
			 * landed must not re-add it. The server has removed the row, so no later poll
			 * carries it either. */
			emit rebuilt();
			break;
		}
	}
}

void XiveChatFeed::setRealtimeConnected(bool connected)
{
	if (connected == m_realtimeConnected)
		return;
	m_realtimeConnected = connected;

	/* Slow reconciliation cadence while the socket delivers; the configured fast poll when it
	 * does not. Restart a running timer so the change takes effect now rather than after one
	 * more interval at the old rate. */
	const int ms = connected ? kChatLivePollMs : XiveConfig::instance().chatPollMs();
	m_timer->setInterval(ms);
	if (m_timer->isActive()) {
		m_timer->stop();
		m_timer->start();
	}

	/* The socket just dropped: re-read immediately so a gap left by the disconnect is closed now
	 * rather than at the next tick. */
	if (!connected && m_viewers > 0)
		pollNow();
}
