#include "ui/xive-blocked-dialog.h"

#include "ui/xive-theme.h"
#include "xive-api.h"
#include "xive-channel.h"
#include "xive-chat-feed.h"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>

namespace {

/** The name to show: display name, then handle, then the raw id — never nothing. */
QString displayFor(const QJsonObject &ban)
{
	const QString display = ban.value(QStringLiteral("display_name")).toString().trimmed();
	if (!display.isEmpty())
		return display;
	const QString username = ban.value(QStringLiteral("username")).toString().trimmed();
	if (!username.isEmpty())
		return QStringLiteral("@") + username;
	/*
	 * A profile that was deleted after being banned joins to nothing — the server keeps the row
	 * and returns it with null names precisely so the ban stays liftable. Falling back to the id
	 * is ugly and correct; showing "Unknown" for several rows would make them indistinguishable,
	 * and this list exists to be acted on.
	 */
	return ban.value(QStringLiteral("user_id")).toString();
}

/** "3 days ago", from the ISO-8601 the roster carries. Empty when it cannot be read. */
QString whenFor(const QString &iso)
{
	const QDateTime at = QDateTime::fromString(iso, Qt::ISODateWithMs);
	if (!at.isValid())
		return QString();

	const qint64 secs = at.secsTo(QDateTime::currentDateTimeUtc());
	if (secs < 60)
		return QObject::tr("just now");
	if (secs < 3600)
		return QObject::tr("%1m ago").arg(secs / 60);
	if (secs < 86400)
		return QObject::tr("%1h ago").arg(secs / 3600);
	return QObject::tr("%1d ago").arg(secs / 86400);
}

} // namespace

XiveBlockedDialog::XiveBlockedDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(tr("Blocked viewers"));
	setMinimumSize(380, 320);
	XiveTheme::applyDockTheme(this);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(8);

	auto *intro = new QLabel(
		tr("People blocked from this channel's chat. Unblocking lets them post again — it "
		   "does not bring back the messages hidden when they were blocked."),
		this);
	intro->setWordWrap(true);
	intro->setStyleSheet(XiveTheme::noticeCss(false));
	layout->addWidget(intro);

	/*
	 * Scrolled, because the roster is capped server-side rather than paged and a busy channel can
	 * return a screenful. A dialog that grew past the monitor would put the Close button
	 * off-screen.
	 */
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *host = new QWidget(scroll);
	m_list = new QVBoxLayout(host);
	m_list->setContentsMargins(0, 0, 0, 0);
	m_list->setSpacing(4);
	m_list->addStretch(1);
	scroll->setWidget(host);
	layout->addWidget(scroll, 1);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	m_status->setStyleSheet(XiveTheme::noticeCss(false));
	layout->addWidget(m_status);

	auto *buttons = new QHBoxLayout();
	buttons->setSpacing(6);
	m_refresh = new QPushButton(tr("Refresh"), this);
	connect(m_refresh, &QPushButton::clicked, this, &XiveBlockedDialog::reload);
	buttons->addWidget(m_refresh);
	buttons->addStretch(1);
	auto *close = new QPushButton(tr("Close"), this);
	close->setObjectName(QStringLiteral("xivePrimary"));
	connect(close, &QPushButton::clicked, this, &QDialog::accept);
	buttons->addWidget(close);
	layout->addLayout(buttons);

	reload();
}

void XiveBlockedDialog::setStatus(const QString &text, bool isError)
{
	m_status->setStyleSheet(XiveTheme::noticeCss(isError));
	m_status->setText(text);
	m_status->setVisible(!text.isEmpty());
}

void XiveBlockedDialog::reload()
{
	if (m_loading)
		return;

	XiveApi *api = XiveApi::instance();
	XiveChannel *channel = XiveChannel::instance();
	if (!api || !channel)
		return;

	const QString streamId = channel->streamId();
	if (streamId.isEmpty()) {
		setStatus(tr("No Xive channel yet."), true);
		return;
	}

	m_loading = true;
	m_refresh->setEnabled(false);
	setStatus(tr("Loading…"), false);

	/*
	 * 🔴 QPointer, NOT `this`. The dock opens this with WA_DeleteOnClose, so a moderator who
	 * closes the window while the roster is still in flight destroys the dialog before the reply
	 * lands — and every line in this callback touches a member. The singletons elsewhere in the
	 * plugin capture `this` safely because they outlive the process; a dialog does not.
	 */
	QPointer<XiveBlockedDialog> self(this);
	api->chatRestrictions(streamId, [self](bool ok, const QJsonObject &body, int status,
					       const QString &error) {
		if (!self)
			return;
		self->m_loading = false;
		self->m_refresh->setEnabled(true);

		if (!ok || !body.value(QStringLiteral("success")).toBool(false)) {
			/*
			 * 403 is worth its own sentence. This route is creator-or-moderator, and a
			 * plain "Forbidden" in a dock the broadcaster is looking at reads as a bug
			 * rather than as the one thing it actually means.
			 */
			self->setStatus(status == 403
						? tr("Only the channel's creator and its moderators "
						     "can see who is blocked.")
						: (error.isEmpty()
							   ? tr("Could not load the blocked list.")
							   : error),
					true);
			return;
		}

		QList<QJsonObject> bans;
		const QJsonArray raw = body.value(QStringLiteral("bans")).toArray();
		for (const QJsonValue &entry : raw) {
			const QJsonObject ban = entry.toObject();
			if (!ban.value(QStringLiteral("user_id")).toString().isEmpty())
				bans.append(ban);
		}
		self->applyRows(bans);
	});
}

void XiveBlockedDialog::applyRows(const QList<QJsonObject> &bans)
{
	/* Clear every row but keep the trailing stretch, which is what holds a short list at the
	 * top of the scroll area instead of centring it. */
	while (m_list->count() > 1) {
		QLayoutItem *item = m_list->takeAt(0);
		if (item->widget())
			item->widget()->deleteLater();
		delete item;
	}

	if (bans.isEmpty()) {
		/* "Nobody is blocked" and "we could not find out" must never look the same in a
		 * moderation window — the failure path above sets an error instead of an empty list. */
		setStatus(tr("Nobody is blocked from this chat."), false);
		return;
	}

	setStatus(QString(), false);
	for (const QJsonObject &ban : bans)
		m_list->insertWidget(m_list->count() - 1, makeRow(ban));
}

QWidget *XiveBlockedDialog::makeRow(const QJsonObject &ban)
{
	const QString userId = ban.value(QStringLiteral("user_id")).toString();
	const QString name = displayFor(ban);

	QFrame *row = XiveTheme::makeCard(this);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(8);

	auto *text = new QVBoxLayout();
	text->setSpacing(1);

	auto *who = new QLabel(name, row);
	/* Display names are viewer input. Plain text, never rich text — a QLabel auto-detects HTML,
	 * so a name containing markup would render as markup in the broadcaster's window. */
	who->setTextFormat(Qt::PlainText);
	who->setStyleSheet(QStringLiteral("color:%1; font-weight:600;")
				   .arg(QString::fromUtf8(XiveTheme::kInk)));
	text->addWidget(who);

	/* One muted line: when, who did it, and why — whichever of the three the server knew. */
	QStringList detail;
	const QString when = whenFor(ban.value(QStringLiteral("created_at")).toString());
	if (!when.isEmpty())
		detail << tr("Blocked %1").arg(when);
	const QString by = ban.value(QStringLiteral("banned_by_username")).toString().trimmed();
	if (!by.isEmpty()) {
		detail << tr("by @%1").arg(by);
	} else if (!ban.value(QStringLiteral("banned_by")).toString().isEmpty()) {
		detail << tr("by a moderator");
	} else {
		/*
		 * A null `banned_by` is the automatic strike ladder, not a person — the server records
		 * the actor precisely so this distinction can be drawn, and saying "by a moderator"
		 * here would blame somebody for something nobody did.
		 */
		detail << tr("automatically");
	}
	const QString reason = ban.value(QStringLiteral("reason")).toString().trimmed();
	if (!reason.isEmpty())
		detail << QStringLiteral("— ") + reason;

	auto *meta = new QLabel(detail.join(QStringLiteral(" ")), row);
	meta->setTextFormat(Qt::PlainText); /* `reason` is moderator-authored free text. */
	meta->setWordWrap(true);
	meta->setStyleSheet(QStringLiteral("color:%1; font-size:10px;")
				    .arg(QString::fromUtf8(XiveTheme::kInkMuted)));
	text->addWidget(meta);
	layout->addLayout(text, 1);

	auto *unblock = new QPushButton(tr("Unblock"), row);
	unblock->setCursor(Qt::PointingHandCursor);
	layout->addWidget(unblock, 0);

	connect(unblock, &QPushButton::clicked, this, [this, row, unblock, userId, name]() {
		XiveChatFeed *feed = XiveChatFeed::instance();
		if (!feed)
			return;
		unblock->setEnabled(false);
		/* Same hazard as the read, one level worse: the row widget can also be gone. Both are
		 * QPointers, so a window closed mid-unblock drops the result instead of writing into
		 * freed memory. The unban itself still lands server-side, which is the right outcome. */
		QPointer<XiveBlockedDialog> self(this);
		QPointer<QWidget> rowRef(row);
		QPointer<QPushButton> buttonRef(unblock);
		feed->unbanUser(userId, [self, rowRef, buttonRef, name](bool ok,
								       const QString &error) {
			if (!self)
				return;
			if (!ok) {
				if (buttonRef)
					buttonRef->setEnabled(true);
				self->setStatus(error.isEmpty()
							? tr("Could not unblock %1.").arg(name)
							: error,
						true);
				return;
			}
			/*
			 * Removed here rather than by reloading the whole list. A reload would fight
			 * a moderator working through several rows — every unblock would rebuild the
			 * window under their cursor and move the next button they were reaching for.
			 */
			if (rowRef)
				rowRef->deleteLater();
			self->setStatus(tr("%1 can post again.").arg(name), false);
		});
	});

	return row;
}
