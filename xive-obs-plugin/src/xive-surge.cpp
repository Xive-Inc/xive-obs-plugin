#include "xive-surge.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QObject>
#include <QtGlobal>

namespace XiveSurge {

State parse(const QJsonObject &surge)
{
	State s;
	if (surge.isEmpty())
		return s;

	s.active = surge.value(QStringLiteral("active")).toBool(false);
	s.level = surge.value(QStringLiteral("level")).toInt(0);
	s.points = surge.value(QStringLiteral("points")).toInt(0);
	s.events = surge.value(QStringLiteral("events")).toInt(0);
	s.participants = surge.value(QStringLiteral("participants")).toInt(0);

	/*
	 * `expires_at` is epoch milliseconds and outgrows a 32-bit int by three orders of magnitude,
	 * so it is read through toDouble() — QJsonValue::toInt() silently returns its default for
	 * anything that will not fit, which would zero every deadline and take the banner down the
	 * instant it appeared.
	 */
	const QJsonValue expires = surge.value(QStringLiteral("expires_at"));
	s.expiresAtMs = expires.isDouble() ? static_cast<qint64>(expires.toDouble()) : 0;

	const QJsonArray contributors = surge.value(QStringLiteral("contributors")).toArray();
	for (const QJsonValue &who : contributors) {
		const QString name = who.toString().trimmed();
		if (!name.isEmpty())
			s.contributors.append(name);
	}

	const QJsonObject igniter = surge.value(QStringLiteral("igniter")).toObject();
	s.igniterName = igniter.value(QStringLiteral("name")).toString().trimmed();
	s.igniterPoints = igniter.value(QStringLiteral("points")).toInt(0);

	return s;
}

int levelThreshold(int level)
{
	return kStartThreshold + (qMax(1, level) - 1) * kLevelStep;
}

int toNextLevel(const State &state)
{
	if (state.level >= kMaxLevel)
		return 0;
	return qMax(0, levelThreshold(state.level + 1) - state.points);
}

int secondsLeft(const State &state, qint64 nowMs)
{
	if (!state.active || state.expiresAtMs <= 0)
		return 0;
	const qint64 ms = state.expiresAtMs - nowMs;
	return ms <= 0 ? 0 : static_cast<int>(ms / 1000);
}

QString formatCountdown(int seconds)
{
	const int safe = qMax(0, seconds);
	return QStringLiteral("%1:%2")
		.arg(safe / 60)
		.arg(safe % 60, 2, 10, QLatin1Char('0'));
}

int windowPercent(const State &state, qint64 nowMs)
{
	const int left = secondsLeft(state, nowMs);
	return qBound(0, (left * 100) / kWindowSeconds, 100);
}

QString tier(int level)
{
	if (level >= 20)
		return QObject::tr("MAX SURGE");
	if (level >= 15)
		return QObject::tr("Insane Surge");
	if (level >= 10)
		return QObject::tr("Massive Surge");
	if (level >= 6)
		return QObject::tr("Huge Surge");
	if (level >= 3)
		return QObject::tr("Big Surge");
	return QObject::tr("Surge");
}

} // namespace XiveSurge
