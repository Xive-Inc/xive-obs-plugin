#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QList>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QWidget;

/*
 * Who is blocked from this chat, and the one button that lifts it.
 *
 * ── Why this is a dialog and not a menu item ─────────────────────────────────────────────────
 *
 * Every other moderation action in this dock hangs off a right-click on the person's message.
 * Unblocking cannot: `chat/ban` publishes a purge, so the moment somebody is blocked their lines
 * leave the feed. There is nothing left to right-click, which is exactly why the plugin shipped
 * with `chatUnban` wired to the API and reachable from nowhere — and why the block confirmation's
 * promise that you could "unblock them" was not true in this dock.
 *
 * The website has the same constraint and answers it the same way: StudioCommunitySection reads
 * `chat/restrictions` and lists the bans, rather than trying to find them in the chat buffer.
 *
 * ── What it does NOT do ──────────────────────────────────────────────────────────────────────
 *
 * It does not block anybody. Blocking stays on the message menu where the target is unambiguous —
 * a searchable list of every viewer would be a different feature, and picking the wrong row out of
 * one is a mistake this dock should not make easy.
 *
 * It lists BANS only, though `chat/restrictions` also returns moderators and live timeouts. The
 * moderator roster is already visible per-message (the Mod tag), and an active mute expires on its
 * own and can be lifted from the person's message, which is still in the feed — neither has the
 * unreachability problem that justifies this window.
 */
class XiveBlockedDialog : public QDialog {
	Q_OBJECT

public:
	explicit XiveBlockedDialog(QWidget *parent = nullptr);

private:
	void reload();
	void applyRows(const QList<QJsonObject> &bans);
	/* One row: who, when, why, and Unblock. */
	QWidget *makeRow(const QJsonObject &ban);
	void setStatus(const QString &text, bool isError);

	/* Rows live in here so a reload can clear it without touching the surrounding chrome. */
	QVBoxLayout *m_list = nullptr;
	QLabel *m_status = nullptr;
	QPushButton *m_refresh = nullptr;
	bool m_loading = false;
};
