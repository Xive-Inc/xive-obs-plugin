#include "xive-auth.h"
#include "xive-config.h"
#include "xive-log.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

/* Pre-registered in the Authentik "xive-obs" provider as strict redirect URIs. Changing this
 * list means changing the provider too, or sign-in breaks with redirect_uri_mismatch. */
const quint16 kLoopbackPorts[] = {47525, 47526, 47527};

/* Refresh this far before the access token actually expires, so an in-flight request never
 * races the expiry. Authentik issues 1h tokens for this provider. */
constexpr int kRefreshSkewSeconds = 300;

XiveAuth *g_instance = nullptr;

QByteArray randomBase64Url(int bytes)
{
	QByteArray raw(bytes, Qt::Uninitialized);
	QRandomGenerator::system()->generate(raw.begin(), raw.end());
	return raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString htmlResponse(const QString &heading, const QString &detail)
{
	return QStringLiteral("<!doctype html><html><head><meta charset=\"utf-8\">"
			      "<title>Xive</title><style>"
			      "body{background:#0f1115;color:#e8eaed;font:16px/1.6 system-ui,sans-serif;"
			      "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
			      "div{text-align:center;max-width:32rem;padding:2rem}"
			      "h1{font-size:1.4rem;margin:0 0 .5rem}p{margin:0;color:#9aa0a6}"
			      "</style></head><body><div><h1>%1</h1><p>%2</p></div></body></html>")
		/* Escaped: `detail` can be the callback's own error_description, which anyone can
		 * put in a link to this listener. */
		.arg(heading.toHtmlEscaped(), detail.toHtmlEscaped());
}

void writeAndClose(QTcpSocket *socket, int status, const QString &statusText, const QString &body)
{
	const QByteArray payload = body.toUtf8();
	QByteArray head;
	head += QStringLiteral("HTTP/1.1 %1 %2\r\n").arg(status).arg(statusText).toUtf8();
	head += "Content-Type: text/html; charset=utf-8\r\n";
	head += QStringLiteral("Content-Length: %1\r\n").arg(payload.size()).toUtf8();
	head += "Cache-Control: no-store\r\n";
	head += "Connection: close\r\n\r\n";

	socket->write(head);
	socket->write(payload);
	socket->flush();
	socket->disconnectFromHost();
}

} // namespace

XiveAuth::XiveAuth(QObject *parent) : QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);

	m_refreshTimer = new QTimer(this);
	m_refreshTimer->setSingleShot(true);
	connect(m_refreshTimer, &QTimer::timeout, this, [this]() {
		if (XiveConfig::instance().refreshToken().isEmpty())
			return;
		xive_info("access token near expiry; refreshing");
		doRefresh();
	});

	m_accountLabel = XiveConfig::instance().accountLabel();
	m_accountEmail = XiveConfig::instance().accountEmail();
}

XiveAuth::~XiveAuth()
{
	stopLoopbackServer();
}

XiveAuth *XiveAuth::instance()
{
	return g_instance;
}

void XiveAuth::createInstance()
{
	if (!g_instance)
		g_instance = new XiveAuth();
}

void XiveAuth::destroyInstance()
{
	delete g_instance;
	g_instance = nullptr;
}

bool XiveAuth::isSignedIn() const
{
	return !XiveConfig::instance().refreshToken().isEmpty();
}

QString XiveAuth::accountLabel() const
{
	return m_accountLabel;
}

QString XiveAuth::accountEmail() const
{
	return m_accountEmail;
}

/* ------------------------------------------------------------------------- */
/* Token access                                                              */
/* ------------------------------------------------------------------------- */

void XiveAuth::withAccessToken(std::function<void(const QString &)> cb)
{
	if (!isSignedIn()) {
		cb(QString());
		return;
	}

	const bool usable = !m_accessToken.isEmpty() && m_accessExpiry.isValid() &&
			    QDateTime::currentDateTimeUtc().secsTo(m_accessExpiry) > kRefreshSkewSeconds;
	if (usable) {
		cb(m_accessToken);
		return;
	}

	m_waiters.push_back(std::move(cb));
	if (!m_refreshing)
		doRefresh();
}

void XiveAuth::forceRefresh(std::function<void(const QString &)> cb)
{
	if (!isSignedIn()) {
		cb(QString());
		return;
	}

	/* Drop the cached token so a concurrent withAccessToken() cannot hand it back out. */
	m_accessToken.clear();
	m_accessExpiry = QDateTime();

	m_waiters.push_back(std::move(cb));
	if (!m_refreshing)
		doRefresh();
}

void XiveAuth::resolveWaiters(const QString &token)
{
	/* Swap first: a callback may itself call withAccessToken and push a new waiter. */
	std::vector<std::function<void(const QString &)>> pending;
	pending.swap(m_waiters);
	for (auto &cb : pending)
		cb(token);
}

void XiveAuth::doRefresh()
{
	const QString refreshToken = XiveConfig::instance().refreshToken();
	if (refreshToken.isEmpty()) {
		resolveWaiters(QString());
		return;
	}

	m_refreshing = true;

	QUrlQuery form;
	form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
	form.addQueryItem(QStringLiteral("refresh_token"), refreshToken);
	form.addQueryItem(QStringLiteral("client_id"), XiveConfig::clientId());

	/* Named local rather than QNetworkRequest req(QUrl(...)): with a qualified call inside the
	 * parentheses that form is a most-vexing-parse and clang reads it as a function
	 * declaration. */
	const QUrl tokenUrl(XiveConfig::tokenEndpoint());
	QNetworkRequest req(tokenUrl);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      QStringLiteral("application/x-www-form-urlencoded"));

	QNetworkReply *reply = m_nam->post(req, form.toString(QUrl::FullyEncoded).toUtf8());

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		m_refreshing = false;

		const QByteArray body = reply->readAll();
		const int status =
			reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

		if (status < 200 || status >= 300) {
			/*
			 * A 400 invalid_grant means the refresh token is dead — expired, revoked,
			 * or already rotated. There is nothing to retry, so sign out and make the
			 * user re-authorize rather than looping.
			 *
			 * Any other status (5xx, or a network error where status is 0) is treated
			 * as transient: the stored token is KEPT so a Xive outage or a laptop that
			 * woke up without wifi does not silently log every broadcaster out.
			 */
			const QJsonObject err = QJsonDocument::fromJson(body).object();
			const QString code = err.value(QStringLiteral("error")).toString();
			const bool dead = (status == 400 && code == QStringLiteral("invalid_grant")) ||
					  status == 401;

			if (dead) {
				xive_warn("refresh token rejected (%s); signing out",
					  qUtf8Printable(code));
				XiveConfig::instance().clearSession();
				m_accountLabel.clear();
				m_accountEmail.clear();
				emit signedInChanged(false);
				emit authFailed(tr("Your Xive sign-in expired. Please connect again."));
			} else {
				xive_warn("token refresh failed (HTTP %d); keeping the stored token",
					  status);
				emit statusMessage(tr("Could not reach Xive. Retrying shortly."));
				/* Try again on a short backoff rather than only on next use. */
				m_refreshTimer->start(30000);
			}

			resolveWaiters(QString());
			return;
		}

		const QJsonObject obj = QJsonDocument::fromJson(body).object();
		if (!applyTokenResponse(obj)) {
			resolveWaiters(QString());
			return;
		}

		xive_info("access token refreshed");
		resolveWaiters(m_accessToken);
	});
}

bool XiveAuth::applyTokenResponse(const QJsonObject &obj)
{
	const QString access = obj.value(QStringLiteral("access_token")).toString();
	if (access.isEmpty()) {
		xive_error("token response carried no access_token");
		emit authFailed(tr("Xive returned an unexpected sign-in response."));
		return false;
	}

	m_accessToken = access;

	const int expiresIn = obj.value(QStringLiteral("expires_in")).toInt(3600);
	m_accessExpiry = QDateTime::currentDateTimeUtc().addSecs(expiresIn);

	/*
	 * Authentik rotates the refresh token on every use, so a response that carries a new one
	 * MUST overwrite the stored value — keeping the old one would work exactly once more and
	 * then fail as already-consumed.
	 */
	const QString rotated = obj.value(QStringLiteral("refresh_token")).toString();
	if (!rotated.isEmpty())
		XiveConfig::instance().setRefreshToken(rotated);

	scheduleRefresh();

	/* The one place both paths (refresh and code exchange) store a token, so the signal cannot be
	 * missed by either. Listeners that ask withAccessToken() get this token straight back — it is
	 * fresh, so no second refresh is triggered. */
	emit accessTokenChanged();
	return true;
}

void XiveAuth::scheduleRefresh()
{
	if (!m_accessExpiry.isValid())
		return;

	const qint64 secs = QDateTime::currentDateTimeUtc().secsTo(m_accessExpiry) - kRefreshSkewSeconds;
	const qint64 ms = qMax<qint64>(30, secs) * 1000;
	m_refreshTimer->start(static_cast<int>(qMin<qint64>(ms, 3600 * 1000)));
}

/* ------------------------------------------------------------------------- */
/* Sign-in                                                                   */
/* ------------------------------------------------------------------------- */

void XiveAuth::beginSignIn()
{
	if (m_server) {
		emit statusMessage(tr("Sign-in already in progress — check your browser."));
		return;
	}

	startLoopbackServer();
	if (!m_server)
		return;

	m_verifier = QString::fromLatin1(randomBase64Url(32));
	m_state = QString::fromLatin1(randomBase64Url(16));

	const QByteArray challenge =
		QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
			.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

	QUrlQuery q;
	q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
	q.addQueryItem(QStringLiteral("client_id"), XiveConfig::clientId());
	q.addQueryItem(QStringLiteral("redirect_uri"), m_redirectUri);
	q.addQueryItem(QStringLiteral("scope"), QStringLiteral("openid profile email offline_access"));
	q.addQueryItem(QStringLiteral("state"), m_state);
	q.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
	q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));

	QUrl url(XiveConfig::authorizeEndpoint());
	url.setQuery(q);

	xive_info("opening the browser for sign-in (loopback %s)", qUtf8Printable(m_redirectUri));

	if (!QDesktopServices::openUrl(url)) {
		failFlow(tr("Could not open your browser. Copy this link into it manually:\n%1")
				 .arg(url.toString()));
		return;
	}

	emit statusMessage(tr("Waiting for you to approve access in your browser…"));

	/*
	 * Do not listen forever. An abandoned attempt would otherwise hold a loopback port for
	 * the lifetime of the OBS process and leave a stale verifier in memory.
	 */
	QTimer::singleShot(5 * 60 * 1000, this, [this]() {
		if (m_server) {
			xive_warn("sign-in timed out with no callback");
			failFlow(tr("Sign-in timed out. Try connecting again."));
		}
	});
}

void XiveAuth::startLoopbackServer()
{
	m_server = new QTcpServer(this);

	quint16 bound = 0;
	for (quint16 port : kLoopbackPorts) {
		if (m_server->listen(QHostAddress::LocalHost, port)) {
			bound = port;
			break;
		}
	}

	if (bound == 0) {
		xive_error("all loopback ports are in use");
		delete m_server;
		m_server = nullptr;
		emit authFailed(tr("Ports 47525-47527 are all in use, so sign-in cannot receive "
				   "its response. Close whatever is using them and try again."));
		return;
	}

	m_redirectUri = QStringLiteral("http://127.0.0.1:%1/callback").arg(bound);
	connect(m_server, &QTcpServer::newConnection, this, &XiveAuth::onLoopbackConnection);
}

void XiveAuth::stopLoopbackServer()
{
	if (!m_server)
		return;
	m_server->close();
	m_server->deleteLater();
	m_server = nullptr;
	m_redirectUri.clear();
}

void XiveAuth::onLoopbackConnection()
{
	QTcpSocket *socket = m_server->nextPendingConnection();
	if (!socket)
		return;

	connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);

	connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
		QByteArray buffer = socket->property("xive_buffer").toByteArray();
		buffer += socket->readAll();

		/* Wait for the full request head before parsing. The request line usually arrives
		 * in one segment, but "usually" is not a parser. */
		const int headEnd = buffer.indexOf("\r\n\r\n");
		if (headEnd < 0) {
			if (buffer.size() > 16384) {
				writeAndClose(socket, 431, QStringLiteral("Request Header Fields Too Large"),
					      htmlResponse(tr("Bad request"), tr("Nothing to see here.")));
				return;
			}
			socket->setProperty("xive_buffer", buffer);
			return;
		}

		const QByteArray requestLine = buffer.left(buffer.indexOf("\r\n"));
		const QList<QByteArray> parts = requestLine.split(' ');
		if (parts.size() < 2 || parts[0] != "GET") {
			writeAndClose(socket, 405, QStringLiteral("Method Not Allowed"),
				      htmlResponse(tr("Bad request"), tr("Nothing to see here.")));
			return;
		}

		const QUrl target(QString::fromUtf8(parts[1]));
		if (target.path() != QStringLiteral("/callback")) {
			/* Browsers cheerfully request /favicon.ico against the loopback listener;
			 * answering 404 keeps that noise out of the flow. */
			writeAndClose(socket, 404, QStringLiteral("Not Found"),
				      htmlResponse(tr("Not found"), tr("Nothing to see here.")));
			return;
		}

		const QUrlQuery query(target);
		const QString error = query.queryItemValue(QStringLiteral("error"));
		const QString code = query.queryItemValue(QStringLiteral("code"));
		const QString state = query.queryItemValue(QStringLiteral("state"));

		/*
		 * The state check is what stops a third party from feeding us their own code — and it
		 * comes BEFORE the error branch, or any web page could cancel a sign-in in progress by
		 * sending the browser to /callback?error=x. A mismatched request is answered and
		 * ignored rather than failing the flow, for the same reason: only the real response
		 * gets to end it.
		 */
		if (state != m_state || m_state.isEmpty()) {
			writeAndClose(socket, 400, QStringLiteral("Bad Request"),
				      htmlResponse(tr("Sign-in failed"),
						   tr("The response did not match this request.")));
			xive_warn("ignored a loopback callback with a mismatched state");
			return;
		}

		if (!error.isEmpty()) {
			const QString desc = query.queryItemValue(
				QStringLiteral("error_description"), QUrl::FullyDecoded);
			writeAndClose(socket, 200, QStringLiteral("OK"),
				      htmlResponse(tr("Sign-in cancelled"),
						   desc.isEmpty() ? error : desc));
			failFlow(tr("Xive declined the sign-in: %1").arg(desc.isEmpty() ? error : desc));
			return;
		}

		if (code.isEmpty()) {
			writeAndClose(socket, 400, QStringLiteral("Bad Request"),
				      htmlResponse(tr("Sign-in failed"),
						   tr("No authorization code was returned.")));
			failFlow(tr("Xive returned no authorization code. Try again."));
			return;
		}

		writeAndClose(socket, 200, QStringLiteral("OK"),
			      htmlResponse(tr("You're connected to Xive"),
					   tr("You can close this tab and go back to OBS.")));

		/* One code per listener: stop accepting before the exchange so a replayed request
		 * cannot start a second one. Grab the redirect URI first — tearing down the
		 * listener clears it, and the token endpoint validates it. */
		const QString redirectUri = m_redirectUri;
		stopLoopbackServer();
		exchangeCode(code, redirectUri);
	});
}

void XiveAuth::exchangeCode(const QString &code, const QString &redirectUri)
{
	emit statusMessage(tr("Finishing sign-in…"));

	QUrlQuery form;
	form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
	form.addQueryItem(QStringLiteral("code"), code);
	form.addQueryItem(QStringLiteral("redirect_uri"), redirectUri);
	form.addQueryItem(QStringLiteral("client_id"), XiveConfig::clientId());
	form.addQueryItem(QStringLiteral("code_verifier"), m_verifier);

	const QUrl tokenUrl(XiveConfig::tokenEndpoint());
	QNetworkRequest req(tokenUrl);
	req.setHeader(QNetworkRequest::ContentTypeHeader,
		      QStringLiteral("application/x-www-form-urlencoded"));

	QNetworkReply *reply = m_nam->post(req, form.toString(QUrl::FullyEncoded).toUtf8());

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		m_verifier.clear();
		m_state.clear();

		const QByteArray body = reply->readAll();
		const int status =
			reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

		if (status < 200 || status >= 300) {
			const QJsonObject err = QJsonDocument::fromJson(body).object();
			const QString desc = err.value(QStringLiteral("error_description")).toString();
			xive_error("code exchange failed (HTTP %d)", status);
			failFlow(desc.isEmpty()
					 ? tr("Sign-in failed while exchanging the code (HTTP %1).")
						   .arg(status)
					 : desc);
			return;
		}

		const QJsonObject obj = QJsonDocument::fromJson(body).object();
		if (obj.value(QStringLiteral("refresh_token")).toString().isEmpty()) {
			/* Without a refresh token the plugin would need re-authorizing every hour,
			 * which is unusable mid-broadcast. Treat it as a configuration fault. */
			xive_error("token response carried no refresh_token (offline_access missing?)");
			failFlow(tr("Xive did not return a long-lived session. "
				    "Contact support — the OBS client is misconfigured."));
			return;
		}

		if (!applyTokenResponse(obj))
			return;

		xive_info("sign-in complete");
		emit signedInChanged(true);
		emit statusMessage(tr("Connected to Xive."));
		fetchUserInfo();
	});
}

void XiveAuth::fetchUserInfo()
{
	if (m_accessToken.isEmpty())
		return;

	const QUrl userInfoUrl(XiveConfig::userInfoEndpoint());
	QNetworkRequest req(userInfoUrl);
	req.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());

	QNetworkReply *reply = m_nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const int status =
			reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (status < 200 || status >= 300) {
			/* Cosmetic only — the account label is never used for authorization, so a
			 * failure here does not affect the session. */
			xive_warn("userinfo failed (HTTP %d); no account label", status);
			return;
		}

		const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
		const QString name = o.value(QStringLiteral("name")).toString().isEmpty()
					     ? o.value(QStringLiteral("preferred_username")).toString()
					     : o.value(QStringLiteral("name")).toString();
		const QString email = o.value(QStringLiteral("email")).toString();

		m_accountLabel = email.isEmpty() ? name
						 : (name.isEmpty() ? email
								   : QStringLiteral("%1 (%2)").arg(name, email));
		XiveConfig::instance().setAccountLabel(m_accountLabel);

		/*
		 * Kept on its own as well as inside the label. The label is a sentence for a human to
		 * read and its shape changes with what the IdP returned; the docks need one address to
		 * compare against, so it is stored unformatted and lower-cased — an email is
		 * case-insensitive and the two sides of the comparison come from different systems.
		 */
		m_accountEmail = email.trimmed().toLower();
		XiveConfig::instance().setAccountEmail(m_accountEmail);

		emit signedInChanged(true);
	});
}

void XiveAuth::failFlow(const QString &why)
{
	stopLoopbackServer();
	m_verifier.clear();
	m_state.clear();
	resolveWaiters(QString());
	emit authFailed(why);
}

void XiveAuth::signOut()
{
	stopLoopbackServer();
	m_refreshTimer->stop();

	m_accessToken.clear();
	m_accessExpiry = QDateTime();
	m_accountLabel.clear();
	m_accountEmail.clear();
	m_verifier.clear();
	m_state.clear();

	XiveConfig::instance().clearSession();

	xive_info("signed out");
	emit signedInChanged(false);
	emit statusMessage(tr("Signed out."));
}
