#include "ui/xive-chat-dock.h"

#include "ui/xive-blocked-dialog.h"
#include "ui/xive-theme.h"
#include "xive-channel.h"
#include "xive-chat-feed.h"
#include "xive-chat-render.h"
#include "xive-quick-actions.h"
#include "xive-surge.h"
#include "xive-surge-feed.h"

#include <QAction>
#include <QCursor>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

const char *XiveChatDock::kDockId = "xive_chat_dock";

XiveChatDock::XiveChatDock(QWidget *parent) : QWidget(parent)
{
	XiveTheme::applyDockTheme(this);

	auto *shell = new QVBoxLayout(this);
	shell->setContentsMargins(0, 0, 0, 0);
	shell->setSpacing(0);

	shell->addWidget(XiveTheme::makeHeader(this, tr("Live chat"), &m_headerDot, &m_headerState,
					       &m_headerMeta));

	auto *layout = new QVBoxLayout();
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);
	shell->addLayout(layout, 1);

	m_notice = new QLabel(this);
	m_notice->setWordWrap(true);
	m_notice->setStyleSheet(XiveTheme::noticeCss(false));
	m_notice->setVisible(false);
	layout->addWidget(m_notice);

	buildSurgeBanner(layout);

	/*
	 * The pinned banner, above the feed and outside it.
	 *
	 * Deliberately NOT a paragraph inside the QTextBrowser. That document is append-only — the
	 * whole dock is built around never revising it — and a pin changes and clears in place, which
	 * would force a full renderAll() every time. A separate widget also survives the scrollback
	 * being trimmed, which matters because a pinned message is frequently older than the
	 * newest 50 and absent from the buffer entirely.
	 */
	m_pinFrame = new QFrame(this);
	m_pinFrame->setObjectName(QStringLiteral("xivePinBanner"));
	m_pinFrame->setStyleSheet(QStringLiteral("#xivePinBanner {"
						 "  background: #1d1a2e;"
						 "  border: 1px solid #4c3f8f;"
						 "  border-radius: 8px;"
						 "}"));
	auto *pinLayout = new QVBoxLayout(m_pinFrame);
	pinLayout->setContentsMargins(8, 6, 8, 7);
	pinLayout->setSpacing(2);

	auto *pinTop = new QHBoxLayout();
	pinTop->setSpacing(6);
	m_pinTitle = new QLabel(m_pinFrame);
	m_pinTitle->setStyleSheet(QStringLiteral("color:#b9a9ff; font-size:10px; font-weight:600;"));
	pinTop->addWidget(m_pinTitle, 1);

	m_pinUnpin = new QPushButton(tr("Unpin"), m_pinFrame);
	m_pinUnpin->setCursor(Qt::PointingHandCursor);
	m_pinUnpin->setStyleSheet(QStringLiteral("QPushButton {"
						 "  color:#b9a9ff; background:transparent;"
						 "  border:none; font-size:10px; padding:0 2px;"
						 "} QPushButton:hover { color:#ffffff; }"));
	connect(m_pinUnpin, &QPushButton::clicked, this, [this]() {
		XiveChatFeed *live = XiveChatFeed::instance();
		if (!live)
			return;
		runModeration(tr("Message unpinned."),
			      [live](auto done) { live->unpinMessage(std::move(done)); });
	});
	pinTop->addWidget(m_pinUnpin, 0);
	pinLayout->addLayout(pinTop);

	m_pinBody = new QLabel(m_pinFrame);
	m_pinBody->setWordWrap(true);
	/* See the header: chat content is viewer input and must never be parsed as markup. */
	m_pinBody->setTextFormat(Qt::PlainText);
	m_pinBody->setStyleSheet(QStringLiteral("color:#e6e2f5; font-size:11px;"));
	pinLayout->addWidget(m_pinBody);

	m_pinFrame->setVisible(false);
	layout->addWidget(m_pinFrame);

	m_view = new QTextBrowser(this);
	m_view->setOpenExternalLinks(false);
	m_view->setOpenLinks(false);
	m_view->setFrameShape(QFrame::NoFrame);
	/* Rich text ignores the widget stylesheet, so the feed carries its own type ramp. */
	m_view->document()->setDefaultStyleSheet(XiveTheme::chatDocumentStyleSheet());
	/* setOpenLinks(false) above is what makes this safe: the anchor is a private scheme, and
	 * without it a click would try to navigate the browser to "xive-msg:…". */
	connect(m_view, &QTextBrowser::anchorClicked, this, &XiveChatDock::onAnchorClicked);
	m_view->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_view, &QWidget::customContextMenuRequested, this,
		&XiveChatDock::onFeedContextMenu);
	layout->addWidget(m_view, 1);

	/*
	 * The edit banner. Hidden in normal composing, so the composer is exactly what it was; shown
	 * only while a message is loaded for editing, where it does two jobs — it names what is being
	 * edited so a forgotten edit mode cannot silently swallow the next thing typed, and it warns
	 * that the result is a NEW message at the bottom of chat rather than an in-place correction.
	 */
	m_editFrame = new QFrame(this);
	m_editFrame->setObjectName(QStringLiteral("xiveEditBanner"));
	m_editFrame->setStyleSheet(QStringLiteral("#xiveEditBanner {"
						  "  background: #1a1726;"
						  "  border: 1px solid #4a3f74;"
						  "  border-radius: 8px;"
						  "}"));
	auto *editRow = new QHBoxLayout(m_editFrame);
	editRow->setContentsMargins(8, 5, 8, 6);
	editRow->setSpacing(6);

	m_editLabel = new QLabel(m_editFrame);
	m_editLabel->setWordWrap(true);
	m_editLabel->setTextFormat(Qt::PlainText);
	m_editLabel->setStyleSheet(QStringLiteral("color:#c3b6ff; font-size:10px;"));
	editRow->addWidget(m_editLabel, 1);

	m_editCancel = new QPushButton(tr("Cancel"), m_editFrame);
	m_editCancel->setCursor(Qt::PointingHandCursor);
	m_editCancel->setStyleSheet(QStringLiteral("QPushButton {"
						   "  color:#c3b6ff; background:transparent;"
						   "  border:none; font-size:10px; padding:0 2px;"
						   "} QPushButton:hover { color:#ffffff; }"));
	connect(m_editCancel, &QPushButton::clicked, this, &XiveChatDock::cancelEdit);
	editRow->addWidget(m_editCancel, 0);

	m_editFrame->setVisible(false);
	layout->addWidget(m_editFrame);

	auto *row = new QHBoxLayout();
	row->setSpacing(6);
	m_input = new QLineEdit(this);
	m_input->setPlaceholderText(tr("Send a message…"));
	m_input->setMaxLength(500);
	connect(m_input, &QLineEdit::returnPressed, this, &XiveChatDock::sendMessage);
	row->addWidget(m_input, 1);

	/*
	 * Escape leaves edit mode. Scoped to the composer with Qt::WidgetShortcut rather than to the
	 * dock: an application-wide Escape inside OBS would fight whatever else the creator has
	 * focused, and this only ever makes sense while the cursor is in the box.
	 */
	auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_input);
	escape->setContext(Qt::WidgetShortcut);
	connect(escape, &QShortcut::activated, this, &XiveChatDock::cancelEdit);

	m_sendButton = new QPushButton(tr("Send"), this);
	m_sendButton->setObjectName(QStringLiteral("xivePrimary"));
	connect(m_sendButton, &QPushButton::clicked, this, &XiveChatDock::sendMessage);
	row->addWidget(m_sendButton);

	layout->addLayout(row);

	/* The header mirrors the channel, exactly as the popout's does. */
	if (XiveChannel *ch = XiveChannel::instance())
		connect(ch, &XiveChannel::changed, this, &XiveChatDock::updateHeader);
	updateHeader();

	if (XiveChatFeed *feed = XiveChatFeed::instance()) {
		connect(feed, &XiveChatFeed::appended, this, &XiveChatDock::appendMessages);
		connect(feed, &XiveChatFeed::reset, this, &XiveChatDock::showEmptyState);
		/*
		 * A reset means a different broadcast, so an id held for editing now belongs to
		 * another channel's chat. Sending it would recall a message on the stream the dock
		 * just left — cancel instead, and the draft goes with it because it was a correction
		 * to a message nobody in this room ever saw.
		 */
		connect(feed, &XiveChatFeed::reset, this, &XiveChatDock::cancelEdit);
		connect(feed, &XiveChatFeed::rebuilt, this, &XiveChatDock::renderAll);
		connect(feed, &XiveChatFeed::pinChanged, this, &XiveChatDock::updatePinBanner);
		connect(feed, &XiveChatFeed::notice, this,
			[this](const QString &message) { setNotice(message); });
	}

	/* Render whatever the feed already holds, so a dock shown later is not empty. That includes
	 * the pin: a dock opened mid-broadcast must show it without waiting for the next change. */
	renderAll();
	if (XiveChatFeed *feed = XiveChatFeed::instance())
		updatePinBanner(feed->pinnedMessage());

	if (XiveSurgeFeed *surge = XiveSurgeFeed::instance())
		connect(surge, &XiveSurgeFeed::changed, this, &XiveChatDock::updateSurgeBanner);
	/* Paint from whatever the feed already holds: a dock opened in the middle of a surge must
	 * show it now, not at the next contribution. */
	updateSurgeBanner();
}

/*
 * The Xive Surge banner, directly over the messages.
 *
 * IT IS ABOUT WHAT THE ROOM IS DOING, which is why it lives on the chat dock and not in the manager
 * dock beside the title field — the same call the website makes, where the banner sits over chat on
 * the watch page rather than in the creator's studio.
 *
 * Widgets, not rich text, for the same reason the pin banner is: the message QTextBrowser is
 * append-only by design and a surge changes in place every second while it runs. Drawing it into
 * the document would force a full renderAll() per tick.
 */
void XiveChatDock::buildSurgeBanner(QVBoxLayout *layout)
{
	/* ---- the active banner --------------------------------------------------------- */

	m_surgeFrame = new QFrame(this);
	m_surgeFrame->setObjectName(QStringLiteral("xiveSurgeBanner"));
	/* Purple → fuchsia, the web's `from-purple-600/20 via-fuchsia-500/15 to-pink-500/20`. QSS
	 * has qlineargradient, which is the one gradient Qt's widget styling does support — unlike
	 * the rich text in the feed below, where the same brand wash has to be a flat colour. */
	m_surgeFrame->setStyleSheet(QStringLiteral(
		"#xiveSurgeBanner {"
		"  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
		"      stop:0 #241a3a, stop:0.5 #2a1b38, stop:1 #33203a);"
		"  border: 1px solid #5b3a86;"
		"  border-radius: 8px;"
		"}"));
	auto *surgeLayout = new QVBoxLayout(m_surgeFrame);
	surgeLayout->setContentsMargins(8, 6, 8, 7);
	surgeLayout->setSpacing(4);

	/* Row 1 — what it is, how big, and how long is left. */
	auto *topRow = new QHBoxLayout();
	topRow->setSpacing(6);

	m_surgeTier = new QLabel(m_surgeFrame);
	m_surgeTier->setStyleSheet(
		QStringLiteral("color:#f0abfc; font-size:11px; font-weight:800;"));
	topRow->addWidget(m_surgeTier, 0);

	m_surgeLevel = new QLabel(m_surgeFrame);
	m_surgeLevel->setStyleSheet(QStringLiteral("color:#fae8ff; background:#3d2a52;"
						   "border-radius:4px; padding:1px 5px;"
						   "font-size:10px; font-weight:800;"));
	topRow->addWidget(m_surgeLevel, 0);
	topRow->addStretch(1);

	m_surgeClock = new QLabel(m_surgeFrame);
	m_surgeClock->setStyleSheet(QStringLiteral("color:#f5d0fe; font-size:12px;"
						   "font-weight:700;"));
	/*
	 * The tooltip carries what the number MEANS, because a bare countdown over a chat reads as
	 * "time until this ends" — which is exactly backwards. Every contribution resets it.
	 */
	m_surgeClock->setToolTip(
		tr("Time left before the surge ends — every gift, Spark or sub resets it"));
	topRow->addWidget(m_surgeClock, 0);
	surgeLayout->addLayout(topRow);

	/*
	 * The window meter. It shows the CLOCK as a proportion of the five-minute window, so it snaps
	 * back to full on every contribution and drains as the room goes quiet — deliberately not
	 * progress towards the next level, which would sit still for minutes at a time. The thing a
	 * room can act on is the clock.
	 */
	m_surgeMeter = new QProgressBar(m_surgeFrame);
	m_surgeMeter->setRange(0, 100);
	m_surgeMeter->setTextVisible(false);
	m_surgeMeter->setFixedHeight(4);
	m_surgeMeter->setStyleSheet(QStringLiteral(
		"QProgressBar { background:#20182e; border:none; border-radius:2px; }"
		"QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
		"    stop:0 #a855f7, stop:1 #f472b6); border-radius:2px; }"));
	surgeLayout->addWidget(m_surgeMeter);

	/*
	 * PEOPLE FIRST, POINTS LAST AND LABELLED. `points` is a weighted score — one large gift is
	 * worth several small ones — so printing it as a count of anything would be a lie the
	 * broadcaster has no way to catch. `events` is the honest count and is printed as one.
	 */
	m_surgeStats = new QLabel(m_surgeFrame);
	m_surgeStats->setWordWrap(true);
	m_surgeStats->setStyleSheet(QStringLiteral("color:#d8d4e3; font-size:10px;"));
	surgeLayout->addWidget(m_surgeStats);

	/*
	 * The contributor belt, with the Surge Igniter first. The web scrolls this as a marquee; a
	 * QLabel does not scroll, so it elides instead — the names are a courtesy and the Igniter,
	 * which is the one that is a callout rather than a list, is the one guaranteed to survive.
	 * Plain text: these are viewer display names and must never be parsed as markup.
	 */
	m_surgeContributors = new QLabel(m_surgeFrame);
	m_surgeContributors->setTextFormat(Qt::PlainText);
	m_surgeContributors->setStyleSheet(QStringLiteral("color:#a29bb8; font-size:10px;"));
	surgeLayout->addWidget(m_surgeContributors);

	m_surgeFrame->setVisible(false);
	layout->addWidget(m_surgeFrame);

	/* ---- the "building up" line ----------------------------------------------------- */

	/*
	 * Shown when the room has started giving but has not reached the threshold. It is the only
	 * state a creator can still change the outcome of, and it is what makes the mechanic
	 * discoverable rather than something that appears from nowhere — the studio's own surge card
	 * shows the same line. One row, quiet, no meter.
	 */
	m_surgeIdleFrame = new QFrame(this);
	m_surgeIdleFrame->setObjectName(QStringLiteral("xiveSurgeIdle"));
	m_surgeIdleFrame->setStyleSheet(QStringLiteral("#xiveSurgeIdle {"
						       "  background: #16131f;"
						       "  border: 1px solid #2e2740;"
						       "  border-radius: 8px;"
						       "}"));
	auto *idleLayout = new QHBoxLayout(m_surgeIdleFrame);
	idleLayout->setContentsMargins(8, 4, 8, 5);
	idleLayout->setSpacing(6);

	m_surgeIdleLabel = new QLabel(m_surgeIdleFrame);
	m_surgeIdleLabel->setWordWrap(true);
	m_surgeIdleLabel->setStyleSheet(QStringLiteral("color:#a1a1aa; font-size:10px;"));
	idleLayout->addWidget(m_surgeIdleLabel, 1);

	m_surgeIdleFrame->setVisible(false);
	layout->addWidget(m_surgeIdleFrame);
}

void XiveChatDock::updateSurgeBanner()
{
	XiveSurgeFeed *feed = XiveSurgeFeed::instance();
	if (!feed || !m_surgeFrame || !m_surgeIdleFrame)
		return;

	const XiveSurge::State &state = feed->state();

	if (!state.active) {
		m_surgeFrame->setVisible(false);
		/*
		 * Nothing at all until somebody has given. This sits directly over the message list in
		 * a panel that is often only a few hundred pixels tall, and a permanent strip
		 * announcing that nothing is happening would cost every broadcaster two lines of chat
		 * for the whole stream.
		 */
		if (state.points <= 0) {
			m_surgeIdleFrame->setVisible(false);
			return;
		}
		m_surgeIdleLabel->setText(tr("⚡ Support is building — %1 of %2 to start a XIVE SURGE.")
						  .arg(state.points)
						  .arg(XiveSurge::kStartThreshold));
		m_surgeIdleFrame->setVisible(true);
		return;
	}

	m_surgeIdleFrame->setVisible(false);

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	const int left = feed->secondsLeft();
	const int level = qMax(1, state.level);

	m_surgeTier->setText(XiveSurge::tier(level).toUpper());
	m_surgeLevel->setText(tr("LVL %1").arg(level));
	m_surgeClock->setText(XiveSurge::formatCountdown(left));
	m_surgeMeter->setValue(XiveSurge::windowPercent(state, now));

	QString stats = (state.participants == 1 ? tr("%1 person") : tr("%1 people"))
				.arg(QLocale().toString(state.participants));
	stats += QStringLiteral(" · ");
	stats += (state.events == 1 ? tr("%1 contribution") : tr("%1 contributions"))
			 .arg(QLocale().toString(state.events));
	stats += QStringLiteral(" · ") + tr("%1 pts").arg(QLocale().toString(state.points));
	/* The gap to the next level is not on the wire — both sides hold the constants and derive
	 * it, which is one fewer field to keep in step. At the ceiling there is no next level to
	 * name, so it says so instead of printing a zero. */
	if (level >= XiveSurge::kMaxLevel)
		stats += QStringLiteral(" · ") + tr("max level");
	else
		stats += QStringLiteral(" · ") +
			 tr("%1 to level %2").arg(XiveSurge::toNextLevel(state)).arg(level + 1);
	m_surgeStats->setText(stats);

	/*
	 * The Igniter is pinned at the front and marked, then the rest of the belt. Deduped against
	 * the Igniter's own name, which is in `contributors` too — the server sends both, because a
	 * client that does not draw the callout still wants them in the list.
	 */
	QStringList belt;
	if (!state.igniterName.isEmpty())
		belt << tr("👑 %1").arg(state.igniterName);
	for (const QString &who : state.contributors) {
		if (who == state.igniterName)
			continue;
		belt << who;
	}
	m_surgeContributors->setText(belt.join(QStringLiteral(" · ")));
	m_surgeContributors->setVisible(!belt.isEmpty());

	m_surgeFrame->setVisible(true);
}

void XiveChatDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (!m_counted) {
		if (XiveChatFeed *feed = XiveChatFeed::instance())
			feed->addViewer();
		/* The surge is its own feed with its own cadence and its own clock, so it is counted
		 * separately — a dock that is hidden must not keep a one-second timer alive. */
		if (XiveSurgeFeed *surge = XiveSurgeFeed::instance())
			surge->addViewer();
		m_counted = true;
	}
}

void XiveChatDock::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (m_counted) {
		if (XiveChatFeed *feed = XiveChatFeed::instance())
			feed->removeViewer();
		if (XiveSurgeFeed *surge = XiveSurgeFeed::instance())
			surge->removeViewer();
		m_counted = false;
	}
}

QString XiveChatDock::selfHandle() const
{
	XiveChannel *channel = XiveChannel::instance();
	return channel ? channel->username() : QString();
}

void XiveChatDock::updateHeader()
{
	XiveChannel *channel = XiveChannel::instance();
	const bool live = channel && channel->isLive();
	XiveTheme::setLiveState(m_headerDot, m_headerState, live);

	if (!m_headerMeta)
		return;

	/*
	 * "1,234 watching", the studio rail's counter. Offline, the LIVE/OFFLINE word already says
	 * everything there is to say, so the label gets out of the way rather than reading
	 * "0 watching" at a creator who has not gone live yet.
	 */
	if (live) {
		m_headerMeta->setText(
			tr("%1 watching").arg(QLocale::system().toString(channel->viewers())));
		m_headerMeta->setVisible(true);
	} else {
		m_headerMeta->clear();
		m_headerMeta->setVisible(false);
	}
}

void XiveChatDock::setNotice(const QString &text)
{
	m_notice->setText(text);
	m_notice->setVisible(!text.isEmpty());
}

void XiveChatDock::showEmptyState()
{
	m_view->clear();
	m_view->append(XiveChatRender::emptyStateHtml());
	m_empty = true;
}

void XiveChatDock::renderAll()
{
	XiveChatFeed *feed = XiveChatFeed::instance();
	const QList<QJsonObject> all = feed ? feed->messages() : QList<QJsonObject>();

	m_view->clear();
	m_empty = false;

	int shown = 0;
	for (const QJsonObject &message : all) {
		const QString html = XiveChatRender::messageHtml(message, all, selfHandle());
		if (html.isEmpty())
			continue;
		m_view->append(html);
		++shown;
	}

	if (shown == 0)
		showEmptyState();
}

void XiveChatDock::appendMessages(const QList<QJsonObject> &messages)
{
	if (messages.isEmpty())
		return;

	/*
	 * A vote revises the tally inside a poll card that is already laid out, and a QTextBrowser
	 * cannot go back and change one. So a batch containing a vote redraws the whole document
	 * instead of appending to it — see the note in the header.
	 */
	bool revisesAPoll = false;
	for (const QJsonObject &message : messages) {
		if (XiveQuickActions::parsePollVote(
			    message.value(QStringLiteral("content")).toString(), nullptr, nullptr)) {
			revisesAPoll = true;
			break;
		}
	}

	/* Stay pinned to the newest message only if the reader was already at the bottom —
	 * otherwise scrolling back through chat would be yanked away on every poll. */
	QScrollBar *bar = m_view->verticalScrollBar();
	const bool atBottom = bar->value() >= bar->maximum() - 4;

	if (revisesAPoll) {
		renderAll();
	} else {
		XiveChatFeed *feed = XiveChatFeed::instance();
		const QList<QJsonObject> all = feed ? feed->messages() : messages;
		for (const QJsonObject &message : messages) {
			const QString html =
				XiveChatRender::messageHtml(message, all, selfHandle());
			if (html.isEmpty())
				continue;
			if (m_empty) {
				m_view->clear();
				m_empty = false;
			}
			/* append() adds a paragraph at the end of the document. insertHtml() would
			 * write wherever the cursor happens to be, which is mid-document as soon as
			 * the user clicks. */
			m_view->append(html);
		}
	}

	if (atBottom)
		bar->setValue(bar->maximum());
}

/* ------------------------------------------------------------------------- */
/* Moderation                                                                */
/* ------------------------------------------------------------------------- */

void XiveChatDock::onAnchorClicked(const QUrl &url)
{
	if (url.scheme() != QStringLiteral("xive-msg"))
		return;

	showModerationMenu(url.path(), QCursor::pos());
}

void XiveChatDock::onFeedContextMenu(const QPoint &pos)
{
	/* Hit-test for the name anchor under the pointer. Right-clicking the message TEXT finds
	 * nothing, which is deliberate — a menu that appears anywhere would fire on a drag-select. */
	const QString href = m_view->anchorAt(pos);
	if (href.isEmpty())
		return;

	const QUrl url(href);
	if (url.scheme() != QStringLiteral("xive-msg"))
		return;

	showModerationMenu(url.path(), m_view->viewport()->mapToGlobal(pos));
}

void XiveChatDock::updatePinBanner(const QJsonObject &pin)
{
	if (!m_pinFrame)
		return;

	if (pin.isEmpty()) {
		m_pinFrame->setVisible(false);
		return;
	}

	const QString author = pin.value(QStringLiteral("display_name")).toString();
	const QString content = pin.value(QStringLiteral("content")).toString();

	/*
	 * "Pinned · <name>", plus the lifetime when there is one. `pin_until_stream_end` and
	 * `pin_expires_at` are mutually exclusive in practice but not in the schema, so both are
	 * reported rather than assuming one wins.
	 */
	QString title = tr("📌 Pinned");
	if (!author.isEmpty())
		title += QStringLiteral(" · ") + author;
	if (pin.value(QStringLiteral("pin_until_stream_end")).toBool())
		title += QStringLiteral(" · ") + tr("until the stream ends");
	else if (!pin.value(QStringLiteral("pin_expires_at")).toString().isEmpty())
		title += QStringLiteral(" · ") + tr("expires");
	m_pinTitle->setText(title);

	m_pinBody->setText(content);

	/*
	 * The Unpin button follows `can_pin`, which is the HOST's Pro. Unpinning itself needs no Pro
	 * server-side, but someone who cannot pin is not this channel's moderator either, so offering
	 * it would only produce a 403.
	 */
	XiveChatFeed *feed = XiveChatFeed::instance();
	m_pinUnpin->setVisible(feed != nullptr && feed->canPin());

	m_pinFrame->setVisible(true);
}

void XiveChatDock::runModeration(
	const QString &done,
	const std::function<void(std::function<void(bool, const QString &)>)> &call)
{
	setNotice(tr("Working…"));
	call([this, done](bool ok, const QString &error) {
		setNotice(ok ? done : error);
		m_notice->setStyleSheet(XiveTheme::noticeCss(!ok));
	});
}

void XiveChatDock::showModerationMenu(const QString &messageId, const QPoint &globalPos)
{
	XiveChatFeed *feed = XiveChatFeed::instance();
	if (!feed || messageId.isEmpty())
		return;

	/* Everything the menu needs comes from the buffer rather than from the href. A message old
	 * enough to have been trimmed is still ON SCREEN — the document is append-only and outlives
	 * the buffer — so this really can miss, and saying so beats a menu that silently no-ops. */
	QJsonObject message;
	for (const QJsonObject &candidate : feed->messages()) {
		if (candidate.value(QStringLiteral("id")).toString() == messageId) {
			message = candidate;
			break;
		}
	}
	if (message.isEmpty()) {
		setNotice(tr("That message has scrolled out of the buffer."));
		return;
	}

	const QString userId = message.value(QStringLiteral("user_id")).toString();
	const QString name = message.value(QStringLiteral("display_name")).toString();
	const bool isSelf = message.value(QStringLiteral("is_creator")).toBool();
	const bool isModerator = message.value(QStringLiteral("is_moderator")).toBool();

	QMenu menu(this);
	menu.setStyleSheet(XiveTheme::menuStyleSheet());

	/* A header, so a misfired click on the wrong name is obvious BEFORE anything is chosen. */
	QAction *heading = menu.addAction(name.isEmpty() ? tr("This viewer") : name);
	heading->setEnabled(false);
	menu.addSeparator();

	/*
	 * Edit leads on your own messages, as it does on the web — it is the reason you opened the
	 * menu on something you wrote, and Delete sits under it where a misfire is less costly.
	 * Offered only on your own lines: the recall route lets anyone delete their own message, but
	 * re-sending as somebody else is not a thing the API can do or should.
	 */
	QAction *editAction = isSelf ? menu.addAction(tr("Edit message")) : nullptr;
	QAction *deleteAction = menu.addAction(tr("Delete message"));

	/*
	 * Pin / unpin.
	 *
	 * Offered only when the API said `can_pin` — that answer folds in the HOST's Xive Pro, which
	 * the plugin has no way to see for itself. Showing the item unconditionally would mean a 403
	 * for every free channel.
	 *
	 * "Until the stream ends" is only offered while live: the server refuses it otherwise with
	 * CHAT_PIN_STREAM_NOT_LIVE, because such a pin would be stored and then hidden immediately.
	 */
	QAction *unpinAction = nullptr;
	/*
	 * Each duration is its own action, mapped to the minutes it sends. A submenu rather than a
	 * flat list of eight: the message menu already carries delete, edit, five moderation items
	 * and the blocked list, and eight more entries would bury all of them.
	 *
	 * Mirrors PIN_DURATION_OPTIONS in utils/streaming/chatPinDuration.ts — the same eight the web
	 * offers, in the same order. The server clamps to 1..1440 (StreamChatPins), so none of these
	 * can be rejected for being out of range.
	 */
	QHash<QAction *, int> pinMinutes;
	QAction *pinUntilEnd = nullptr;
	const bool alreadyPinned =
		feed->pinnedMessage().value(QStringLiteral("id")).toString() == messageId;

	if (feed->canPin()) {
		menu.addSeparator();
		if (alreadyPinned) {
			unpinAction = menu.addAction(tr("Unpin message"));
		} else {
			QMenu *pinMenu = menu.addMenu(tr("Pin message"));
			pinMenu->setStyleSheet(XiveTheme::menuStyleSheet());

			/* 0 minutes is the open-ended pin — XiveApi::chatPin omits the key entirely. */
			pinMinutes.insert(pinMenu->addAction(tr("Until I unpin")), 0);

			/*
			 * Only while live. The server refuses it otherwise with
			 * CHAT_PIN_STREAM_NOT_LIVE, because such a pin would be stored and then
			 * hidden immediately — which reads as the feature being broken.
			 */
			XiveChannel *channel = XiveChannel::instance();
			if (channel && channel->isLive())
				pinUntilEnd = pinMenu->addAction(tr("Until this stream ends"));

			pinMenu->addSeparator();
			const QList<QPair<QString, int>> timed = {
				{tr("1 minute"), 1},   {tr("5 minutes"), 5},
				{tr("10 minutes"), 10}, {tr("30 minutes"), 30},
				{tr("1 hour"), 60},    {tr("24 hours"), 1440},
			};
			for (const auto &option : timed)
				pinMinutes.insert(pinMenu->addAction(option.first), option.second);
		}
	}

	/*
	 * The plugin signs in as the channel's own creator, so a message flagged is_creator is our
	 * own. The web hides its moderation menu on your own messages the same way; muting yourself
	 * is not a feature.
	 */
	QAction *mute5 = nullptr;
	QAction *mute15 = nullptr;
	QAction *unmute = nullptr;
	QAction *ban = nullptr;
	QAction *modToggle = nullptr;

	if (!isSelf && !userId.isEmpty()) {
		menu.addSeparator();
		mute5 = menu.addAction(tr("Mute 5 minutes"));
		mute15 = menu.addAction(tr("Mute 15 minutes"));
		unmute = menu.addAction(tr("Unmute"));
		menu.addSeparator();
		ban = menu.addAction(tr("Block from chat…"));
		menu.addSeparator();
		modToggle = menu.addAction(isModerator ? tr("Remove as moderator")
						       : tr("Make moderator"));
	}

	/*
	 * Always offered, on every message including your own, because it is about NOBODY on screen.
	 * Blocking publishes a purge, so a blocked viewer's lines leave the feed — the person you
	 * would want to unblock is by definition the one you cannot right-click. Hanging it off any
	 * message is arbitrary, and the alternative was leaving `chatUnban` wired to the API and
	 * reachable from nowhere, which is what the dock shipped with while its own block dialog
	 * promised you could "unblock them".
	 */
	menu.addSeparator();
	QAction *blockedList = menu.addAction(tr("Blocked viewers…"));

	QAction *chosen = menu.exec(globalPos);
	if (chosen == nullptr)
		return;

	XiveChatFeed *live = XiveChatFeed::instance();
	if (!live)
		return;

	if (chosen == editAction) {
		beginEdit(message);
		return;
	}

	if (chosen == blockedList) {
		/*
		 * Parented to the dock so it inherits the theme and OBS keeps it above the main
		 * window; WA_DeleteOnClose because a dock can outlive many openings and each one
		 * builds its own rows.
		 */
		auto *dialog = new XiveBlockedDialog(this);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->open();
		return;
	}

	if (chosen == deleteAction) {
		runModeration(tr("Message deleted."), [live, messageId](auto done) {
			live->deleteMessage(messageId, std::move(done));
		});
		return;
	}

	if (chosen == pinUntilEnd) {
		runModeration(tr("Pinned until the stream ends."), [live, messageId](auto done) {
			live->pinMessage(messageId, 0, true, std::move(done));
		});
		return;
	}

	if (pinMinutes.contains(chosen)) {
		const int minutes = pinMinutes.value(chosen);
		/* The confirmation names the duration, because eight options one submenu deep is
		 * exactly where a misfire is plausible and otherwise invisible. */
		const QString done = minutes <= 0
					     ? tr("Message pinned until you unpin it.")
					     : (minutes < 60
							? tr("Pinned for %n minute(s).", "", minutes)
							: (minutes < 1440
								   ? tr("Pinned for %n hour(s).", "",
									minutes / 60)
								   : tr("Pinned for 24 hours.")));
		runModeration(done, [live, messageId, minutes](auto cb) {
			live->pinMessage(messageId, minutes, false, std::move(cb));
		});
		return;
	}

	if (chosen == unpinAction) {
		runModeration(tr("Message unpinned."),
			      [live](auto done) { live->unpinMessage(std::move(done)); });
		return;
	}

	if (chosen == mute5 || chosen == mute15) {
		const int minutes = chosen == mute5 ? 5 : 15;
		runModeration(tr("Muted %1 for %n minute(s).", "", minutes).arg(name),
			      [live, userId, minutes](auto done) {
				      live->timeoutUser(userId, minutes, std::move(done));
			      });
		return;
	}

	if (chosen == unmute) {
		runModeration(tr("Unmuted %1.").arg(name), [live, userId](auto done) {
			live->unmuteUser(userId, std::move(done));
		});
		return;
	}

	if (chosen == ban) {
		/*
		 * The one confirmation. The web does not ask — it can afford not to, because a
		 * misclick there is undone from the same menu a moment later. This is a dock a
		 * broadcaster clicks in while live and looking at a camera, and a block is the action
		 * a viewer notices.
		 */
		const auto choice = QMessageBox::warning(
			this, tr("Block from chat"),
			tr("Block %1 from this broadcast's chat?\n\nThey will not be able to post "
			   "again until you unblock them.")
				.arg(name),
			QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
		if (choice != QMessageBox::Yes)
			return;

		runModeration(tr("%1 is blocked from chat.").arg(name), [live, userId](auto done) {
			live->banUser(userId, std::move(done));
		});
		return;
	}

	if (chosen == modToggle) {
		if (isModerator) {
			runModeration(tr("%1 is no longer a moderator.").arg(name),
				      [live, userId](auto done) {
					      live->removeModerator(userId, std::move(done));
				      });
		} else {
			runModeration(tr("%1 is now a moderator.").arg(name),
				      [live, userId](auto done) {
					      live->addModerator(userId, std::move(done));
				      });
		}
	}
}

void XiveChatDock::sendMessage()
{
	XiveChatFeed *feed = XiveChatFeed::instance();
	if (!feed)
		return;

	const QString text = m_input->text().trimmed();
	if (text.isEmpty())
		return;

	m_input->setEnabled(false);
	m_sendButton->setEnabled(false);

	const QString editingId = m_editingId;

	/*
	 * 🔴 THE DRAFT IS NEVER CLEARED ON FAILURE, and for an edit that is not a nicety. Editing
	 * deletes the original before it sends the replacement, so a send that failed has already
	 * destroyed the message — the text in this box is then the ONLY copy of it in existence.
	 */
	auto finish = [this](bool ok, bool originalDeleted, const QString &error) {
		m_input->setEnabled(true);
		m_sendButton->setEnabled(true);
		m_input->setFocus();

		if (!ok) {
			/*
			 * The original is gone and the replacement never landed. Leaving edit mode ON
			 * would make the obvious retry — press Save again — try to delete a message
			 * that no longer exists, which fails before it ever reaches the send. So the
			 * composer drops back to a normal one holding the same text: pressing Send now
			 * posts the correction as a new message, which is the only thing left that can
			 * work.
			 */
			if (originalDeleted && !m_editingId.isEmpty()) {
				m_editingId.clear();
				updateComposerMode();
			}
			setNotice(error);
			return;
		}

		m_input->clear();
		if (!m_editingId.isEmpty()) {
			m_editingId.clear();
			updateComposerMode();
			setNotice(tr("Edited. The new version is at the bottom of chat."));
			return;
		}
		setNotice(QString());
	};

	if (editingId.isEmpty()) {
		/* A plain send can only fail one way, and it never deletes anything. */
		feed->send(text, [finish](bool ok, const QString &error) {
			finish(ok, false, error);
		});
	} else {
		feed->editMessage(editingId, text, finish);
	}
}

void XiveChatDock::beginEdit(const QJsonObject &message)
{
	m_editingId = message.value(QStringLiteral("id")).toString();
	m_input->setText(message.value(QStringLiteral("content")).toString());
	updateComposerMode();
	m_input->setFocus();
	/* Cursor at the end, not selecting everything: an edit is usually a fix to the tail of a
	 * line, and a select-all means the first keystroke destroys the message being corrected. */
	m_input->setCursorPosition(m_input->text().length());
}

void XiveChatDock::cancelEdit()
{
	if (m_editingId.isEmpty())
		return;
	m_editingId.clear();
	m_input->clear();
	updateComposerMode();
	setNotice(QString());
	m_input->setFocus();
}

void XiveChatDock::updateComposerMode()
{
	const bool editing = !m_editingId.isEmpty();

	m_editFrame->setVisible(editing);
	if (editing) {
		/*
		 * The warning is the point of this label. Chat has no edit endpoint — the platform
		 * defines an edit as a delete plus a re-send — so the corrected line arrives as a new
		 * message at the bottom. Said before anything is sent, because a broadcaster who
		 * expects the original to change in place and instead finds it gone, with a duplicate
		 * further down, would reasonably read that as the dock breaking.
		 */
		m_editLabel->setText(tr("Editing your message. Sending replaces it with a new one at "
					"the bottom of chat."));
	}

	m_sendButton->setText(editing ? tr("Save edit") : tr("Send"));
	m_input->setPlaceholderText(editing ? tr("Edit your message…") : tr("Send a message…"));
}
