#pragma once

#include <QWidget>

class QLabel;
class QTextBrowser;
class QTimer;

/*
 * Gifts and wallet. The leaderboard is scoped to the CURRENT broadcast server-side (via
 * BroadcastWindow::startedAt), which matters here: an OBS channel row is permanent per creator,
 * so an unscoped read would show every gift they have ever received as if it arrived tonight.
 */
class XiveAlertsDock : public QWidget {
	Q_OBJECT

public:
	static const char *kDockId;

	explicit XiveAlertsDock(QWidget *parent = nullptr);

private slots:
	void poll();

private:
	void setNotice(const QString &text);

	/* Header status pair — the popout's live dot and LIVE/OFFLINE word. */
	QLabel *m_headerDot = nullptr;
	QLabel *m_headerState = nullptr;
	QLabel *m_walletLabel = nullptr;
	QLabel *m_totalLabel = nullptr;
	QLabel *m_notice = nullptr;
	QTextBrowser *m_view = nullptr;
	QTimer *m_timer = nullptr;

	bool m_inFlight = false;
};
