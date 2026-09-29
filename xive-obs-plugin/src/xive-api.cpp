#include "xive-api.h"
#include "xive-auth.h"
#include "xive-config.h"
#include "xive-log.h"

#include <QBuffer>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {
XiveApi *g_instance = nullptr;
}

XiveApi::XiveApi(XiveAuth *auth, QObject *parent) : QObject(parent), m_auth(auth)
{
	m_nam = new QNetworkAccessManager(this);
}

XiveApi *XiveApi::instance()
{
	return g_instance;
}

void XiveApi::createInstance(XiveAuth *auth)
{
	if (!g_instance)
		g_instance = new XiveApi(auth);
}

void XiveApi::destroyInstance()
{
	delete g_instance;
	g_instance = nullptr;
}

void XiveApi::get(const QString &path, Callback cb)
{
	send("GET", path, nullptr, std::move(cb), true);
}

void XiveApi::post(const QString &path, const QJsonObject &body, Callback cb)
{
	send("POST", path, &body, std::move(cb), true);
}

void XiveApi::send(const QByteArray &verb, const QString &path, const QJsonObject *body,
		   Callback cb, bool allowRetry)
{
	/* Copy the body: the caller's object may be a temporary, and the lambda below outlives
	 * this stack frame. */
	const QByteArray payload = body ? QJsonDocument(*body).toJson(QJsonDocument::Compact)
					: QByteArray();
	const bool hasBody = body != nullptr;

	m_auth->withAccessToken([this, verb, path, payload, hasBody, cb, allowRetry](
					const QString &token) {
		if (token.isEmpty()) {
			cb(false, QJsonObject(), 0,
			   QObject::tr("You are not signed in to Xive."));
			return;
		}

		QNetworkRequest req(QUrl(XiveConfig::apiBase() + path));
		req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
		req.setRawHeader("Accept", "application/json");
		req.setRawHeader("User-Agent",
				 QStringLiteral("xive-obs-plugin/%1")
					 .arg(QStringLiteral(XIVE_PLUGIN_VERSION))
					 .toUtf8());
		if (hasBody)
			req.setHeader(QNetworkRequest::ContentTypeHeader,
				      QStringLiteral("application/json"));
		req.setTransferTimeout(20000);

		QNetworkReply *reply = nullptr;
		if (verb == "GET") {
			reply = m_nam->get(req);
		} else {
			reply = m_nam->sendCustomRequest(req, verb, payload);
		}

		connect(reply, &QNetworkReply::finished, this,
			[this, reply, verb, path, payload, hasBody, cb, allowRetry]() {
				reply->deleteLater();

				const int status =
					reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
						.toInt();
				const QByteArray raw = reply->readAll();

				/*
				 * A 401 means the server disagrees with our expiry arithmetic —
				 * clock skew, or the token was revoked. Refresh once and replay.
				 * Only once: a refresh that keeps yielding 401s is a real auth
				 * failure, and retrying it in a loop would hammer Authentik.
				 */
				if (status == 401 && allowRetry) {
					xive_info("got 401 on %s; refreshing once and retrying",
						  qUtf8Printable(path));
					m_auth->forceRefresh([this, verb, path, payload, hasBody,
							      cb](const QString &fresh) {
						if (fresh.isEmpty()) {
							cb(false, QJsonObject(), 401,
							   QObject::tr("Your Xive sign-in expired."));
							return;
						}
						const QJsonObject parsed =
							QJsonDocument::fromJson(payload).object();
						send(verb, path, hasBody ? &parsed : nullptr, cb,
						     false);
					});
					return;
				}

				if (status == 0) {
					const QString err = reply->errorString();
					xive_warn("%s %s failed at the transport layer",
						  verb.constData(), qUtf8Printable(path));
					cb(false, QJsonObject(), 0,
					   QObject::tr("Could not reach Xive: %1").arg(err));
					return;
				}

				const QJsonDocument doc = QJsonDocument::fromJson(raw);
				const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject();

				if (status < 200 || status >= 300) {
					/* Streaming routes answer with a plain string `error`;
					 * the structured envelope uses an object. Handle both. */
					QString message;
					const QJsonValue errVal = obj.value(QStringLiteral("error"));
					if (errVal.isString())
						message = errVal.toString();
					else if (errVal.isObject())
						message = errVal.toObject()
								  .value(QStringLiteral("message"))
								  .toString();

					if (message.isEmpty())
						message = QObject::tr("Xive returned HTTP %1.")
								  .arg(status);

					xive_warn("%s %s -> HTTP %d", verb.constData(),
						  qUtf8Printable(path), status);
					cb(false, obj, status, message);
					return;
				}

				cb(true, obj, status, QString());
			});
	});
}

/* ------------------------------------------------------------------------- */
/* Encoder credentials                                                       */
/* ------------------------------------------------------------------------- */

namespace {

/*
 * `obs/setup` caps the free-text `category` at 64 characters, and the 23 listed categories with
 * longer names would 400 the whole call.
 *
 * Dropping the field is the right failure mode rather than truncating it: setup REUSES the
 * category already on the row whenever the request omits one, so an over-long name leaves the
 * stored category exactly as the manager dock last wrote it. Truncating would instead overwrite a
 * correct category with a prefix of itself that matches no catalogue row at all.
 *
 * Only reached when there is no slug. A slug bypasses the cap entirely — see setupApplyCategory().
 */
QString setupSafeCategory(const QString &category)
{
	return category.length() > 64 ? QString() : category;
}

/*
 * The category half of a setup body: the slug when there is one, the capped free text otherwise.
 *
 * Exactly one of the two, never both, for the same reason updateMetadata sends one — the 64-char
 * cap is applied to `category` BEFORE the slug is resolved, so echoing a long catalogue name back
 * alongside its slug would 400 on a category the server itself handed us.
 *
 * Naming neither is a meaningful third state and must stay reachable: setup carries the row's
 * existing `category_id` forward when a call names no category, which is what keeps the bare
 * credential-fetch call from re-filing the channel.
 */
void setupApplyCategory(QJsonObject &body, const QString &category, const QString &categorySlug)
{
	if (!categorySlug.isEmpty()) {
		body[QStringLiteral("categorySlug")] = categorySlug;
		return;
	}
	const QString safeCategory = setupSafeCategory(category);
	if (!safeCategory.isEmpty())
		body[QStringLiteral("category")] = safeCategory;
}

} // namespace

void XiveApi::obsSetup(const QString &title, const QString &category, const QString &categorySlug,
		       Callback cb)
{
	QJsonObject body;
	/* The server rejects an empty title with 400, so never send one. */
	body[QStringLiteral("title")] = title.isEmpty() ? QStringLiteral("Live on Xive") : title;
	setupApplyCategory(body, category, categorySlug);

	post(QStringLiteral("/api/streaming/obs/setup"), body, std::move(cb));
}

void XiveApi::obsRefreshCredentials(Callback cb)
{
	/* The handler resolves the channel by creator and reads no body fields, but it is a POST
	 * route, so it still needs a body. */
	post(QStringLiteral("/api/streaming/obs/refresh-credentials"), QJsonObject(), std::move(cb));
}

void XiveApi::obsRotateKey(const QString &title, const QString &category,
			   const QString &categorySlug, Callback cb)
{
	QJsonObject body;
	body[QStringLiteral("title")] = title.isEmpty() ? QStringLiteral("Live on Xive") : title;
	setupApplyCategory(body, category, categorySlug);

	/* Both flags are required together — the server treats the confirmation as part of the
	 * request so a rotation can never happen from a single stray click. */
	body[QStringLiteral("regenerate_stream_key")] = true;
	body[QStringLiteral("regenerate_confirmed")] = true;

	post(QStringLiteral("/api/streaming/obs/setup"), body, std::move(cb));
}

/* ------------------------------------------------------------------------- */
/* Channel state                                                             */
/* ------------------------------------------------------------------------- */

void XiveApi::streamDetail(const QString &streamId, Callback cb)
{
	get(QStringLiteral("/api/streaming/obs/%1").arg(streamId), std::move(cb));
}

void XiveApi::myActive(Callback cb)
{
	get(QStringLiteral("/api/streaming/my-active"), std::move(cb));
}

/* Where both calls below go. One route serves the read and the write — see the header. */
static const char *kCreatorChannelPath = "/api/streaming/creator/channel";

void XiveApi::creatorChannel(Callback cb)
{
	/* Empty body: a partial update that supplies nothing changes nothing, and answers with the
	 * whole stored channel. */
	post(QString::fromUtf8(kCreatorChannelPath), QJsonObject(), std::move(cb));
}

void XiveApi::saveGoLiveNotification(bool notify, const QString &message, Callback cb)
{
	QJsonObject body;
	body[QStringLiteral("goLiveNotify")] = notify;

	/*
	 * ⚠️ ALWAYS SENT, EVEN EMPTY — and it has to be a string, not omitted.
	 *
	 * Omitting a field means "leave it alone" on this route, so skipping an empty message would
	 * make clearing the message impossible: the creator would delete the text, save, and find
	 * the old copy still there next time. An empty string is a value, and COALESCE takes it.
	 */
	body[QStringLiteral("goLiveMessage")] = message;

	post(QString::fromUtf8(kCreatorChannelPath), body, std::move(cb));
}

void XiveApi::updateMetadata(const QString &streamId, const QString &title,
			     const QString &category, const QString &categorySlug, Callback cb)
{
	QJsonObject body;
	body[QStringLiteral("streamId")] = streamId;
	body[QStringLiteral("title")] = title;

	/*
	 * Exactly one of the two, never both.
	 *
	 * The server applies its 64-character cap to the `category` TEXT before it resolves the
	 * slug, and 23 listed catalogue rows have names longer than that (the longest is 108 —
	 * see the comment above smResolveCategory in app/streaming/writes.php). Echoing the picked
	 * name back alongside the slug would therefore 400 on exactly those rows, for a category
	 * the server itself handed us.
	 *
	 * Sending the slug alone avoids it and is strictly better anyway: on a slug hit the server
	 * overwrites `category` with the catalogue's own spelling regardless, so the text we would
	 * have sent is discarded. Free text keeps the old path, cap and all.
	 */
	if (!categorySlug.isEmpty())
		body[QStringLiteral("categorySlug")] = categorySlug;
	else if (!category.isEmpty())
		body[QStringLiteral("category")] = category;

	post(QStringLiteral("/api/streaming/creator/streams/metadata"), body, std::move(cb));
}

void XiveApi::categories(const QString &query, int limit, Callback cb)
{
	/* Built by hand rather than with QUrlQuery because `send()` takes a path, not a QUrl, and
	 * round-tripping through QUrl here would only risk double-encoding what is already encoded.
	 * `q` is user input, so it is percent-encoded; `limit` is an int and cannot need it. */
	QString path = QStringLiteral("/api/streaming/categories?limit=%1").arg(limit);
	const QString q = query.trimmed();
	if (!q.isEmpty())
		path += QStringLiteral("&q=") + QString::fromUtf8(QUrl::toPercentEncoding(q));

	get(path, std::move(cb));
}

/* ------------------------------------------------------------------------- */
/* Activity feed                                                             */
/* ------------------------------------------------------------------------- */

void XiveApi::streamActivity(const QString &streamId, const QString &since, Callback cb)
{
	QString path = QStringLiteral("/api/streaming/%1/activity").arg(streamId);
	if (!since.isEmpty()) {
		/* The cursor is an ISO-8601 timestamp: the "+" in a zone offset would otherwise be
		 * decoded as a space, and the ":" separators are sub-delims. Encoded rather than
		 * interpolated. */
		path += QStringLiteral("?since=") +
			QString::fromUtf8(QUrl::toPercentEncoding(since));
	}
	get(path, std::move(cb));
}

/* ------------------------------------------------------------------------- */
/* Xive Surge                                                                */
/* ------------------------------------------------------------------------- */

void XiveApi::streamSurge(const QString &streamId, Callback cb)
{
	get(QStringLiteral("/api/streaming/%1/surge").arg(streamId), std::move(cb));
}

/* ------------------------------------------------------------------------- */
/* Chat                                                                      */
/* ------------------------------------------------------------------------- */

void XiveApi::chatMessages(const QString &streamId, Callback cb)
{
	get(QStringLiteral("/api/streaming/%1/chat/messages").arg(streamId), std::move(cb));
}

void XiveApi::chatSend(const QString &streamId, const QString &content, Callback cb)
{
	QJsonObject body;
	body[QStringLiteral("content")] = content;
	post(QStringLiteral("/api/streaming/%1/chat/send").arg(streamId), body, std::move(cb));
}

void XiveApi::chatRestrictions(const QString &streamId, Callback cb)
{
	get(QStringLiteral("/api/streaming/%1/chat/restrictions").arg(streamId), std::move(cb));
}

void XiveApi::chatModerators(const QString &streamId, Callback cb)
{
	get(QStringLiteral("/api/streaming/%1/chat/moderators").arg(streamId), std::move(cb));
}

/* ------------------------------------------------------------------------- */
/* Chat moderation                                                           */
/* ------------------------------------------------------------------------- */

namespace {

/* Six of the seven take exactly one field, spelled the way the server reads it. */
QJsonObject userIdBody(const QString &userId)
{
	QJsonObject body;
	body[QStringLiteral("userId")] = userId;
	return body;
}

} // namespace

void XiveApi::chatRecall(const QString &streamId, const QString &messageId, Callback cb)
{
	QJsonObject body;
	body[QStringLiteral("messageId")] = messageId;
	post(QStringLiteral("/api/streaming/%1/chat/recall").arg(streamId), body, std::move(cb));
}

void XiveApi::chatTimeout(const QString &streamId, const QString &userId, int minutes, Callback cb)
{
	QJsonObject body = userIdBody(userId);
	body[QStringLiteral("durationMinutes")] = minutes;
	post(QStringLiteral("/api/streaming/%1/chat/timeout").arg(streamId), body, std::move(cb));
}

void XiveApi::chatUnmute(const QString &streamId, const QString &userId, Callback cb)
{
	post(QStringLiteral("/api/streaming/%1/chat/unmute").arg(streamId), userIdBody(userId),
	     std::move(cb));
}

void XiveApi::chatBan(const QString &streamId, const QString &userId, Callback cb)
{
	post(QStringLiteral("/api/streaming/%1/chat/ban").arg(streamId), userIdBody(userId),
	     std::move(cb));
}

void XiveApi::chatUnban(const QString &streamId, const QString &userId, Callback cb)
{
	post(QStringLiteral("/api/streaming/%1/chat/unban").arg(streamId), userIdBody(userId),
	     std::move(cb));
}

void XiveApi::chatAddModerator(const QString &streamId, const QString &userId, Callback cb)
{
	post(QStringLiteral("/api/streaming/%1/chat/add-moderator").arg(streamId),
	     userIdBody(userId), std::move(cb));
}

void XiveApi::chatRemoveModerator(const QString &streamId, const QString &userId, Callback cb)
{
	post(QStringLiteral("/api/streaming/%1/chat/remove-moderator").arg(streamId),
	     userIdBody(userId), std::move(cb));
}

void XiveApi::chatPin(const QString &streamId, const QString &messageId, int durationMinutes,
		      bool untilStreamEnd, Callback cb)
{
	QJsonObject body;
	body[QStringLiteral("messageId")] = messageId;
	/*
	 * Omitted rather than sent as 0 for an open-ended pin, matching the web's pinMessage(): all
	 * spellings mean the same thing to `expiryFromBody` today, but omission is what "no limit"
	 * actually is, and it keeps the wire honest if that ever stops being true.
	 */
	if (durationMinutes > 0)
		body[QStringLiteral("durationMinutes")] = durationMinutes;
	/*
	 * Sent only when true. The server reads it as `=== true`, so false is the same as absent —
	 * but omitting it keeps the "pin until unpinned" request minimal and makes the two calls
	 * obviously different in a packet capture.
	 */
	if (untilStreamEnd)
		body[QStringLiteral("untilStreamEnd")] = true;
	post(QStringLiteral("/api/streaming/%1/chat/pin").arg(streamId), body, std::move(cb));
}

void XiveApi::chatUnpin(const QString &streamId, Callback cb)
{
	/* No body: the pin is one row per stream, so the stream id is the whole address. */
	post(QStringLiteral("/api/streaming/%1/chat/unpin").arg(streamId), QJsonObject(),
	     std::move(cb));
}

/* ------------------------------------------------------------------------- */
/* Gifts / wallet                                                            */
/* ------------------------------------------------------------------------- */

void XiveApi::giftsLeaderboard(const QString &streamId, Callback cb)
{
	get(QStringLiteral("/api/streaming/%1/gifts/leaderboard").arg(streamId), std::move(cb));
}

void XiveApi::giftsCatalog(Callback cb)
{
	get(QStringLiteral("/api/streaming/gifts/catalog"), std::move(cb));
}

void XiveApi::walletBalance(Callback cb)
{
	get(QStringLiteral("/api/streaming/wallet/balance"), std::move(cb));
}

/* ------------------------------------------------------------------------- */
/* Gift overlay                                                              */
/* ------------------------------------------------------------------------- */

void XiveApi::overlayLink(Callback cb)
{
	/* No parameters, deliberately: the route derives the creator from the session and takes
	 * no id at all, so there is no way to ask for somebody else's link. See the header. */
	get(QStringLiteral("/api/streaming/overlay/link"), std::move(cb));
}
