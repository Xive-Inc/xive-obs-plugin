#pragma once

#include <QString>
#include <QWidget>

class QCompleter;
class QEvent;
class QJsonArray;
class QLabel;
class QLineEdit;
class QStandardItemModel;
class QTimer;

/*
 * Category field backed by the Xive directory (public.stream_categories).
 *
 * Replaces the free-text box the manager dock shipped with. The catalogue is ~16,000 rows — IGDB
 * games plus hand-curated categories — so it cannot be a combo box at any size; this is a search
 * field over `GET /api/streaming/categories`, which is the same thing StudioCategoryPicker.tsx is
 * on the web.
 *
 * It emits a NAME and a SLUG. The slug is what lets the server resolve `category_id` exactly
 * instead of guessing from the display name, and is the entire reason this widget exists.
 *
 * Free text is still accepted, deliberately. A creator who never opens the popup, or who is
 * playing something the catalogue has never heard of, keeps what they typed: the slug stays empty
 * and the server falls back to matching the name, exactly as every client did before the
 * catalogue existed. Typing after a pick clears the slug, because the text no longer describes
 * the row that was chosen.
 */
class XiveCategoryPicker : public QWidget {
	Q_OBJECT

public:
	explicit XiveCategoryPicker(QWidget *parent = nullptr);

	/** What to send as `category`. Trimmed. */
	QString categoryName() const;
	/** What to send as `categorySlug`, or empty when the text is unmatched free text. */
	QString categorySlug() const { return m_slug; }

	/**
	 * Seed from the server without counting as an edit.
	 *
	 * A no-op when nothing actually changed, so the polling dock cannot move the cursor out
	 * from under someone typing by writing the same value back every tick.
	 */
	void setCategory(const QString &name, const QString &slug);

	/** True while the creator is interacting, so a poll knows to leave the field alone. */
	bool isBusy() const;

signals:
	/** The creator changed the field — typed in it, or picked a row. */
	void edited();

protected:
	/* Watches the line edit for its first focus, to warm the browse list. See the .cpp. */
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void scheduleSearch();
	void runSearch();
	void applyResults(const QJsonArray &rows);
	void updateHint();

	QLineEdit *m_edit = nullptr;
	QLabel *m_hint = nullptr;
	QCompleter *m_completer = nullptr;
	QStandardItemModel *m_model = nullptr;
	QTimer *m_debounce = nullptr;

	/** Slug of the row last picked from the popup. Empty means free text. */
	QString m_slug;

	/** Whether the browse list has been fetched. See the focus handling in the .cpp. */
	bool m_primed = false;

	/*
	 * Whether the in-flight search should open the popup when it lands. True for a keystroke,
	 * false for the warm-up on first focus — a list that appears merely because the field was
	 * clicked into is a popup nobody asked for.
	 */
	bool m_popupOnResult = false;

	/*
	 * Guards the autocomplete race: a slow early request landing after a fast later one would
	 * otherwise replace the list with results for a prefix the creator has already typed past.
	 */
	int m_requestSeq = 0;
};
