#include "ui/xive-activity-dock.h"

#include "ui/xive-theme.h"
#include "xive-activity.h"
#include "xive-activity-feed.h"
#include "xive-channel.h"
#include "xive-chat-feed.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

const char *XiveActivityDock::kDockId = "xive_activity_dock";

namespace {

/* Studio palette, same event-type mapping: --studio-yellow / -orange / -pink / -green / -blue. */
QString colourForType(const QString &type)
{
	if (type == QStringLiteral("gift"))
		return QStringLiteral("#ffd34d");
	if (type == QStringLiteral("super-chat"))
		return QStringLiteral("#ff9d2e");
	if (type == QStringLiteral("subscription") || type == QStringLiteral("gifted-sub") ||
	    type == QStringLiteral("supporter-tier"))
		return QStringLiteral("#ff2e93");
	if (type == QStringLiteral("follow"))
		return QStringLiteral("#24e28a");
	/* Sparks get the studio's purple; they had no colour before because they had no way of
	 * reaching this dock at all. */
	if (type == QStringLiteral("spark"))
		return QStringLiteral("#a855f7");
	if (type == QStringLiteral("marker") || type == QStringLiteral("raid"))
		return QStringLiteral("#4977ff");
	return QString::fromUtf8(XiveTheme::kInkMuted);
}

} // namespace

XiveActivityDock::XiveActivityDock(QWidget *parent) : QWidget(parent)
{
	XiveTheme::applyDockTheme(this);

	auto *shell = new QVBoxLayout(this);
	shell->setContentsMargins(0, 0, 0, 0);
	shell->setSpacing(0);

	shell->addWidget(XiveTheme::makeHeader(this, tr("Activity"), &m_headerDot, &m_headerState));

	auto *layout = new QVBoxLayout();
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);
	shell->addLayout(layout, 1);

	auto *row = new QHBoxLayout();
	row->setSpacing(6);
	row->addWidget(XiveTheme::makeSectionLabel(this, tr("Show")));

	m_filter = new QComboBox(this);
	for (const auto &option : XiveActivity::filterOptions())
		m_filter->addItem(option.second, option.first);
	connect(m_filter, &QComboBox::currentIndexChanged, this, [this]() { rebuild(); });
	row->addWidget(m_filter, 1);
	layout->addLayout(row);

	m_notice = new QLabel(this);
	m_notice->setWordWrap(true);
	m_notice->setStyleSheet(XiveTheme::noticeCss(false));
	m_notice->setVisible(false);
	layout->addWidget(m_notice);

	m_view = new QTextBrowser(this);
	m_view->setOpenExternalLinks(false);
	m_view->setFrameShape(QFrame::NoFrame);
	m_view->document()->setDefaultStyleSheet(XiveTheme::documentStyleSheet());
	layout->addWidget(m_view, 1);

	if (XiveChannel *ch = XiveChannel::instance()) {
		connect(ch, &XiveChannel::changed, this, [this]() {
			XiveChannel *live = XiveChannel::instance();
			XiveTheme::setLiveState(m_headerDot, m_headerState, live && live->isLive());
		});
		XiveTheme::setLiveState(m_headerDot, m_headerState, ch->isLive());
	}

	/*
	 * Two sources, deliberately. The events themselves come from the activity route; the chat
	 * buffer is still consulted for super chats, whose `is_priority` flag lives on the message.
	 * See the header of xive-activity.h for why this dock no longer classifies chat text.
	 */
	if (XiveActivityFeed *activity = XiveActivityFeed::instance()) {
		connect(activity, &XiveActivityFeed::changed, this, [this]() { rebuild(); });
		connect(activity, &XiveActivityFeed::notice, this, [this](const QString &message) {
			m_notice->setText(message);
			m_notice->setVisible(!message.isEmpty());
		});
	}

	if (XiveChatFeed *feed = XiveChatFeed::instance()) {
		connect(feed, &XiveChatFeed::appended, this, [this]() { rebuild(); });
		connect(feed, &XiveChatFeed::reset, this, [this]() { rebuild(); });
		connect(feed, &XiveChatFeed::rebuilt, this, [this]() { rebuild(); });
	}

	/* "5m ago" has to keep up on its own — nothing else redraws the list while chat is quiet. */
	m_relativeTimeTimer = new QTimer(this);
	m_relativeTimeTimer->setInterval(30000);
	connect(m_relativeTimeTimer, &QTimer::timeout, this, [this]() {
		if (isVisible())
			rebuild();
	});
	m_relativeTimeTimer->start();

	rebuild();
}

void XiveActivityDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (!m_counted) {
		if (XiveChatFeed *feed = XiveChatFeed::instance())
			feed->addViewer();
		if (XiveActivityFeed *activity = XiveActivityFeed::instance())
			activity->addViewer();
		m_counted = true;
	}
}

void XiveActivityDock::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (m_counted) {
		if (XiveChatFeed *feed = XiveChatFeed::instance())
			feed->removeViewer();
		if (XiveActivityFeed *activity = XiveActivityFeed::instance())
			activity->removeViewer();
		m_counted = false;
	}
}

void XiveActivityDock::rebuild()
{
	XiveActivityFeed *activity = XiveActivityFeed::instance();
	XiveChatFeed *feed = XiveChatFeed::instance();
	if (!activity)
		return;

	/* Chat is optional here: if its dock has never been opened the buffer is simply empty, and
	 * the only thing lost is super chats. The activity route carries everything else. */
	const QList<QJsonObject> messages = feed ? feed->messages() : QList<QJsonObject>();
	const QList<XiveActivity::Event> all = XiveActivity::mergeEvents(activity->events(), messages);
	const QMap<QString, int> counts = XiveActivity::countByFilter(all);

	/* Put the count next to each label so the broadcaster can see what happened without
	 * switching filters. */
	for (int i = 0; i < m_filter->count(); ++i) {
		const QString id = m_filter->itemData(i).toString();
		QString label;
		for (const auto &option : XiveActivity::filterOptions()) {
			if (option.first == id) {
				label = option.second;
				break;
			}
		}
		const int n = counts.value(id, 0);
		m_filter->setItemText(i, n > 0 ? QStringLiteral("%1 (%2)").arg(label).arg(n)
					       : label);
	}

	const QString filter = m_filter->currentData().toString();
	const QList<XiveActivity::Event> shown = XiveActivity::filterEvents(all, filter);

	if (shown.isEmpty()) {
		m_view->clear();
		if (m_notice->text().isEmpty()) {
			m_notice->setText(all.isEmpty()
						  ? tr("No activity yet this broadcast.")
						  : tr("Nothing matching that filter yet."));
			m_notice->setVisible(true);
		}
		return;
	}

	QString html;
	for (const XiveActivity::Event &e : shown) {
		const QString when = XiveActivity::relativeTime(e.createdAtIso);
		html += QStringLiteral(
				"<div style=\"margin:4px 0;padding-left:6px;"
				"border-left:3px solid %1;\">"
				"<b style=\"color:%1;\">%2</b>%3<br>"
				"<span style=\"color:#e4e4e7;\">%4</span>"
				"<span style=\"color:#71717a;\"> · %5</span></div>")
				.arg(colourForType(e.type), e.displayName.toHtmlEscaped(),
				     e.amountLabel.isEmpty()
					     ? QString()
					     : QStringLiteral(
							 " <span style=\"color:#ffd34d;\">%1</span>")
						       .arg(e.amountLabel.toHtmlEscaped()),
				     e.description.toHtmlEscaped(), when);
	}

	m_view->setHtml(html);
}
