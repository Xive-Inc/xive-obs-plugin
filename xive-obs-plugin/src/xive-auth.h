#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

#include <functional>
#include <vector>

class QNetworkAccessManager;
class QTcpServer;
class QTimer;
class QJsonObject;

/*
 * OAuth 2.0 Authorization Code + PKCE against Authentik, with a loopback redirect (RFC 8252).
 *
 * The flow:
 *   1. Bind a QTcpServer on 127.0.0.1 at one of three pre-registered ports.
 *   2. Open the system browser at the authorize endpoint with an S256 challenge.
 *   3. Authentik redirects back to the loopback listener with ?code=&state=.
 *   4. Exchange the code (+ verifier) for an access token and a 30-day refresh token.
 *
 * Why three fixed ports rather than an ephemeral one: Authentik matches redirect URIs strictly,
 * and a strict allowlist of three is a tighter grant than the regex an ephemeral port needs.
 * Three is enough that a port collision is not a dead end.
 */
class XiveAuth : public QObject {
	Q_OBJECT

public:
	explicit XiveAuth(QObject *parent = nullptr);
	~XiveAuth() override;

	static XiveAuth *instance();
	static void createInstance();
	static void destroyInstance();

	bool isSignedIn() const;
	QString accountLabel() const;

	/*
	 * The signed-in account's email, or empty when userinfo has not answered (or failed).
	 *
	 * Handed to the browser docks so a panel holding a web session for a DIFFERENT creator can
	 * notice and drop it — see XiveBrowserDock and XiveConfig::accountEmail(). Empty means "do
	 * not know", never "nobody": a dock must not act on it as though it were a mismatch.
	 */
	QString accountEmail() const;

	/*
	 * Hands `cb` a usable access token, refreshing first when the cached one is missing or
	 * close to expiry. `cb` receives an empty string when no token could be obtained — every
	 * caller must handle that rather than assuming success.
	 */
	void withAccessToken(std::function<void(const QString &)> cb);

	/* Refresh even if the cached token still looks valid. Used when the API answers 401,
	 * which means the server disagrees with our expiry arithmetic. */
	void forceRefresh(std::function<void(const QString &)> cb);

	void beginSignIn();
	void signOut();

signals:
	void signedInChanged(bool signedIn);
	void statusMessage(const QString &message);
	void authFailed(const QString &message);
	/*
	 * A new access token is in hand — after a refresh or a completed sign-in. The browser panels
	 * listen so they get the replacement NOW, not at their next 10-minute push, which could land
	 * after the token they hold has already expired.
	 */
	void accessTokenChanged();

private:
	void startLoopbackServer();
	void stopLoopbackServer();
	void onLoopbackConnection();
	void handleCallbackQuery(const QString &query, QTcpServer *server);

	/* Takes the redirect URI explicitly: the token endpoint validates it, but the loopback
	 * listener (and with it m_redirectUri) is torn down before the exchange starts. */
	void exchangeCode(const QString &code, const QString &redirectUri);
	void doRefresh();
	bool applyTokenResponse(const QJsonObject &obj);
	void fetchUserInfo();
	void scheduleRefresh();
	void resolveWaiters(const QString &token);
	void failFlow(const QString &why);

	QNetworkAccessManager *m_nam = nullptr;
	QTcpServer *m_server = nullptr;
	QTimer *m_refreshTimer = nullptr;

	QString m_accessToken;
	QDateTime m_accessExpiry;

	/* Live only for the duration of one sign-in attempt. */
	QString m_verifier;
	QString m_state;
	QString m_redirectUri;

	QString m_accountLabel;
	QString m_accountEmail;

	bool m_refreshing = false;
	std::vector<std::function<void(const QString &)>> m_waiters;
};
