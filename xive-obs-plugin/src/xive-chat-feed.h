#pragma once

#include "xive-api.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

class QTimer;

/*
 * One shared, polled copy of the current broadcast's chat.
 *
 * Both the chat dock and the activity dock are views of the same `chat/messages` payload — the
 * activity feed is nothing but a classification of chat (see xive-activity.h). Letting each dock
 * poll on its own timer would double the request rate against the API for one broadcaster's worth
 * of information, and would let the two docks disagree about what had been said.
 *
 * So the polling lives here, once, and the docks render from it.
 */
class XiveChatFeed : public QObject {
	Q_OBJECT

public:
	explicit XiveChatFeed(QObject *parent = nullptr);

	static XiveChatFeed *instance();
	static void createInstance();
	static void destroyInstance();

	/* Poll while at least one viewer is interested. Each dock registers and unregisters as it
	 * is shown or hidden, so a collapsed pair of docks costs nothing. */
	void addViewer();
	void removeViewer();

	const QList<QJsonObject> &messages() const { return m_messages; }

	/*
	 * The pinned message, or an empty object when nothing is pinned.
	 *
	 * 🔴 Served SEPARATELY from `messages` by the API, and kept separate here for the same reason:
	 * a pin can be older than the newest 50 and therefore absent from the list entirely — which is
	 * exactly when the banner earns its place. Never look for it in messages().
	 *
	 * It carries the full message shape plus `pinned_by`, `pinned_at`, `pin_expires_at` and
	 * `pin_until_stream_end`, and its `id` matches the copy in the scrollback when there is one.
	 */
	const QJsonObject &pinnedMessage() const { return m_pinned; }

	/*
	 * Whether this caller may pin, straight from the API's `can_pin`.
	 *
	 * NOT derivable here: pinning is gated on the HOST's Xive Pro, which the plugin cannot see.
	 * Guessing from "am I the creator" would light the menu item up on a free channel and then
	 * 403 when it was used.
	 */
	bool canPin() const { return m_canPin; }

	void pollNow();
	void send(const QString &content, std::function<void(bool ok, const QString &error)> cb);

	/*
	 * Realtime (uni_sse) hooks, wired to XiveRealtime in plugin-main.
	 *
	 * A pushed message is deduped and appended exactly like a polled one (same `m_seen`), so the
	 * socket and the poll can never double-show a message. A pushed delete drops it locally. The
	 * connected flag backs the poll off to a slow reconciliation cadence while the socket carries
	 * chat, and restores the 3s poll — with an immediate catch-up read — the moment it drops.
	 */
	void ingestRealtimeMessage(const QJsonObject &message);
	void applyRealtimeDelete(const QString &messageId);
	void setRealtimeConnected(bool connected);

	using ActionCallback = std::function<void(bool ok, const QString &error)>;

	/*
	 * The moderation actions, as the chat dock needs them: the stream id is filled in here
	 * rather than at every call site, and a success re-polls so the dock reflects the change
	 * without the moderator wondering whether it took.
	 *
	 * `deleteMessage` also drops the message from the retained buffer and emits `rebuilt()`,
	 * because the poll only ever ADDS: a deleted message is simply absent from the next
	 * response, and nothing would otherwise take it off the screen.
	 */
	void deleteMessage(const QString &messageId, ActionCallback cb);

	/*
	 * Edit one of your own messages.
	 *
	 * 🔴 THERE IS NO EDIT ENDPOINT, AND THIS IS NOT A WORKAROUND — it is how the platform defines
	 * the feature. `editMessage()` in hooks/streaming/useStreamChat.ts is `deleteMessage()` then
	 * `sendMessage()`, and chat_moderation.php says so in the comment above handleStreamChatRecall.
	 * Two consequences the UI has to own rather than hide:
	 *
	 *   - THE EDIT IS A NEW MESSAGE. New id, new timestamp, and it lands at the BOTTOM of chat
	 *     rather than changing in place. Anyone who read the original already read the original.
	 *   - THE TWO CALLS ARE NOT ATOMIC. If the delete lands and the send fails — a network blip,
	 *     a mute applied in between — the original is gone and the replacement never arrives. The
	 *     creator's words are then only in whatever the caller kept, which is why the failure is
	 *     reported with a distinct message and why the dock does not clear its composer until this
	 *     succeeds.
	 *
	 * Refuses empty content and anything over 500 characters before touching the network, matching
	 * the web's guard — deleting first and discovering the replacement is invalid afterwards would
	 * destroy a message to no purpose.
	 */
	/*
	 * `originalDeleted` is what makes the failure recoverable, and it is why this does not reuse
	 * ActionCallback. Two failures are possible and they need opposite responses:
	 *
	 *   false — the recall failed. Nothing happened; the original is still in chat. Retrying the
	 *           whole edit is correct.
	 *   true  — the recall landed and the re-send did not. The original is GONE, so retrying the
	 *           edit would try to delete it a second time and never reach the send. The only
	 *           sensible retry is to post the draft as a NEW message, which is what the dock
	 *           switches to.
	 *
	 * It is true on success too, trivially, since both halves ran.
	 */
	using EditCallback = std::function<void(bool ok, bool originalDeleted, const QString &error)>;

	void editMessage(const QString &messageId, const QString &newContent, EditCallback cb);
	void timeoutUser(const QString &userId, int minutes, ActionCallback cb);
	void unmuteUser(const QString &userId, ActionCallback cb);
	void banUser(const QString &userId, ActionCallback cb);
	/*
	 * ⚠️ WIDER THAN ITS NAME. The server clears the ban across EVERY channel this creator owns,
	 * not just this broadcast (see handleStreamChatUnban) — deliberately, because banning is
	 * creator-wide too and a per-stream lift would leave an un-undoable ban on the other channels.
	 * Reached from the blocked-viewers list, never from the message menu: a ban purges the
	 * viewer's messages, so there is no line left to right-click.
	 */
	void unbanUser(const QString &userId, ActionCallback cb);
	void addModerator(const QString &userId, ActionCallback cb);
	void removeModerator(const QString &userId, ActionCallback cb);
	/* `durationMinutes` <= 0 pins until someone unpins. See XiveApi::chatPin. */
	void pinMessage(const QString &messageId, int durationMinutes, bool untilStreamEnd,
			ActionCallback cb);
	void unpinMessage(ActionCallback cb);

signals:
	/* Emitted when new messages arrived, with just the new ones so the chat dock can append
	 * rather than redraw. */
	void appended(const QList<QJsonObject> &newMessages);
	/* Emitted when the conversation was reset — a different channel, so views must clear. */
	void reset();
	/* Emitted when the retained buffer changed in a way appending cannot express — today, a
	 * message being deleted. Views must redraw from messages() rather than append. */
	void rebuilt();
	/* Human-readable status, or empty when things are fine. */
	void notice(const QString &message);
	/*
	 * The pinned message changed — including to nothing, which arrives as an empty object.
	 *
	 * Emitted only on an actual change (compared by message id), because the poll re-reads the
	 * pin every few seconds and a signal per tick would redraw the banner continuously.
	 */
	void pinChanged(const QJsonObject &pin);

private:
	void tick();
	/* The shared body of the seven actions above — see the note in the .cpp. */
	void runAction(const std::function<void(const QString &streamId, XiveApi::Callback done)> &issue,
		       ActionCallback cb);

	QTimer *m_timer = nullptr;
	QList<QJsonObject> m_messages;
	QJsonObject m_pinned;
	bool m_canPin = false;
	QSet<QString> m_seen;
	QString m_streamId;
	int m_viewers = 0;
	bool m_inFlight = false;
	bool m_realtimeConnected = false;
};
