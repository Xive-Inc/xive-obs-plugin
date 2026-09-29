#include "ui/xive-category-picker.h"

#include "ui/xive-theme.h"
#include "xive-api.h"
#include "xive-log.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

namespace {

/** Per-row slug, carried on the model item so picking a row yields both halves at once. */
constexpr int kSlugRole = Qt::UserRole + 1;

/*
 * Matches the 220ms the web hook uses (useStreamCategories.ts). Below the point where typing
 * starts to feel laggy, and above the interval that would let a fast typist queue five trigram
 * scans over 16k rows.
 */
constexpr int kDebounceMs = 220;

/** Enough to fill the popup twice over without making the response large. */
constexpr int kResultLimit = 25;

} // namespace

XiveCategoryPicker::XiveCategoryPicker(QWidget *parent) : QWidget(parent)
{
	auto *column = new QVBoxLayout(this);
	column->setContentsMargins(0, 0, 0, 0);
	column->setSpacing(3);

	m_edit = new QLineEdit(this);
	/*
	 * 120, not the 64 the server caps free text at.
	 *
	 * A picked catalogue name is written into this field BY the completer, and 23 listed
	 * categories are longer than 64 characters (the longest is 108). A 64-char maxLength would
	 * silently truncate those on the way in and leave the creator looking at a name that is not
	 * the category they chose.
	 *
	 * The 64-char rule still applies to free text and is enforced where it actually belongs:
	 * updateHint() warns as it is typed, and XiveChannel::saveMetadata refuses the save. Neither
	 * applies to a picked row, which travels as a slug and never as text.
	 */
	m_edit->setMaxLength(120);
	m_edit->setPlaceholderText(tr("Search games and categories"));
	m_edit->setToolTip(tr("Start typing to search the Xive directory, or press ↓ to browse.\n"
			      "Anything not in the directory is saved as you typed it."));
	column->addWidget(m_edit);

	m_edit->installEventFilter(this);

	m_hint = new QLabel(this);
	m_hint->setWordWrap(true);
	column->addWidget(m_hint);

	m_model = new QStandardItemModel(this);

	m_completer = new QCompleter(m_model, this);
	/*
	 * Unfiltered: the SERVER did the matching, over alt-names and trigram similarity that a
	 * local prefix filter knows nothing about. Letting QCompleter filter the results again
	 * would hide every row whose name does not literally start with what was typed — which is
	 * most of what makes the search worth having.
	 */
	m_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
	m_completer->setCaseSensitivity(Qt::CaseInsensitive);
	m_completer->setMaxVisibleItems(8);
	/* A popup is its own top-level window and inherits nothing from the dock's stylesheet, the
	 * same trap the chat dock's moderation menu hits. Theme it directly or it opens as a stock
	 * light-grey list in front of a dark dock. */
	if (QAbstractItemView *popup = m_completer->popup())
		popup->setStyleSheet(XiveTheme::popupListStyleSheet());
	m_edit->setCompleter(m_completer);

	m_debounce = new QTimer(this);
	m_debounce->setSingleShot(true);
	m_debounce->setInterval(kDebounceMs);
	connect(m_debounce, &QTimer::timeout, this, &XiveCategoryPicker::runSearch);

	connect(m_edit, &QLineEdit::textEdited, this, [this]() {
		/* Typed text no longer describes the row that was picked, so the slug goes. Without
		 * this, editing "Minecraft" down to "Mine" would still file the broadcast under
		 * Minecraft. */
		m_slug.clear();
		updateHint();
		emit edited();
		scheduleSearch();
	});

	/*
	 * The QModelIndex overload, not the QString one: the slug lives on the item and the name
	 * alone cannot be mapped back to it — two catalogue rows are allowed to share a name (a
	 * game and its remake), and guessing between them is how a broadcast ends up filed under
	 * the wrong one.
	 */
	connect(m_completer, QOverload<const QModelIndex &>::of(&QCompleter::activated), this,
		[this](const QModelIndex &index) {
			if (!index.isValid())
				return;
			m_slug = index.data(kSlugRole).toString();
			updateHint();
			emit edited();
		});

	updateHint();
}

bool XiveCategoryPicker::eventFilter(QObject *watched, QEvent *event)
{
	/*
	 * Warm the list the first time the field is focused.
	 *
	 * ↓ opens the popup on whatever the model already holds, so without this the browse
	 * gesture the tooltip advertises does nothing at all until something has been typed. Doing
	 * it on focus rather than in the constructor keeps it off the OBS startup path: the manager
	 * dock is built every launch, and most launches never touch this field.
	 */
	if (watched == m_edit && event->type() == QEvent::FocusIn && !m_primed) {
		m_primed = true;
		m_popupOnResult = false;
		runSearch();
	}
	return QWidget::eventFilter(watched, event);
}

QString XiveCategoryPicker::categoryName() const
{
	return m_edit->text().trimmed();
}

bool XiveCategoryPicker::isBusy() const
{
	return m_edit->hasFocus() || (m_completer->popup() && m_completer->popup()->isVisible());
}

void XiveCategoryPicker::setCategory(const QString &name, const QString &slug)
{
	if (name == m_edit->text() && slug == m_slug)
		return;

	m_edit->setText(name);
	m_slug = slug;
	updateHint();
}

void XiveCategoryPicker::scheduleSearch()
{
	m_popupOnResult = true;
	m_debounce->start();
}

void XiveCategoryPicker::runSearch()
{
	XiveApi *api = XiveApi::instance();
	if (!api)
		return;

	const int seq = ++m_requestSeq;

	/* The reply outlives this call and is kept alive by XiveApi, not by us — a dock torn down
	 * mid-request (OBS shutdown is the realistic one) would leave the callback holding a
	 * dangling `this`. QPointer nulls itself when the widget goes. */
	QPointer<XiveCategoryPicker> self(this);

	api->categories(m_edit->text(), kResultLimit,
			[this, self, seq](bool ok, const QJsonObject &body, int,
					  const QString &error) {
				if (!self)
					return;
				if (seq != m_requestSeq)
					return; /* a newer keystroke already answered */
				if (!ok) {
					/* Not surfaced: the field still works as free text without
					 * the directory, and an error line under a search box that
					 * the next keystroke retries anyway is just noise. */
					xive_debug("category search failed: %s",
						   qUtf8Printable(error));
					return;
				}
				applyResults(body.value(QStringLiteral("categories")).toArray());
			});
}

void XiveCategoryPicker::applyResults(const QJsonArray &rows)
{
	m_model->clear();

	for (const QJsonValue &value : rows) {
		const QJsonObject row = value.toObject();
		const QString name = row.value(QStringLiteral("name")).toString();
		const QString slug = row.value(QStringLiteral("slug")).toString();
		if (name.isEmpty() || slug.isEmpty())
			continue;

		auto *item = new QStandardItem(name);
		item->setData(slug, kSlugRole);
		item->setEditable(false);

		/*
		 * Release year distinguishes the reissues the catalogue is full of — there are four
		 * rows called "Resident Evil 4" — and the live count is the same signal the web
		 * picker shows. Both go in the tooltip rather than the label, because the label text
		 * is what gets written into the field when the row is picked.
		 */
		QStringList detail;
		const int year = row.value(QStringLiteral("releaseYear")).toInt();
		if (year > 0)
			detail << QString::number(year);
		const int live = row.value(QStringLiteral("liveStreamCount")).toInt();
		if (live > 0)
			detail << tr("%n live", "", live);
		if (!detail.isEmpty())
			item->setToolTip(QStringLiteral("%1 — %2").arg(name, detail.join(
									       QStringLiteral(" · "))));

		m_model->appendRow(item);
	}

	/* Re-open against the refreshed list. Without this the popup keeps showing the results for
	 * the previous keystroke until the next one arrives. Skipped for the warm-up fetch, which
	 * must not put a list on screen the creator did not ask for. */
	if (m_popupOnResult && m_model->rowCount() > 0 && m_edit->hasFocus())
		m_completer->complete();
}

void XiveCategoryPicker::updateHint()
{
	const QString text = m_edit->text().trimmed();

	/* Which of the three states the field is in is not guessable from the text alone — "Rust"
	 * looks identical whether it resolved to the game or is a word someone typed — and the
	 * difference decides whether the broadcast is listed in the directory at all. */
	if (!m_slug.isEmpty()) {
		m_hint->setStyleSheet(XiveTheme::noticeCss(false));
		m_hint->setText(tr("In the Xive directory — your stream is listed under it."));
		return;
	}
	if (text.isEmpty()) {
		m_hint->setStyleSheet(XiveTheme::noticeCss(false));
		m_hint->setText(tr("Defaults to “Live”."));
		return;
	}
	/* Only free text is capped; a picked row is already past this branch. Warned here as it is
	 * typed rather than only at save, where it would be a round trip and a red banner. */
	if (text.length() > 64) {
		m_hint->setStyleSheet(XiveTheme::noticeCss(true));
		m_hint->setText(tr("Too long — a typed category must be 64 characters or fewer. "
				   "Pick one from the list instead."));
		return;
	}
	m_hint->setStyleSheet(XiveTheme::noticeCss(false));
	m_hint->setText(tr("Not in the directory. Saved as typed; pick from the list to be "
			   "listed under a category."));
}
