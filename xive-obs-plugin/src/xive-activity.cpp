#include "xive-activity.h"

#include <QDateTime>
#include <QSet>

/* std::sort. GCC pulls this in transitively through the Qt headers; MSVC does not, so relying on
 * that would break the Windows build only. Same class of problem as the most-vexing-parse that
 * only clang rejected. */
#include <algorithm>

namespace XiveActivity {

namespace {

/*
 * The vocabulary this build knows, mirroring `stream_events_type_check` in
 * scripts/apply-stream-activity-events.sql and the web's EVENT_TYPES.
 *
 * ⚠️ `clip` IS DELIBERATELY ABSENT even though the server's type constraint still allows it.
 * Nothing on the platform writes a clip event — the only mentions of TYPE_CLIP in the API are the
 * constant and the allowlist it sits in — so the row can never arrive, and the filter chip it fed
 * was a bucket that could only ever read zero.
 *
 * 🔴 IF A PRODUCER IS EVER WRITTEN, PUT IT BACK HERE **AND** IN filterOptions() — clip events will
 * not simply turn up under "All" until then. An earlier version of this note said they would; that
 * was wrong about the file it was written in. fromServerRows() DROPS a row whose type it does not
 * know (three lines below), deliberately, so that a plugin older than the server cannot draw a row
 * with no colour, no description and no filter that can reach it. The cost of that choice is
 * exactly this: a new server-side event type is invisible in the dock, silently, until a build
 * names it.
 *
 * `super-chat` STAYS, and is not the same case. No stream_events row carries it either — which is
 * why it is absent from the Lumia plugin, whose only source is that stream — but this dock also
 * synthesises super chats locally from priority chat messages (superChatsFromMessages below), and
 * those are real and do appear.
 */
const QSet<QString> &knownTypes()
{
	static const QSet<QString> types = {
		QStringLiteral("follow"),         QStringLiteral("gift"),
		QStringLiteral("spark"),          QStringLiteral("subscription"),
		QStringLiteral("gifted-sub"),     QStringLiteral("supporter-tier"),
		QStringLiteral("super-chat"),     QStringLiteral("raid"),
		QStringLiteral("marker"),
	};
	return types;
}

/* Only used when a row arrives with a `filter` this build does not recognise. */
QString filterForType(const QString &type)
{
	if (type == QStringLiteral("follow"))
		return QStringLiteral("follows");
	if (type == QStringLiteral("gift"))
		return QStringLiteral("gifts");
	if (type == QStringLiteral("spark"))
		return QStringLiteral("sparks");
	if (type == QStringLiteral("subscription") || type == QStringLiteral("gifted-sub") ||
	    type == QStringLiteral("supporter-tier"))
		return QStringLiteral("subs");
	if (type == QStringLiteral("super-chat"))
		return QStringLiteral("super-chats");
	if (type == QStringLiteral("raid"))
		return QStringLiteral("raids");
	if (type == QStringLiteral("marker"))
		return QStringLiteral("markers");
	return QStringLiteral("all");
}

qint64 epochMs(const QString &iso)
{
	const QDateTime ts = QDateTime::fromString(iso, Qt::ISODateWithMs);
	return ts.isValid() ? ts.toMSecsSinceEpoch() : 0;
}

} // namespace

QList<QPair<QString, QString>> filterOptions()
{
	/* Same ids and same order as the web's STUDIO_ACTIVITY_FILTER_OPTIONS, LESS "Clips" — see
	 * knownTypes(): nothing writes a clip event, so that chip could only ever count zero.
	 * "Sparks" is new on both sides: spark events used to be filed under the "all" bucket, which
	 * excluded them from every counter and made them unreachable under any filter. */
	return {
		{QStringLiteral("all"), QStringLiteral("All")},
		{QStringLiteral("follows"), QStringLiteral("Follows")},
		{QStringLiteral("gifts"), QStringLiteral("Gifts")},
		{QStringLiteral("sparks"), QStringLiteral("Sparks")},
		{QStringLiteral("subs"), QStringLiteral("Subs")},
		{QStringLiteral("super-chats"), QStringLiteral("Super Chats")},
		{QStringLiteral("raids"), QStringLiteral("Raids")},
		{QStringLiteral("markers"), QStringLiteral("Markers")},
	};
}

QList<Event> fromServerRows(const QList<QJsonObject> &rows)
{
	QList<Event> events;
	events.reserve(rows.size());

	for (const QJsonObject &row : rows) {
		const QString id = row.value(QStringLiteral("id")).toString();
		const QString type = row.value(QStringLiteral("type")).toString();
		if (id.isEmpty() || !knownTypes().contains(type))
			continue;

		Event e;
		e.id = id;
		e.type = type;

		const QString filter = row.value(QStringLiteral("filter")).toString();
		bool known = false;
		for (const auto &option : filterOptions()) {
			if (option.first == filter) {
				known = true;
				break;
			}
		}
		e.filter = known ? filter : filterForType(type);

		e.displayName = row.value(QStringLiteral("display_name")).toString().trimmed();
		if (e.displayName.isEmpty())
			e.displayName = QStringLiteral("Someone");

		e.description = row.value(QStringLiteral("description")).toString().trimmed();
		e.amountLabel = row.value(QStringLiteral("amount_label")).toString().trimmed();
		e.createdAtIso = row.value(QStringLiteral("created_at")).toString();

		events.append(e);
	}

	return events;
}

QList<Event> superChatsFromMessages(const QList<QJsonObject> &messages)
{
	QList<Event> events;

	for (const QJsonObject &msg : messages) {
		if (!msg.value(QStringLiteral("is_priority")).toBool())
			continue;
		const QString content = msg.value(QStringLiteral("content")).toString().trimmed();
		if (content.isEmpty())
			continue;

		Event e;
		e.id = QStringLiteral("super-") + msg.value(QStringLiteral("id")).toString();
		e.type = QStringLiteral("super-chat");
		e.filter = QStringLiteral("super-chats");
		e.displayName = msg.value(QStringLiteral("display_name")).toString();
		e.description = content;
		e.createdAtIso = msg.value(QStringLiteral("created_at")).toString();
		events.append(e);
	}

	return events;
}

QList<Event> mergeEvents(const QList<Event> &serverEvents, const QList<QJsonObject> &messages)
{
	QList<Event> merged = serverEvents;
	QSet<QString> seen;
	for (const Event &e : merged)
		seen.insert(e.id);

	for (const Event &e : superChatsFromMessages(messages)) {
		if (seen.contains(e.id))
			continue;
		seen.insert(e.id);
		merged.append(e);
	}

	/* Newest first. Parsed once per comparison is wasteful on a long feed, but the list is
	 * bounded at a few hundred rows and this runs on a 30 s redraw, not per frame. */
	std::sort(merged.begin(), merged.end(), [](const Event &a, const Event &b) {
		return epochMs(a.createdAtIso) > epochMs(b.createdAtIso);
	});

	return merged;
}

QList<Event> filterEvents(const QList<Event> &events, const QString &filter)
{
	if (filter == QStringLiteral("all"))
		return events;

	QList<Event> out;
	for (const Event &e : events) {
		if (e.filter == filter)
			out.append(e);
	}
	return out;
}

QMap<QString, int> countByFilter(const QList<Event> &events)
{
	QMap<QString, int> counts;
	for (const auto &option : filterOptions())
		counts[option.first] = 0;
	counts[QStringLiteral("all")] = static_cast<int>(events.size());

	for (const Event &e : events) {
		/* Every event has a real bucket now. The old version skipped anything filed under
		 * "all", which is how sparks ended up counted nowhere. */
		if (e.filter != QStringLiteral("all") && counts.contains(e.filter))
			counts[e.filter] += 1;
	}
	return counts;
}

QString relativeTime(const QString &iso)
{
	const QDateTime ts = QDateTime::fromString(iso, Qt::ISODateWithMs);
	if (!ts.isValid())
		return QString();

	const qint64 diffSec = qMax<qint64>(0, ts.secsTo(QDateTime::currentDateTimeUtc()));
	if (diffSec < 60)
		return QStringLiteral("just now");

	const qint64 mins = diffSec / 60;
	if (mins < 60)
		return QStringLiteral("%1m ago").arg(mins);

	const qint64 hrs = mins / 60;
	/* Days matter now that the feed carries backfilled history rather than only this session's
	 * chat: the old branch rendered a three-day-old gift as "72h ago". */
	if (hrs < 24)
		return QStringLiteral("%1h ago").arg(hrs);

	return QStringLiteral("%1d ago").arg(hrs / 24);
}

} // namespace XiveActivity
