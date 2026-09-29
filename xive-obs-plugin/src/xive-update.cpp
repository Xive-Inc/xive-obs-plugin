#include "xive-update.h"

#include "xive-config.h"
#include "xive-log.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

XiveUpdate *g_instance = nullptr;

/*
 * 20 s after the docks are up. Long enough that the launch check — which is what a creator is
 * actually waiting on at startup — has the network to itself, short enough that the answer is
 * there before anyone goes looking for the panel.
 */
constexpr int kFirstCheckDelayMs = 20000;

/* Once a day. Releases are days apart, and OBS stays open for a whole broadcast. */
constexpr int kRecheckIntervalMs = 24 * 60 * 60 * 1000;

} // namespace

XiveUpdate::XiveUpdate(QObject *parent) : QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
}

XiveUpdate *XiveUpdate::instance()
{
	return g_instance;
}

void XiveUpdate::createInstance()
{
	if (!g_instance)
		g_instance = new XiveUpdate();
}

void XiveUpdate::destroyInstance()
{
	delete g_instance;
	g_instance = nullptr;
}

void XiveUpdate::start()
{
	if (!XiveConfig::instance().updateCheck()) {
		xive_info("update check disabled by preference");
		return;
	}

	if (m_started) {
		/* The preference was switched back on, or start() ran twice. Answer now rather
		 * than waiting a day for the timer that is already running. */
		checkNow(false);
		return;
	}
	m_started = true;

	QTimer::singleShot(kFirstCheckDelayMs, this, [this]() { checkNow(false); });

	m_timer = new QTimer(this);
	m_timer->setInterval(kRecheckIntervalMs);
	connect(m_timer, &QTimer::timeout, this, [this]() { checkNow(false); });
	m_timer->start();
}

QString XiveUpdate::installerUrl(const QString &platformKey)
{
	QString base = XiveConfig::updateManifestUrl();
	const qsizetype cut = base.lastIndexOf(QLatin1Char('/'));
	if (cut <= 0)
		return QString();
	base.truncate(cut); /* .../obs */

	/* Both Linux keys share one installer; it works out which OBS it is looking at. */
	if (platformKey.startsWith(QLatin1String("linux-")))
		return base + QStringLiteral("/install-linux.sh");
	if (platformKey.startsWith(QLatin1String("macos-")))
		return base + QStringLiteral("/install-macos.sh");
	if (platformKey == QLatin1String("windows-x86_64"))
		return base + QStringLiteral("/install-windows.ps1");

	return QString();
}

QString XiveUpdate::installCommand(const QString &platformKey)
{
	const QString url = installerUrl(platformKey);
	if (url.isEmpty())
		return QString();

	if (platformKey == QLatin1String("linux-flatpak-x86_64"))
		return QStringLiteral("curl -fsSL %1 | sh").arg(url);
	/* --allow-native, because the installer refuses the native build without it: it is
	 * built against a Qt 6.11-era distro and fails to load on an older one. */
	if (platformKey == QLatin1String("linux-native-x86_64"))
		return QStringLiteral("curl -fsSL %1 | sh -s -- --allow-native").arg(url);
	if (platformKey.startsWith(QLatin1String("macos-")))
		return QStringLiteral("curl -fsSL %1 | sh").arg(url);

	/*
	 * PowerShell, and PIPED rather than saved to a file: `iex` over a downloaded string is not
	 * a script FILE, so the default `Restricted` ExecutionPolicy does not apply to it and there
	 * is no downloaded executable for SmartScreen to warn about. The same bytes saved as
	 * install-windows.ps1 and double-clicked ARE blocked. The script's own header carries the
	 * long version of that reasoning.
	 */
	if (platformKey == QLatin1String("windows-x86_64"))
		return QStringLiteral("irm %1 | iex").arg(url);

	/* Every platform the manifest can carry has an installer, so this is the floor for a
	 * platform key from a future build that this one has never heard of — installerUrl() is
	 * deliberately looser than this list, so a new linux-* key still resolves to a URL. */
	return QString();
}

void XiveUpdate::checkNow(bool userInitiated)
{
	if (!userInitiated && !XiveConfig::instance().updateCheck())
		return;

	if (m_reply) {
		xive_debug("update check already in flight");
		return;
	}

	const QUrl url(XiveConfig::updateManifestUrl());
	if (!url.isValid()) {
		xive_warn("update manifest URL is not valid: %s",
			  qUtf8Printable(url.toString()));
		if (userInitiated)
			emit checkFailed(tr("The update location is misconfigured."));
		return;
	}

	QNetworkRequest req(url);
	req.setRawHeader("Accept", "application/json");
	req.setRawHeader("User-Agent", QStringLiteral("xive-obs-plugin/%1")
						.arg(QStringLiteral(XIVE_PLUGIN_VERSION))
						.toUtf8());
	/* No Authorization header, on purpose: this is a public object on the CDN, the check has
	 * to work signed out, and the token has no business leaving api.thexive.com. */
	req.setTransferTimeout(15000);

	QNetworkReply *reply = m_nam->get(req);
	m_reply = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply, userInitiated]() {
		reply->deleteLater();
		if (m_reply == reply)
			m_reply = nullptr;

		const int status =
			reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QByteArray raw = reply->readAll();

		if (status == 0) {
			xive_info("update check could not reach %s: %s",
				  qUtf8Printable(XiveConfig::updateManifestUrl()),
				  qUtf8Printable(reply->errorString()));
			if (userInitiated)
				emit checkFailed(
					tr("Could not reach Xive to check for a newer version."));
			return;
		}

		if (status < 200 || status >= 300) {
			xive_warn("update check got HTTP %d", status);
			if (userInitiated)
				emit checkFailed(
					tr("Could not check for updates (HTTP %1).").arg(status));
			return;
		}

		const XiveRelease release = xiveParseManifest(raw, xivePlatformKey());
		if (!release.isValid()) {
			xive_warn("update manifest could not be decoded");
			if (userInitiated)
				emit checkFailed(tr("The update information could not be read."));
			return;
		}

		const QString current = QStringLiteral(XIVE_PLUGIN_VERSION);
		if (xiveCompareVersions(release.version, current) <= 0) {
			m_pending = XiveRelease();
			xive_info("update check: running %s, published %s — up to date",
				  qUtf8Printable(current), qUtf8Printable(release.version));
			if (userInitiated)
				emit upToDate(current);
			return;
		}

		m_pending = release;
		xive_info("update available: %s (running %s), platform %s%s",
			  qUtf8Printable(release.version), qUtf8Printable(current),
			  qUtf8Printable(release.platform),
			  release.hasPlatform ? "" : " — nothing published for it");

		/*
		 * A skipped version stays skipped until a newer one supersedes it or the creator
		 * asks. `pending()` is set either way, so the button still finds it.
		 */
		if (!userInitiated &&
		    XiveConfig::instance().skippedUpdateVersion() == release.version) {
			xive_info("update %s was skipped by the creator; staying quiet",
				  qUtf8Printable(release.version));
			return;
		}

		emit updateAvailable(release);
	});
}
