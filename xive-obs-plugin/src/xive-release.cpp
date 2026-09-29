#include "xive-release.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QtGlobal>

namespace {

/* "v0.3.0-rc1" -> [0, 3, 0]. A segment that is not a number counts as zero rather than
 * poisoning the whole comparison — a manifest is remote input. */
QList<int> numericParts(const QString &version)
{
	QString core = version.trimmed();
	if (core.startsWith(QLatin1Char('v')) || core.startsWith(QLatin1Char('V')))
		core.remove(0, 1);

	const qsizetype dash = core.indexOf(QLatin1Char('-'));
	if (dash >= 0)
		core.truncate(dash);
	const qsizetype plus = core.indexOf(QLatin1Char('+'));
	if (plus >= 0)
		core.truncate(plus);

	QList<int> out;
	const QStringList bits = core.split(QLatin1Char('.'), Qt::SkipEmptyParts);
	for (const QString &bit : bits) {
		bool ok = false;
		const int n = bit.toInt(&ok);
		out.append(ok && n > 0 ? n : 0);
	}
	return out;
}

} // namespace

int xiveCompareVersions(const QString &a, const QString &b)
{
	const QList<int> l = numericParts(a);
	const QList<int> r = numericParts(b);
	const qsizetype n = qMax(l.size(), r.size());

	for (qsizetype i = 0; i < n; ++i) {
		const int lv = i < l.size() ? l.at(i) : 0;
		const int rv = i < r.size() ? r.at(i) : 0;
		if (lv != rv)
			return lv < rv ? -1 : 1;
	}
	return 0;
}

QString xivePlatformKey()
{
#if defined(Q_OS_MACOS)
#if defined(Q_PROCESSOR_ARM)
	return QStringLiteral("macos-arm64");
#else
	/* No Intel build is published. Named anyway so the manifest can grow one without a
	 * plugin change, and so the "nothing for your platform" path is reached honestly. */
	return QStringLiteral("macos-x86_64");
#endif
#elif defined(Q_OS_WIN)
	return QStringLiteral("windows-x86_64");
#else
	return QFile::exists(QStringLiteral("/.flatpak-info"))
		       ? QStringLiteral("linux-flatpak-x86_64")
		       : QStringLiteral("linux-native-x86_64");
#endif
}

XiveRelease xiveParseManifest(const QByteArray &json, const QString &platformKey)
{
	XiveRelease rel;
	rel.platform = platformKey;

	const QJsonDocument doc = QJsonDocument::fromJson(json);
	if (!doc.isObject())
		return rel;

	const QJsonObject o = doc.object();
	rel.version = o.value(QStringLiteral("version")).toString().trimmed();
	if (rel.version.isEmpty())
		return rel;

	rel.released = o.value(QStringLiteral("released")).toString().trimmed();
	rel.minObsMajor = o.value(QStringLiteral("min_obs_major")).toInt(0);

	const QJsonObject platforms = o.value(QStringLiteral("platforms")).toObject();
	const QJsonObject p = platforms.value(platformKey).toObject();
	if (p.isEmpty())
		return rel;

	rel.hasPlatform = true;
	rel.file = p.value(QStringLiteral("file")).toString();
	rel.url = p.value(QStringLiteral("url")).toString();
	rel.sha256 = p.value(QStringLiteral("sha256")).toString();
	rel.size = static_cast<qint64>(p.value(QStringLiteral("size")).toDouble(0));
	rel.supported = p.value(QStringLiteral("supported")).toBool(true);
	rel.requiresNote = p.value(QStringLiteral("requires")).toString();

	/* A platform entry with no URL is not installable, and offering a button that leads
	 * nowhere is worse than the generic "nothing published for you yet" path. */
	if (rel.url.isEmpty())
		rel.hasPlatform = false;

	return rel;
}
