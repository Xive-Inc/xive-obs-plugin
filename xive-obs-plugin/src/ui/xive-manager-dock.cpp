#include "ui/xive-manager-dock.h"

#include "ui/xive-category-picker.h"
#include "ui/xive-theme.h"
#include "xive-api.h"
#include "xive-channel.h"
#include "xive-chat-feed.h"
#include "xive-log.h"
#include "xive-quick-actions.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

const char *XiveManagerDock::kDockId = "xive_manager_dock";

XiveManagerDock::XiveManagerDock(bool withQuickActions, QWidget *parent) : QWidget(parent)
{
	XiveTheme::applyDockTheme(this);

	/*
	 * Structure is unchanged from the original dock — stream details first, quick actions
	 * below — because that order is the point of a manager dock. What changed is the chrome:
	 * the popout's header strip, and each group boxed into its own card the way the web panels
	 * are, instead of one flat column separated by bevelled rules.
	 */
	auto *shell = new QVBoxLayout(this);
	shell->setContentsMargins(0, 0, 0, 0);
	shell->setSpacing(0);

	shell->addWidget(XiveTheme::makeHeader(this, tr("Stream manager"), &m_headerDot,
					       &m_headerState));

	auto *layout = new QVBoxLayout();
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);
	shell->addLayout(layout, 1);

	/* ---- stream details ---- */

	auto *detailsCard = XiveTheme::makeCard(this);
	auto *details = new QVBoxLayout(detailsCard);
	details->setContentsMargins(10, 10, 10, 10);
	details->setSpacing(8);

	details->addWidget(XiveTheme::makeSectionLabel(detailsCard, tr("Stream details")));

	m_stateLabel = new QLabel(detailsCard);
	m_stateLabel->setWordWrap(true);
	details->addWidget(m_stateLabel);

	auto *form = new QFormLayout();
	form->setLabelAlignment(Qt::AlignLeft);
	form->setContentsMargins(0, 0, 0, 0);
	form->setSpacing(6);

	m_titleEdit = new QLineEdit(detailsCard);
	m_titleEdit->setMaxLength(120);
	m_titleEdit->setPlaceholderText(tr("What are you streaming?"));
	connect(m_titleEdit, &QLineEdit::textEdited, this, [this]() { m_titleDirty = true; });
	form->addRow(tr("Title"), m_titleEdit);

	m_categoryPicker = new XiveCategoryPicker(detailsCard);
	connect(m_categoryPicker, &XiveCategoryPicker::edited, this,
		[this]() { m_categoryDirty = true; });
	form->addRow(tr("Category"), m_categoryPicker);

	details->addLayout(form);

	/* The save is this card's call to action, so it carries the brand gradient. */
	m_saveButton = new QPushButton(tr("Save"), detailsCard);
	m_saveButton->setObjectName(QStringLiteral("xivePrimary"));
	connect(m_saveButton, &QPushButton::clicked, this, &XiveManagerDock::saveMetadata);
	details->addWidget(m_saveButton);

	layout->addWidget(detailsCard);

	/* ---- go live notification ---- */

	/*
	 * Its own card, not a row in "Stream details", because it is not stored with them: the title
	 * and category belong to the BROADCAST (public.streams, via
	 * /api/streaming/creator/streams/metadata) and this belongs to the CHANNEL
	 * (public.stream_channels, via /api/streaming/creator/channel). Whatever is left here is
	 * also the default next time, which is not true of anything in the card above.
	 *
	 * The Save button covers both, so the split is a fact about storage rather than a second
	 * thing for the creator to remember to press.
	 */
	auto *notifyCard = XiveTheme::makeCard(this);
	auto *notify = new QVBoxLayout(notifyCard);
	notify->setContentsMargins(10, 10, 10, 10);
	notify->setSpacing(8);

	notify->addWidget(XiveTheme::makeSectionLabel(notifyCard, tr("Go live notification")));

	m_notifyCheck = new QCheckBox(tr("Notify my followers when I go live"), notifyCard);
	m_notifyCheck->setChecked(true);
	connect(m_notifyCheck, &QCheckBox::toggled, this, [this]() { m_notifyDirty = true; });
	notify->addWidget(m_notifyCheck);

	m_notifyMessage = new QLineEdit(notifyCard);
	/*
	 * SC_NOTIFY_MESSAGE_MAX in app/streaming/channels.php. The server answers a ValidationError
	 * rather than truncating past it, so the cap is enforced here instead of being discovered by
	 * the creator when a save fails.
	 */
	m_notifyMessage->setMaxLength(200);
	m_notifyMessage->setPlaceholderText(tr("Optional message — leave blank for the default"));
	connect(m_notifyMessage, &QLineEdit::textEdited, this, [this]() { m_notifyDirty = true; });
	notify->addWidget(m_notifyMessage);

	layout->addWidget(notifyCard);

	/* ---- share link ---- */

	/*
	 * Its own card rather than a row inside "Stream details": nothing here is editable and
	 * nothing here is saved by the Save button above, so putting it in that form would invite
	 * exactly the wrong expectation.
	 */
	auto *shareCard = XiveTheme::makeCard(this);
	auto *share = new QVBoxLayout(shareCard);
	share->setContentsMargins(10, 10, 10, 10);
	share->setSpacing(8);

	share->addWidget(XiveTheme::makeSectionLabel(shareCard, tr("Your channel link")));

	auto *shareHint = new QLabel(tr("Post this anywhere to send people to your stream."),
				     shareCard);
	shareHint->setWordWrap(true);
	shareHint->setStyleSheet(XiveTheme::noticeCss(false));
	share->addWidget(shareHint);

	m_shareEdit = new QLineEdit(shareCard);
	m_shareEdit->setReadOnly(true);
	m_shareEdit->setCursorPosition(0);
	m_shareEdit->setPlaceholderText(tr("Resolving your channel…"));
	share->addWidget(m_shareEdit);

	auto *shareButtons = new QHBoxLayout();
	shareButtons->setContentsMargins(0, 0, 0, 0);
	shareButtons->setSpacing(6);

	/* Copying is what this card is for, so it gets the brand gradient; opening the page is the
	 * quiet companion action. */
	m_shareCopyButton = new QPushButton(tr("Copy link"), shareCard);
	m_shareCopyButton->setObjectName(QStringLiteral("xivePrimary"));
	connect(m_shareCopyButton, &QPushButton::clicked, this, &XiveManagerDock::copyChannelLink);
	shareButtons->addWidget(m_shareCopyButton, 1);

	m_shareOpenButton = new QPushButton(tr("Open"), shareCard);
	m_shareOpenButton->setToolTip(tr("Open your channel page in your browser."));
	connect(m_shareOpenButton, &QPushButton::clicked, this, [this]() {
		XiveChannel *ch = XiveChannel::instance();
		const QString url = ch ? ch->channelUrl() : QString();
		if (url.isEmpty())
			return;
		QDesktopServices::openUrl(QUrl(url));
	});
	shareButtons->addWidget(m_shareOpenButton);

	share->addLayout(shareButtons);

	layout->addWidget(shareCard);

	/* ---- quick actions (fallback only) ---- */

	/*
	 * Built ONLY when there is no `/popout/actions` browser dock — see the class comment.
	 *
	 * Where it is built, it is the two actions that have a protocol behind them and no more.
	 * "Edit stream details" is the title/category form above. "Raid channel" is excluded — the
	 * platform does not support raids, and it ends the broadcast server-side. "Run giveaway" is
	 * a permanently disabled placeholder on the web (StudioGiveawayModal.tsx: "giveaway backend
	 * not available"), so there is nothing to port. The web panel has all of them because it is
	 * the web; that gap is exactly why the browser dock is preferred.
	 */
	if (withQuickActions) {
		auto *actionsCard = XiveTheme::makeCard(this);
		auto *actions = new QVBoxLayout(actionsCard);
		actions->setContentsMargins(10, 10, 10, 10);
		actions->setSpacing(8);

		actions->addWidget(XiveTheme::makeSectionLabel(actionsCard, tr("Quick actions")));

		m_pollButton = new QPushButton(tr("Start a poll…"), actionsCard);
		connect(m_pollButton, &QPushButton::clicked, this, &XiveManagerDock::managePoll);
		actions->addWidget(m_pollButton);

		m_adBreakButton = new QPushButton(tr("Announce ad break"), actionsCard);
		connect(m_adBreakButton, &QPushButton::clicked, this,
			&XiveManagerDock::announceAdBreak);
		actions->addWidget(m_adBreakButton);

		layout->addWidget(actionsCard);
	}

	m_statusLabel = new QLabel(this);
	m_statusLabel->setWordWrap(true);
	m_statusLabel->setStyleSheet(XiveTheme::noticeCss(false));
	layout->addWidget(m_statusLabel);

	layout->addStretch(1);

	if (XiveChannel *ch = XiveChannel::instance())
		connect(ch, &XiveChannel::changed, this, &XiveManagerDock::refreshUi);

	refreshUi();
	loadGoLiveNotification();
}

void XiveManagerDock::loadGoLiveNotification()
{
	XiveApi *api = XiveApi::instance();
	if (!api)
		return;

	api->creatorChannel([this](bool ok, const QJsonObject &body, int, const QString &) {
		/*
		 * Silent on failure, deliberately. This runs at dock construction, before the creator
		 * has asked for anything; a red line about a channel read would be the first thing
		 * they saw on a launch where the network was slow. The fields keep their defaults —
		 * notify on, no message — which is also what the server defaults to.
		 */
		if (!ok || !m_notifyCheck || !m_notifyMessage)
			return;

		const QJsonObject channel = body.value(QStringLiteral("channel")).toObject();
		if (channel.isEmpty())
			return;


		/*
		 * Never over a creator mid-edit. `m_notifyDirty` is set the moment either widget is
		 * touched, and this answer can arrive well after that on a slow link.
		 */
		if (m_notifyDirty)
			return;

		/* Absent means the row has no explicit setting; the server's default is on. */
		const QJsonValue notify = channel.value(QStringLiteral("goLiveNotify"));
		m_notifyCheck->setChecked(notify.isBool() ? notify.toBool() : true);

		/* Null is "no message", which is a blank field rather than the string "null". */
		const QJsonValue message = channel.value(QStringLiteral("goLiveMessage"));
		m_notifyMessage->setText(message.isString() ? message.toString() : QString());
	});
}

void XiveManagerDock::saveGoLiveNotification()
{
	XiveApi *api = XiveApi::instance();
	if (!api || !m_notifyCheck || !m_notifyMessage)
		return;

	api->saveGoLiveNotification(
		m_notifyCheck->isChecked(), m_notifyMessage->text().trimmed(),
		[this](bool ok, const QJsonObject &body, int, const QString &error) {
			const bool success = ok && body.value(QStringLiteral("success")).toBool();
			if (!success) {
				/*
				 * ⚠️ REPORTED, NOT FAILED — the title and category are already saved
				 * by the time this runs, and they went through a different route. The
				 * web says the same thing in the same situation (see the inner
				 * try/catch in ObsBroadcastStudio's handleSaveMetadata): telling the
				 * creator nothing saved when most of it did is the worse lie.
				 *
				 * The dirty flag stays set, so their text is still on screen and the
				 * next Save retries it.
				 */
				setStatus(tr("Stream details saved, but the go-live notification "
					     "did not: %1")
						  .arg(error.isEmpty() ? tr("unknown error") : error),
					  true);
				return;
			}
			m_notifyDirty = false;
			setStatus(tr("Saved."), false);
		});
}

void XiveManagerDock::refreshUi()
{
	XiveChannel *ch = XiveChannel::instance();
	if (!ch) {
		m_stateLabel->setText(tr("Not ready."));
		XiveTheme::setLiveState(m_headerDot, m_headerState, false);
		/* Buttons default to enabled, so an early return has to turn them off explicitly or
		 * this branch ships two live buttons with nothing behind them. */
		m_shareCopyButton->setEnabled(false);
		m_shareOpenButton->setEnabled(false);
		return;
	}

	XiveTheme::setLiveState(m_headerDot, m_headerState, ch->isLive());

	/* The header already says LIVE/OFFLINE, so this line carries only what it cannot: the
	 * viewer count, or why there is nothing to manage yet. */
	if (ch->streamId().isEmpty()) {
		m_stateLabel->setStyleSheet(XiveTheme::noticeCss(false));
		m_stateLabel->setText(tr("No Xive channel yet — connect in the Xive dock."));
	} else if (ch->isLive()) {
		m_stateLabel->setStyleSheet(QString());
		m_stateLabel->setText(tr("%n viewer(s) watching", "", ch->viewers()));
	} else {
		m_stateLabel->setStyleSheet(XiveTheme::noticeCss(false));
		m_stateLabel->setText(tr("Not broadcasting."));
	}

	/*
	 * The share link, which is available whether or not the creator is live — the whole point is
	 * to hand it out BEFORE going live. Empty until the handle has been seeded from the stream
	 * detail route; an unresolved handle leaves the placeholder showing and the buttons off,
	 * because a half-built URL that 404s is worse than an obviously-not-ready field.
	 */
	const QString shareUrl = ch->channelUrl();
	if (m_shareEdit->text() != shareUrl) {
		m_shareEdit->setText(shareUrl);
		/* Long handles otherwise arrive scrolled to their end. */
		m_shareEdit->setCursorPosition(0);
	}
	m_shareCopyButton->setEnabled(!shareUrl.isEmpty());
	m_shareOpenButton->setEnabled(!shareUrl.isEmpty());

	if (!m_titleDirty)
		m_titleEdit->setText(ch->title());
	/* `isBusy()` covers the gap the dirty flag does not: the popup is open and nothing has been
	 * picked yet, so there is no edit to be dirty about, but writing the field would still yank
	 * the list out from under the creator mid-choice. */
	if (!m_categoryDirty && !m_categoryPicker->isBusy())
		m_categoryPicker->setCategory(ch->category(), ch->categorySlug());

	m_saveButton->setEnabled(!ch->streamId().isEmpty());

	/*
	 * Both quick actions post into the live chat, so they only mean anything while live — and
	 * they are null whenever a browser dock is carrying the actions instead, which is the
	 * normal case now. Guarded rather than assumed: refreshUi() runs on every channel change.
	 */
	if (m_pollButton && m_adBreakButton) {
		const bool live = ch->isLive();
		m_pollButton->setEnabled(live);
		m_adBreakButton->setEnabled(live);
		const QString onlyLive = live ? QString() : tr("Available once you are live.");
		m_pollButton->setToolTip(onlyLive);
		m_adBreakButton->setToolTip(onlyLive);
	}
}

void XiveManagerDock::copyChannelLink()
{
	XiveChannel *ch = XiveChannel::instance();
	const QString url = ch ? ch->channelUrl() : QString();
	if (url.isEmpty()) {
		setStatus(tr("Your channel link is not ready yet."), true);
		return;
	}

	QClipboard *clipboard = QApplication::clipboard();
	if (!clipboard) {
		/* Headless or a broken session: say so rather than reporting a copy that did not
		 * happen. The field is selectable, so there is still a way out. */
		setStatus(tr("Could not reach the clipboard — select the link above and copy it."),
			  true);
		return;
	}

	clipboard->setText(url);
	/*
	 * X11 keeps a second, middle-click buffer that is not the Ctrl+V one. Filling both means the
	 * link pastes wherever the creator reaches for it; Qt reports Selection unsupported on
	 * Wayland/Windows/macOS, and this is skipped there.
	 */
	if (clipboard->supportsSelection())
		clipboard->setText(url, QClipboard::Selection);

	/* Select it too: visible confirmation that this exact string is what went to the clipboard. */
	m_shareEdit->setFocus(Qt::OtherFocusReason);
	m_shareEdit->selectAll();

	setStatus(tr("Channel link copied — paste it wherever you want to post it."), false);
}

void XiveManagerDock::announceAdBreak()
{
	XiveChatFeed *feed = XiveChatFeed::instance();
	if (!feed)
		return;

	m_adBreakButton->setEnabled(false);
	setStatus(tr("Announcing an ad break…"), false);

	feed->send(XiveQuickActions::adBreakMessage(), [this](bool ok, const QString &error) {
		m_adBreakButton->setEnabled(true);
		if (!ok) {
			setStatus(error, true);
			return;
		}
		setStatus(tr("Ad break announced in chat."), false);
	});
}

void XiveManagerDock::managePoll()
{
	XiveChatFeed *feed = XiveChatFeed::instance();
	if (!feed)
		return;

	/*
	 * A poll is a specially-formatted chat message, so it needs a question and its options
	 * gathered before anything is sent. One dialog, options one per line — simpler than a
	 * growing list of fields, and it maps directly onto the payload.
	 */
	QDialog dialog(this);
	dialog.setWindowTitle(tr("Start a poll"));
	/* A dialog is a top-level window, so it inherits nothing from the dock — theme it directly
	 * or it opens as a stock grey box in front of a dark dock. */
	XiveTheme::applyDockTheme(&dialog);

	auto *form = new QVBoxLayout(&dialog);
	form->setContentsMargins(12, 12, 12, 12);
	form->setSpacing(8);

	form->addWidget(new QLabel(tr("Question"), &dialog));
	auto *question = new QLineEdit(&dialog);
	question->setMaxLength(140);
	form->addWidget(question);

	form->addWidget(new QLabel(tr("Options — one per line, %1 to %2")
					   .arg(XiveQuickActions::kMinPollOptions)
					   .arg(XiveQuickActions::kMaxPollOptions),
				   &dialog));
	auto *options = new QPlainTextEdit(&dialog);
	options->setPlaceholderText(tr("Yes\nNo"));
	form->addWidget(options);

	auto *error = new QLabel(&dialog);
	error->setWordWrap(true);
	error->setStyleSheet(XiveTheme::noticeCss(true));
	form->addWidget(error);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
					     &dialog);
	buttons->button(QDialogButtonBox::Ok)->setText(tr("Post poll"));
	buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("xivePrimary"));
	form->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
		const QStringList lines =
			options->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
		/* Validate in the dialog rather than after closing it, so a rejected poll does not
		 * lose what was typed. */
		const QString why = XiveQuickActions::pollValidationError(question->text(), lines);
		if (!why.isEmpty()) {
			error->setText(why);
			return;
		}
		dialog.accept();
	});

	if (dialog.exec() != QDialog::Accepted)
		return;

	const QStringList lines = options->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	const QString payload = XiveQuickActions::buildPollMessage(question->text(), lines);
	if (payload.isEmpty()) {
		setStatus(tr("That poll could not be built."), true);
		return;
	}

	m_pollButton->setEnabled(false);
	setStatus(tr("Posting the poll…"), false);

	feed->send(payload, [this](bool ok, const QString &error2) {
		m_pollButton->setEnabled(true);
		if (!ok) {
			setStatus(error2, true);
			return;
		}
		setStatus(tr("Poll posted to chat."), false);
	});
}

void XiveManagerDock::setStatus(const QString &text, bool isError)
{
	m_statusLabel->setStyleSheet(XiveTheme::noticeCss(isError));
	m_statusLabel->setText(text);
}

void XiveManagerDock::saveMetadata()
{
	XiveChannel *ch = XiveChannel::instance();
	if (!ch)
		return;

	m_saveButton->setEnabled(false);
	setStatus(tr("Saving…"), false);

	ch->saveMetadata(m_titleEdit->text(), m_categoryPicker->categoryName(),
			 m_categoryPicker->categorySlug(),
			 [this](bool ok, const QString &error) {
				 m_saveButton->setEnabled(true);
				 if (!ok) {
					 setStatus(error, true);
					 return;
				 }
				 /* Only drop the dirty flags on success, so a failed save does
				  * not silently lose what the creator typed. */
				 m_titleDirty = false;
				 m_categoryDirty = false;
				 setStatus(tr("Saved."), false);
				 refreshUi();

				 /*
				  * The notification is a SECOND call, to a different route, on a
				  * different table — see saveGoLiveNotification(). Sequenced after
				  * this one rather than fired alongside it so that its failure
				  * message, if it comes, replaces "Saved." instead of racing it.
				  */
				 saveGoLiveNotification();
			 });
}

