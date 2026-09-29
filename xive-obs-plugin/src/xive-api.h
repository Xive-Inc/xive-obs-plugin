#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class XiveAuth;

/*
 * Thin client over api.thexive.com.
 *
 * Every call is asynchronous and hands the callback four things: whether the HTTP round trip
 * succeeded, the decoded body, the status code, and a human-readable error.
 *
 * `ok` means only "the transport and the status line were fine". Several Xive routes answer
 * HTTP 200 with `{"success": false}` — notably chat/messages, whose catch-all deliberately
 * returns an empty list rather than an error so a chat widget does not show a failure over the
 * video. Callers must check `success` in the body themselves; this client will not conflate the
 * two, because "the request failed" and "the stream has no chat yet" call for different UI.
 */
class XiveApi : public QObject {
	Q_OBJECT

public:
	using Callback = std::function<void(bool ok, const QJsonObject &body, int status,
					    const QString &error)>;

	explicit XiveApi(XiveAuth *auth, QObject *parent = nullptr);

	static XiveApi *instance();
	static void createInstance(XiveAuth *auth);
	static void destroyInstance();

	void get(const QString &path, Callback cb);
	void post(const QString &path, const QJsonObject &body, Callback cb);

	/* --- Encoder credentials ------------------------------------------------------- */

	/*
	 * Find-or-create the creator's permanent OBS channel and return its ingest URL + key.
	 * Refuses to reuse a row that is currently LIVE and mints a new channel instead, so it
	 * must NOT be called while a broadcast is in progress — that is what produced the
	 * "dashboard says offline while /stream plays fine" incident on 2026-08-09.
	 *
	 * `title` is required by the server. A category is optional: name no category and the route
	 * keeps the channel filed exactly where it already was, rather than inventing one.
	 *
	 * `categorySlug` names the catalogue row losslessly, same as on updateMetadata, and is
	 * preferred over the name for the reason spelled out there — a slug also carries the 23
	 * catalogue names that are longer than this route's 64-character cap on the text field.
	 */
	void obsSetup(const QString &title, const QString &category, const QString &categorySlug,
		      Callback cb);

	/*
	 * Read back the existing channel's credentials. Resolves by CREATOR, not by any id the
	 * client supplies, and prefers a live channel over an idle one. Never rotates the key and
	 * never creates a row — this is the safe call, and the one to use on dock startup.
	 */
	void obsRefreshCredentials(Callback cb);

	/* Rotate the ingest key. Destructive: every encoder still configured with the old key
	 * stops being able to publish, so the UI must confirm before calling this. */
	void obsRotateKey(const QString &title, const QString &category, const QString &categorySlug,
			  Callback cb);

	/* --- Channel state ------------------------------------------------------------- */

	void streamDetail(const QString &streamId, Callback cb);
	void myActive(Callback cb);

	/* --- Go-live notification ------------------------------------------------------- */

	/*
	 * The creator's channel row — read for `goLiveNotify` and `goLiveMessage`.
	 *
	 * ⚠️ A POST, AND THAT IS NOT A MISTAKE. The obvious call is the public
	 * `GET /api/streaming/channels/{creatorId}`, which this plugin cannot make: it has no
	 * profiles.id. The stream row's `creator_id` is NOT one either — detail.php resolves a
	 * profile from it with `WHERE auth_user_id = :c`, so it is an auth user id and the GET would
	 * 404 on it.
	 *
	 * The write route has neither problem. It resolves the creator from the token
	 * (`$auth['profile_id']`), it is a PARTIAL update where an omitted field is left untouched —
	 * so an empty body changes nothing — and it answers by reading the row back in full. One
	 * call, no identity plumbing, and the only side effect is that a creator who has never saved
	 * a channel gets a row of defaults, which is what their next save would have created anyway.
	 */
	void creatorChannel(Callback cb);

	/*
	 * Save just the go-live notification. Every other channel field is omitted and therefore
	 * left alone — see the COALESCE upsert in app/streaming/channels.php, which exists precisely
	 * so one panel can save without the others resending their values.
	 *
	 * The message is capped at SC_NOTIFY_MESSAGE_MAX (200) server-side; over that the route
	 * answers a ValidationError rather than truncating, so the caller has to hold the same limit.
	 */
	void saveGoLiveNotification(bool notify, const QString &message, Callback cb);

	/*
	 * Write title + category onto the broadcast.
	 *
	 * `categorySlug` is the public identifier of a public.stream_categories row and is what
	 * makes the server resolve `category_id` exactly. Send it empty when the creator typed free
	 * text instead of picking: the server then matches on the display name, which is what every
	 * client did before the catalogue existed. Never send an empty string as a slug — an
	 * explicit slug that resolves to nothing is a 400, deliberately (see smResolveCategory in
	 * app/streaming/writes.php).
	 */
	void updateMetadata(const QString &streamId, const QString &title, const QString &category,
			    const QString &categorySlug, Callback cb);

	/* --- Category directory ---------------------------------------------------------
	 *
	 * public.stream_categories — ~16k IGDB games plus hand-curated categories. Public read, so
	 * this is the one route here that would work signed out; it still goes through the bearer
	 * path because everything else in the dock needs a session anyway.
	 *
	 * An empty `query` is the browse ordering (most live first), which is what to show before
	 * the creator has typed anything.
	 */
	void categories(const QString &query, int limit, Callback cb);

	/* --- Activity feed --------------------------------------------------------------
	 *
	 * The broadcast's follows, gifts, Sparks and supporter unlocks, newest first, already
	 * rendered (`description`, `amount_label`, `filter`) as well as structured.
	 *
	 * ⚠️ THIS ROUTE IS WHY xive-activity.cpp NO LONGER OWNS A REGEX TABLE. There was no activity
	 * endpoint before, so this plugin reconstructed the feed by classifying chat text with a
	 * pattern list hand-mirrored from the website's TypeScript. That could only ever recover
	 * gifts — follows and Sparks wrote no chat line at all — and it invented events whenever a
	 * viewer typed "clip". Server side: api/app/streaming/activity.php.
	 *
	 * CREATOR-ONLY: answers 403 to anyone who does not own the stream.
	 *
	 * `since` is a poll cursor, not a window. Pass the newest `created_at` already held and only
	 * strictly-newer rows come back; pass an empty string for the first load.
	 */
	void streamActivity(const QString &streamId, const QString &since, Callback cb);

	/* --- Xive Surge ------------------------------------------------------------------
	 *
	 * The broadcast's current surge — level, points, participants, contributors and the deadline
	 * — in the same shape the `stream:<id>` publication carries, from the same server function.
	 * A client cannot tell which of the two it is looking at, deliberately.
	 *
	 * ⚠️ PUBLIC, NO BEARER REQUIRED server-side: the banner exists for viewers, most of whom are
	 * signed out. It still goes through the bearer path here because the dock has a session
	 * anyway, and one code path is one fewer thing to get wrong.
	 *
	 * ⚠️ AN ENDED BROADCAST ANSWERS `active: false` WITHOUT SCORING ANYTHING, so this is cheap to
	 * call on an idle channel. Do not gate it on `isLive()` and then skip it — a stream that goes
	 * live while the dock is open needs the read.
	 */
	void streamSurge(const QString &streamId, Callback cb);

	/* --- Chat ---------------------------------------------------------------------- */

	void chatMessages(const QString &streamId, Callback cb);
	void chatSend(const QString &streamId, const QString &content, Callback cb);
	void chatModerators(const QString &streamId, Callback cb);

	/*
	 * The moderation roster: `moderators`, `bans` and `timeouts`, each an array carrying
	 * `user_id`, `username`, `display_name`, `created_at`, and — for bans and timeouts —
	 * `reason`, `expires_at` and who did it.
	 *
	 * 🔴 THIS IS THE ONLY WAY TO REACH A BANNED VIEWER. Banning publishes a purge, so their
	 * messages leave the feed — there is no line left to right-click, and without this route an
	 * unblock could not be offered at all. The web has the same problem and solves it the same
	 * way (StudioCommunitySection reads this route, not the chat buffer).
	 *
	 * ⚠️ `bans` IS SCOPED TO THIS STREAM WHILE THE BAN ITSELF IS NOT. A ban covers every channel
	 * its creator owns and `chat/unban` clears all of them, but this list shows only the channel
	 * asked about — deliberately, so one channel's roster does not display another's. Do not
	 * present the count as "everyone this creator has blocked".
	 *
	 * Timeouts already elapsed are dropped server-side rather than flagged: a lapsed mute is not
	 * a restriction anybody is enforcing.
	 *
	 * Creator or moderator only; anyone else gets 403.
	 */
	void chatRestrictions(const QString &streamId, Callback cb);

	/* --- Chat moderation ------------------------------------------------------------
	 *
	 * These seven answered a routing 404 until 2026-08-09 — they were never ported off Deno and
	 * the omission went unnoticed because the migration was scheduled off traffic, which a dead
	 * route does not generate. The website's own moderation menu called them too, so this was
	 * broken everywhere, not merely missing here. Server side: app/streaming/chat_moderation.php.
	 *
	 * The gate is the CREATOR or a per-stream moderator, except add/remove-moderator which are
	 * creator-only. `recall` is the one a non-moderator may use, on their own message.
	 */

	/* Delete one message. The web builds "edit" from this plus a re-send. */
	void chatRecall(const QString &streamId, const QString &messageId, Callback cb);
	/* Minutes are clamped server-side to 1..60; 0 and nonsense both mean 5. */
	void chatTimeout(const QString &streamId, const QString &userId, int minutes, Callback cb);
	void chatUnmute(const QString &streamId, const QString &userId, Callback cb);
	void chatBan(const QString &streamId, const QString &userId, Callback cb);
	void chatUnban(const QString &streamId, const QString &userId, Callback cb);
	void chatAddModerator(const QString &streamId, const QString &userId, Callback cb);
	void chatRemoveModerator(const QString &streamId, const QString &userId, Callback cb);

	/*
	 * Pin one message to the top of chat, and clear it.
	 *
	 * 🔴 Pinning is gated on the HOST'S Xive Pro, not the caller's — a moderator on a Pro channel
	 * may pin, and the creator of a free channel may not. The denial arrives as 403 with
	 * `code: PRO_REQUIRED`, which is worth showing the broadcaster; a plain "Forbidden" means the
	 * menu item should not have been offered at all. Read `can_pin` from chat/messages rather than
	 * guessing, because the plugin cannot see the host's entitlement.
	 *
	 * `untilStreamEnd` is refused with 400 CHAT_PIN_STREAM_NOT_LIVE when the stream is not live —
	 * such a pin would be stored and then hidden immediately, which reads as the feature being
	 * broken. Pass false to pin until someone unpins.
	 *
	 * Unpin has NO Pro check server-side, deliberately: a creator whose Pro lapsed must still be
	 * able to take a pin down.
	 */
	/*
	 * `durationMinutes` is clamped server-side to 1..1440 (StreamChatPins::MIN/MAX_DURATION_MINUTES)
	 * and is OMITTED when zero or less — omission is what "until someone unpins" actually is on the
	 * wire, and the server reads a missing key as exactly that. Mutually exclusive with
	 * `untilStreamEnd` in practice, though the server would accept both.
	 */
	void chatPin(const QString &streamId, const QString &messageId, int durationMinutes,
		     bool untilStreamEnd, Callback cb);
	void chatUnpin(const QString &streamId, Callback cb);

	/* --- Gifts / wallet ------------------------------------------------------------ */

	void giftsLeaderboard(const QString &streamId, Callback cb);
	void giftsCatalog(Callback cb);
	void walletBalance(Callback cb);

	/* --- Gift overlay ----------------------------------------------------------------
	 *
	 * The creator's own browser-source link for the burn-in gift overlay. Answers
	 * `{ success, path, version }`.
	 *
	 * 🔴 `path` IS ORIGIN-RELATIVE AND IS NOT A URL. The API has no idea which site
	 * origin the caller is looking at, so it deliberately returns only the path and the
	 * caller composes it against the SITE origin — which is not this client's own
	 * apiBase(). XiveOverlaySource::linkUrl() is the one place that composition happens.
	 *
	 * 🔴 THE PATH CARRIES A BEARER CREDENTIAL in its `?t=`: a read-only grant over one
	 * creator's gift feed, valid until the link is rotated. Never log it.
	 *
	 * 503 means no overlay signing secret is configured on this environment. That is a
	 * deployment fact and not something the creator can retry their way out of, so it is
	 * worth saying so rather than showing a generic failure. See
	 * api/app/streaming/overlay_link.php.
	 */
	void overlayLink(Callback cb);

private:
	void send(const QByteArray &verb, const QString &path, const QJsonObject *body,
		  Callback cb, bool allowRetry);

	QNetworkAccessManager *m_nam = nullptr;
	XiveAuth *m_auth = nullptr;
};
