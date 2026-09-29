#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class XiveCategoryPicker;

/*
 * Stream manager: what the broadcast is called, how many people are watching, and the creator's own
 * public link to hand out.
 *
 * ⚠️ THE QUICK ACTIONS ARE CONDITIONAL. When obs-browser can give us panels, `/popout/actions` is
 * registered as its own dock and draws the web's full set — poll, ad break, raid, clip, giveaway,
 * goals, battle invite, co-host invite. This dock's own card only ever offered the two with a chat
 * protocol behind them (see xive-quick-actions.h), so it would be a strictly worse duplicate
 * sitting beside the real thing. It is built only when there is no browser panel to defer to.
 *
 * The details form and the share link stay native either way: `/popout/actions` deliberately
 * refuses `onEditDetails` — that panel carries the ingest credentials, which do not belong in a
 * dock parked over a live scene — so there is no web surface to hand this off to.
 */
class XiveManagerDock : public QWidget {
	Q_OBJECT

public:
	static const char *kDockId;

	/**
	 * `withQuickActions` false when a `/popout/actions` browser dock is carrying them instead.
	 * See the note above the class.
	 */
	explicit XiveManagerDock(bool withQuickActions, QWidget *parent = nullptr);

private slots:
	void saveMetadata();
	void announceAdBreak();
	void managePoll();
	void copyChannelLink();

private:
	void refreshUi();
	void setStatus(const QString &text, bool isError);

	/**
	 * Read the creator's stored go-live notification into the two widgets below.
	 *
	 * Seeded once, and only into fields the creator has not touched — the same rule the web's
	 * `goLiveSeededRef` follows, and for the same reason: a background answer that lands while
	 * someone is typing their announcement would overwrite it mid-sentence.
	 */
	void loadGoLiveNotification();

	/** The second half of a save. Non-fatal — see the note at the call site. */
	void saveGoLiveNotification();

	QLabel *m_stateLabel = nullptr;
	/* Header status pair — the popout's live dot and LIVE/OFFLINE word. */
	QLabel *m_headerDot = nullptr;
	QLabel *m_headerState = nullptr;
	QLineEdit *m_titleEdit = nullptr;
	XiveCategoryPicker *m_categoryPicker = nullptr;
	QPushButton *m_saveButton = nullptr;
	/* Read-only, but a QLineEdit rather than a QLabel so the URL can be selected and dragged
	 * out by hand — the copy button is the fast path, not the only one. */
	QLineEdit *m_shareEdit = nullptr;
	QPushButton *m_shareCopyButton = nullptr;
	QPushButton *m_shareOpenButton = nullptr;
	QPushButton *m_adBreakButton = nullptr;
	QPushButton *m_pollButton = nullptr;
	QLabel *m_statusLabel = nullptr;

	/*
	 * Go-live notification. Lives on public.stream_channels, NOT on the broadcast — so it is a
	 * channel default that outlives any one stream, and it is saved through a different route
	 * from the title and category above.
	 */
	QCheckBox *m_notifyCheck = nullptr;
	QLineEdit *m_notifyMessage = nullptr;

	/* True while the user is typing, so a background poll cannot overwrite a half-typed
	 * title under the cursor. */
	bool m_titleDirty = false;
	bool m_categoryDirty = false;
	bool m_notifyDirty = false;
};
