#pragma once

#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <functional>

class QVBoxLayout;

class QLineEdit;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QLabel;
class QFrame;

/*
 * Live chat for the current broadcast, drawn the way the website draws it.
 *
 * Reads from the shared XiveChatFeed rather than polling itself — the activity dock is a view of the
 * same messages, and two pollers would double the request rate and let the docks disagree.
 *
 * ── The message list is a QTextBrowser, and that decides the rest ────────────────────────────
 *
 * A browser lays a paragraph out once and cannot revise it, so the dock appends and never edits.
 * That is free for ordinary chat, and it is why a poll VOTE — which changes the tally inside a
 * card already on screen — is the one thing that forces the whole document to be redrawn from the
 * feed's buffer. Polls are rare and votes only arrive while one is open, so normal traffic never
 * pays for it. See renderAll().
 */
class XiveChatDock : public QWidget {
	Q_OBJECT

public:
	static const char *kDockId;

	explicit XiveChatDock(QWidget *parent = nullptr);

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private slots:
	void sendMessage();
	/*
	 * Leave edit mode without sending. Bound to Escape and to the Cancel button.
	 *
	 * Restores the composer rather than clearing it: the draft may be several sentences the
	 * creator has just retyped, and Escape is one key away from the keys they were using.
	 */
	void cancelEdit();
	/* A click on a display name — the anchor XiveChatRender puts round it. */
	void onAnchorClicked(const QUrl &url);
	/* Right-click anywhere in the feed: same menu, found by hit-testing for that anchor. */
	void onFeedContextMenu(const QPoint &pos);

private:
	void appendMessages(const QList<QJsonObject> &messages);

	/*
	 * The moderation menu for one message.
	 *
	 * The web reveals a "⋮" button on hover; a QTextBrowser cannot host a widget inside its
	 * text, so the name itself is the target. Everything the menu needs is read back out of the
	 * feed's buffer by message id.
	 */
	void showModerationMenu(const QString &messageId, const QPoint &globalPos);

	/*
	 * Load one of your own messages into the composer for editing.
	 *
	 * ⚠️ EDITING IS DELETE-THEN-RESEND — there is no edit endpoint anywhere on the platform (see
	 * XiveChatFeed::editMessage). So the edited line gets a new id and reappears at the BOTTOM of
	 * chat rather than changing in place, and the composer says so before anything is sent: a
	 * broadcaster who expects an in-place correction and gets a new message at the end of a busy
	 * chat would reasonably think it had failed.
	 */
	void beginEdit(const QJsonObject &message);
	/* Composer chrome for both states — placeholder, button label, the edit banner. */
	void updateComposerMode();

	/* Run one action, reporting the outcome on the notice line. `done` is what it did, phrased
	 * for a person: "Muted Mia for 5 minutes". */
	void runModeration(const QString &done,
			   const std::function<void(std::function<void(bool, const QString &)>)> &call);
	/*
	 * The signed-in creator's handle, for the mention highlight — XiveChannel::username(),
	 * or empty before the channel has been seeded.
	 *
	 * Read fresh on every render rather than cached: the handle arrives asynchronously from
	 * the stream detail seed, so a dock that cached it at construction would highlight
	 * nothing for the whole session on a cold start, silently.
	 */
	QString selfHandle() const;

	/* Redraw every retained message. Used on reset, and whenever a vote lands. */
	void renderAll();
	void showEmptyState();
	void setNotice(const QString &text);
	void updateHeader();

	/*
	 * Redraw the pinned banner above the feed, or hide it when nothing is pinned.
	 *
	 * 🔴 The message body is set as PLAIN TEXT, never rich text. Chat content is arbitrary viewer
	 * input, and a QLabel defaults to auto-detecting HTML — so "<b>hi" from a viewer would render
	 * as markup in the broadcaster's dock, and an <img> would fetch a URL of their choosing.
	 * The label around it is styled instead, which needs no markup in the content at all.
	 */
	void updatePinBanner(const QJsonObject &pin);

	/* Build the two surge frames and insert them at the top of the feed column. Both start
	 * hidden; updateSurgeBanner() decides which, if either, is shown. */
	void buildSurgeBanner(QVBoxLayout *layout);

	/*
	 * Redraw the Xive Surge banner above the feed, or hide it when the room is quiet.
	 *
	 * Reads XiveSurgeFeed rather than taking a parameter: the feed emits one coarse `changed()`
	 * for a new state AND for each tick of the countdown, and both end here.
	 *
	 * TWO VISIBLE STATES, not one. An ACTIVE surge gets the full banner — tier, level, the window
	 * meter, the three numbers and the contributor belt. A room that has started giving but has
	 * not got there yet gets a single quiet line ("Support is building — 7 of 12 to start a XIVE
	 * SURGE"), which is what makes the mechanic discoverable instead of something that appears
	 * from nowhere; the studio's own card does exactly this. Nothing at all when nobody has
	 * given: this sits directly over the message list in a panel that is often 300px tall, and a
	 * permanent strip announcing that nothing is happening would cost two lines of chat forever.
	 */
	void updateSurgeBanner();

	/* Header status pair — the popout's live dot and LIVE/OFFLINE word — plus "N watching". */
	QLabel *m_headerDot = nullptr;
	QLabel *m_headerState = nullptr;
	QLabel *m_headerMeta = nullptr;
	QTextBrowser *m_view = nullptr;
	QLineEdit *m_input = nullptr;
	QPushButton *m_sendButton = nullptr;
	QLabel *m_notice = nullptr;

	/* Pinned-message banner. The frame is what gets shown/hidden, so the layout collapses
	 * completely when nothing is pinned rather than leaving a gap above the feed. */
	QFrame *m_pinFrame = nullptr;
	QLabel *m_pinTitle = nullptr;
	QLabel *m_pinBody = nullptr;
	QPushButton *m_pinUnpin = nullptr;

	/* Xive Surge banner. Like the pin frame, the FRAME is what gets shown and hidden, so the
	 * layout collapses to nothing rather than leaving a gap over the feed. */
	QFrame *m_surgeFrame = nullptr;
	QLabel *m_surgeTier = nullptr;
	QLabel *m_surgeLevel = nullptr;
	QLabel *m_surgeClock = nullptr;
	QProgressBar *m_surgeMeter = nullptr;
	QLabel *m_surgeStats = nullptr;
	QLabel *m_surgeContributors = nullptr;
	/* The building-up line, shown instead of the banner before a surge starts. */
	QFrame *m_surgeIdleFrame = nullptr;
	QLabel *m_surgeIdleLabel = nullptr;

	/* Edit-mode chrome, above the composer: which message is being edited, and Cancel. */
	QFrame *m_editFrame = nullptr;
	QLabel *m_editLabel = nullptr;
	QPushButton *m_editCancel = nullptr;
	/*
	 * The message being edited, or empty when composing normally.
	 *
	 * ⚠️ IT CAN GO STALE. A moderator — or the creator on another device — may delete this
	 * message while it sits here, and the recall half of the edit then fails. That is the safe
	 * direction: nothing is deleted, the draft is untouched, and the notice says why.
	 */
	QString m_editingId;

	/* True while the document holds only the "say hello" placeholder, which the first real
	 * message has to clear. */
	bool m_empty = false;
	bool m_counted = false;
};
