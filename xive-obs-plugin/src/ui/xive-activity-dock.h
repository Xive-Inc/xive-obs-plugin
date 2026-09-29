#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QTextBrowser;
class QTimer;

/*
 * The studio activity feed — gifts, subs, super chats, follows, markers — classified from the
 * shared chat stream. See xive-activity.h for why there is no endpoint behind this.
 */
class XiveActivityDock : public QWidget {
	Q_OBJECT

public:
	static const char *kDockId;

	explicit XiveActivityDock(QWidget *parent = nullptr);

protected:
	/* Polling is reference-counted on the shared feed, so a hidden dock costs no requests. */
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	void rebuild();

	/* Header status pair — the popout's live dot and LIVE/OFFLINE word. */
	QLabel *m_headerDot = nullptr;
	QLabel *m_headerState = nullptr;
	QComboBox *m_filter = nullptr;
	QLabel *m_notice = nullptr;
	QTextBrowser *m_view = nullptr;
	QTimer *m_relativeTimeTimer = nullptr;
	bool m_counted = false;
};
