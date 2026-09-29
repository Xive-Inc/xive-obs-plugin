#include "ui/xive-theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>

namespace XiveTheme {

QString styleSheet()
{
	/*
	 * One template, filled from the palette above, so a colour is defined once. `%1`-style
	 * arguments would run out at nine and read as noise at this length; the tokens are spelled
	 * out and substituted below.
	 */
	static const QString tmpl = QStringLiteral(R"(
QWidget#xiveDock {
	background: @bg;
	color: @ink;
	font-size: 12px;
}

QWidget#xiveDock QLabel {
	color: @ink;
	background: transparent;
}

/* Header ---------------------------------------------------------------- */

QWidget#xiveDockHeader {
	border-bottom: 1px solid @border;
}

QLabel#xiveDockTitle {
	color: @inkSoft;
	font-size: 11px;
	font-weight: 700;
}

QLabel#xiveDockState {
	color: @inkMuted;
	font-size: 9px;
	font-weight: 700;
}

QLabel#xiveDockMeta {
	color: @inkFaint;
	font-size: 10px;
}

/* The status dot is a fixed-size label; the radius is half its side, so it renders as a circle. */
QLabel#xiveDockDot {
	border-radius: 4px;
	background: rgba(161, 161, 170, 0.55);
}

QLabel#xiveDockDot[live="true"] {
	background: @live;
}

/* Cards ------------------------------------------------------------------ */

QFrame#xiveCard {
	border: 1px solid @border;
	border-radius: 10px;
	background: @surface;
}

QLabel#xiveSection {
	color: @inkMuted;
	font-size: 10px;
	font-weight: 700;
}

/* Inputs ----------------------------------------------------------------- */

QWidget#xiveDock QLineEdit,
QWidget#xiveDock QPlainTextEdit,
QWidget#xiveDock QComboBox {
	border: 1px solid rgba(255, 255, 255, 0.10);
	border-radius: 10px;
	background: rgba(10, 10, 15, 0.65);
	padding: 6px 10px;
	color: @ink;
	selection-background-color: rgba(162, 75, 255, 0.45);
}

QWidget#xiveDock QLineEdit:focus,
QWidget#xiveDock QPlainTextEdit:focus,
QWidget#xiveDock QComboBox:focus {
	border-color: @focus;
}

QWidget#xiveDock QLineEdit:disabled,
QWidget#xiveDock QPlainTextEdit:disabled,
QWidget#xiveDock QComboBox:disabled {
	color: @inkFaint;
	background: rgba(10, 10, 15, 0.35);
}

QWidget#xiveDock QComboBox::drop-down {
	border: none;
	width: 18px;
}

QWidget#xiveDock QComboBox QAbstractItemView {
	border: 1px solid @border;
	background: #12121b;
	color: @ink;
	selection-background-color: rgba(162, 75, 255, 0.35);
}

/* Buttons ---------------------------------------------------------------- */

QWidget#xiveDock QPushButton {
	border: 1px solid rgba(255, 255, 255, 0.12);
	border-radius: 10px;
	background: rgba(255, 255, 255, 0.06);
	padding: 7px 12px;
	color: @ink;
	font-weight: 500;
}

QWidget#xiveDock QPushButton:hover:enabled {
	border-color: @borderStrong;
	background: rgba(255, 255, 255, 0.10);
}

QWidget#xiveDock QPushButton:disabled {
	color: @inkFaint;
	background: rgba(255, 255, 255, 0.03);
	border-color: rgba(255, 255, 255, 0.06);
}

/* The one call to action per dock, on the brand gradient. */
QPushButton#xivePrimary {
	border: none;
	border-radius: 10px;
	background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
				    stop:0 #ff2d78, stop:0.5 #a24bff, stop:1 #3d7bff);
	color: #ffffff;
	font-weight: 600;
	padding: 8px 14px;
}

QPushButton#xivePrimary:disabled {
	color: rgba(255, 255, 255, 0.55);
	background: rgba(255, 255, 255, 0.06);
}

/* Feeds ------------------------------------------------------------------ */

QWidget#xiveDock QTextBrowser {
	border: 1px solid @border;
	border-radius: 10px;
	background: @surface;
	color: @ink;
	padding: 4px;
}

QWidget#xiveDock QCheckBox {
	color: @ink;
	spacing: 7px;
}

QWidget#xiveDock QCheckBox:disabled {
	color: @inkFaint;
}

QWidget#xiveDock QCheckBox::indicator {
	width: 14px;
	height: 14px;
	border: 1px solid rgba(255, 255, 255, 0.20);
	border-radius: 4px;
	background: rgba(10, 10, 15, 0.65);
}

QWidget#xiveDock QCheckBox::indicator:checked {
	border-color: #a24bff;
	background: #a24bff;
}

/* Scrollbars: thin and unobtrusive, like the popout's overflow areas. */
QWidget#xiveDock QScrollBar:vertical {
	background: transparent;
	width: 8px;
	margin: 0;
}

QWidget#xiveDock QScrollBar::handle:vertical {
	background: rgba(255, 255, 255, 0.14);
	border-radius: 4px;
	min-height: 24px;
}

QWidget#xiveDock QScrollBar::handle:vertical:hover {
	background: rgba(255, 255, 255, 0.22);
}

QWidget#xiveDock QScrollBar::add-line:vertical,
QWidget#xiveDock QScrollBar::sub-line:vertical,
QWidget#xiveDock QScrollBar::add-page:vertical,
QWidget#xiveDock QScrollBar::sub-page:vertical {
	background: transparent;
	height: 0;
}
)");

	static const QString sheet = QString(tmpl)
					     .replace(QStringLiteral("@bg"), QString::fromUtf8(kBg))
					     .replace(QStringLiteral("@surface"),
						      QString::fromUtf8(kSurface))
					     .replace(QStringLiteral("@borderStrong"),
						      QString::fromUtf8(kBorderStrong))
					     .replace(QStringLiteral("@border"),
						      QString::fromUtf8(kBorder))
					     .replace(QStringLiteral("@inkSoft"),
						      QString::fromUtf8(kInkSoft))
					     .replace(QStringLiteral("@inkMuted"),
						      QString::fromUtf8(kInkMuted))
					     .replace(QStringLiteral("@inkFaint"),
						      QString::fromUtf8(kInkFaint))
					     .replace(QStringLiteral("@ink"), QString::fromUtf8(kInk))
					     .replace(QStringLiteral("@focus"),
						      QString::fromUtf8(kFocus))
					     .replace(QStringLiteral("@live"),
						      QString::fromUtf8(kLive));
	return sheet;
}

void applyDockTheme(QWidget *dock)
{
	if (!dock)
		return;

	dock->setObjectName(QStringLiteral("xiveDock"));
	/* Without this, a stylesheet background on a plain QWidget is not painted at all. */
	dock->setAttribute(Qt::WA_StyledBackground, true);
	dock->setStyleSheet(styleSheet());
}

QWidget *makeHeader(QWidget *parent, const QString &title, QLabel **dotOut, QLabel **stateOut,
		    QLabel **metaOut)
{
	auto *header = new QWidget(parent);
	header->setObjectName(QStringLiteral("xiveDockHeader"));
	header->setAttribute(Qt::WA_StyledBackground, true);

	auto *row = new QHBoxLayout(header);
	row->setContentsMargins(10, 6, 10, 6);
	row->setSpacing(6);

	auto *titleLabel = new QLabel(title.toUpper(), header);
	titleLabel->setObjectName(QStringLiteral("xiveDockTitle"));
	row->addWidget(titleLabel);

	row->addStretch(1);

	if (metaOut) {
		auto *meta = new QLabel(header);
		meta->setObjectName(QStringLiteral("xiveDockMeta"));
		meta->setVisible(false);
		row->addWidget(meta);
		*metaOut = meta;
	}

	auto *dot = new QLabel(header);
	dot->setObjectName(QStringLiteral("xiveDockDot"));
	dot->setFixedSize(8, 8);
	row->addWidget(dot);

	auto *state = new QLabel(header);
	state->setObjectName(QStringLiteral("xiveDockState"));
	row->addWidget(state);

	if (dotOut)
		*dotOut = dot;
	if (stateOut)
		*stateOut = state;

	setLiveState(dot, state, false);
	return header;
}

void setLiveState(QLabel *dot, QLabel *state, bool live)
{
	if (dot) {
		/*
		 * The colour lives in a QSS attribute selector, and Qt does not re-evaluate those on
		 * its own — the style has to be explicitly unset and re-set for the change to paint.
		 */
		dot->setProperty("live", live ? QStringLiteral("true")
					     : QStringLiteral("false"));
		dot->style()->unpolish(dot);
		dot->style()->polish(dot);
	}
	if (state)
		state->setText(live ? QObject::tr("LIVE") : QObject::tr("OFFLINE"));
}

QFrame *makeCard(QWidget *parent)
{
	auto *card = new QFrame(parent);
	card->setObjectName(QStringLiteral("xiveCard"));
	card->setFrameShape(QFrame::NoFrame);
	return card;
}

QString noticeCss(bool isError)
{
	return isError ? QStringLiteral("color:%1;").arg(QString::fromUtf8(kDanger))
		       : QStringLiteral("color:%1;").arg(QString::fromUtf8(kInkMuted));
}

QString menuStyleSheet()
{
	return QStringLiteral(
		       "QMenu { background: #12121b; color: %1; border: 1px solid %2; padding: 4px; }"
		       "QMenu::item { padding: 5px 18px 5px 12px; border-radius: 6px; }"
		       "QMenu::item:selected { background: rgba(162, 75, 255, 0.28); }"
		       "QMenu::item:disabled { color: %3; }"
		       "QMenu::separator { height: 1px; background: %2; margin: 4px 6px; }")
		.arg(QString::fromUtf8(kInk), QString::fromUtf8(kBorderStrong),
		     QString::fromUtf8(kInkFaint));
}

QString popupListStyleSheet()
{
	/* Matches the QComboBox popup rule in styleSheet() — same fill, same border, same purple
	 * selection — so the two kinds of dropdown in the docks do not read as different widgets. */
	return QStringLiteral(
		       "QAbstractItemView { background: #12121b; color: %1; border: 1px solid %2;"
		       " padding: 3px; outline: none;"
		       " selection-background-color: rgba(162, 75, 255, 0.35); }"
		       "QAbstractItemView::item { padding: 5px 8px; border-radius: 6px; }"
		       "QAbstractItemView::item:hover { background: rgba(255, 255, 255, 0.06); }"
		       "QAbstractItemView::item:selected { background: rgba(162, 75, 255, 0.35);"
		       " color: %1; }")
		.arg(QString::fromUtf8(kInk), QString::fromUtf8(kBorderStrong));
}

QLabel *makeSectionLabel(QWidget *parent, const QString &text)
{
	auto *label = new QLabel(text.toUpper(), parent);
	label->setObjectName(QStringLiteral("xiveSection"));
	return label;
}

QString documentStyleSheet()
{
	return QStringLiteral("body { color: %1; font-size: 12px; }"
			      "a { color: #a24bff; }")
		.arg(QString::fromUtf8(kInk));
}

QString chatDocumentStyleSheet()
{
	/*
	 * Qt's rich text understands `.class` selectors in a document stylesheet, so the roles and
	 * the type ramp live here and the per-message HTML stays readable. Anything that changes
	 * per message — a name's colour, a badge's palette — is still written inline, because a
	 * stylesheet cannot carry a value that varies by row.
	 *
	 * Colours are the Tailwind tokens the web uses for the same elements: amber-400 Host,
	 * emerald-400 Mod, purple-400 Priority, --color-stream-ink for the body.
	 * `.creator` keeps its name as the styling hook for the Host role; only the word changed.
	 */
	return documentStyleSheet() +
	       /*
		* 14px, overriding the 12px the other feeds use. The popout draws a chat row at
		* `text-sm` (StreamChatPanel.tsx, the `embeddedInRail` branch — note NOT the
		* `premiumRail` one, which needs `mobilePremium` and CreatorStudioChat does not pass
		* it), and the type ramp below hangs off that base.
		*/
	       QStringLiteral("body { font-size: 14px; }"
			      /* The popout's card timestamps are text-[11px]; its message rows have
			       * no timestamp at all, so this is the nearest reference it gives. */
			      ".ts { color: #5b5b6b; font-size: 11px; }"
			      ".nm { font-weight: 600; }"
			      /*
			       * #CBD5E1, not the ink token: the row body is `dark:text-gray-300`,
			       * and theme.css redefines --color-gray-300 to that value. Reading it
			       * as stream-ink made the dock's message text brighter than the
			       * popout's, which is the difference you notice with both open.
			       */
			      ".body { color: #CBD5E1; }"
			      ".muted { color: #8a8a99; }"
			      ".faint { color: #5b5b6b; }"
			      ".creator { color: #ffb900; font-weight: 600; }"
			      ".mod { color: #00d492; font-weight: 600; }"
			      ".prio { color: #c27aff; font-weight: 600; }"
			      ".chip { font-weight: 700; }"
			      /* An icon badge: a bare glyph, no background run. */
			      ".badge { font-weight: 700; }"
			      ".gift { color: #c27aff; font-weight: 600; }"
			      ".empty { color: #8a8a99; }");
}

} // namespace XiveTheme
