#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

class QTimer;

/*
 * The creator's channel as the docks see it: one shared, polled view of "am I live, what am I
 * called, how many people are watching".
 *
 * It exists so the four docks do not each poll the API on their own timer. A broadcaster leaves
 * OBS open for hours; four independent pollers would be four times the request volume for one
 * creator's worth of information.
 *
 * Live state comes from /api/streaming/my-active, which is the cheap route and returns id, title,
 * viewer_count and status in one call. Category is NOT in that response, so it is seeded once from
 * the stream detail route when the id first becomes known and thereafter only changed by our own
 * writes — a category edited on the website while OBS is open will not be reflected until restart.
 * The seed carries BOTH the name and the catalogue slug, so a category picked in a previous session
 * is still a picked category on the next launch rather than something that has to be found again.
 */
class XiveChannel : public QObject {
	Q_OBJECT

public:
	explicit XiveChannel(QObject *parent = nullptr);

	static XiveChannel *instance();
	static void createInstance();
	static void destroyInstance();

	QString streamId() const { return m_streamId; }
	bool isLive() const { return m_live; }
	QString title() const { return m_title; }
	QString category() const { return m_category; }
	/*
	 * Slug of the catalogue row the category resolved to, or empty.
	 *
	 * Seeded from the stream detail route (`category_slug`), which resolves it through
	 * `category_id` rather than through the deprecated free-text column — so a channel already
	 * filed under a category arrives with one, and the picker can say so instead of warning
	 * that a category the creator picked last night is not in the directory.
	 *
	 * Empty means genuinely unfiled: a channel that has never had a category resolved, or one
	 * whose text never matched the catalogue. Saving without one is still fine — the server
	 * falls back to matching the display name.
	 */
	QString categorySlug() const { return m_categorySlug; }
	int viewers() const { return m_viewers; }

	/*
	 * The creator's Xive handle, without a leading "@".
	 *
	 * Seeded from the stream detail route's `profiles` block, NOT from the OIDC userinfo the
	 * account label comes from: Authentik's `preferred_username` is an identity at the IdP and is
	 * under no obligation to equal the handle `/channel/…` resolves. Building a public link out
	 * of the wrong one hands the creator a link to a stranger's channel or to nothing at all.
	 */
	QString username() const { return m_username; }

	/*
	 * The creator's Xive display name — what they are called ON XIVE, which is not what the
	 * identity provider calls them.
	 *
	 * The account label the sign-in produces comes from Authentik's OIDC `name`/`email` claims:
	 * a real name and a work address, neither of which a creator recognises as themselves. The
	 * display name is the one on their channel page, so it is what the dock says "Signed in as".
	 *
	 * Read from `profiles`, never copied into a stream row — see the channels route, where the
	 * profile is the leading table for exactly this reason. Empty until seeded; the dock falls
	 * back to the handle and then to the account label rather than showing nothing.
	 */
	QString displayName() const { return m_displayName; }

	/*
	 * The creator's public channel link — the one they paste into Discord, X, or wherever else
	 * they tell people they are live. Empty until the handle is known.
	 *
	 * `/channel/<handle>` and not `/stream/<id>`: the channel URL is permanent and always points
	 * at whatever the creator is broadcasting now, while a stream id names one broadcast and goes
	 * stale the moment it ends.
	 */
	QString channelUrl() const;

	void start();
	void stop();
	void pollNow();

	/* Called by the main dock once a credential fetch has told us which channel is ours. */
	void setStreamId(const QString &id);

	/*
	 * Drop everything that belongs to the creator who just signed out. Called from the sign-out
	 * path only — stop() is not the same thing, since it also runs on plugin unload.
	 *
	 * ⚠️ THE STREAM ID GOES TOO, not just the handle. Keeping it meant the next creator's first
	 * poll found no active stream of their own, fell through to the detail seed, and seeded from
	 * the PREVIOUS creator's broadcast — which carries that creator's handle and put their
	 * channel link back on screen under the new account. See XiveConfig::clearSession().
	 */
	void forgetUser();

	/*
	 * Live viewer count off the realtime stream (wired to XiveRealtime in plugin-main).
	 *
	 * viewer_presence.php publishes the concurrent count on the stream channel the instant it
	 * changes, so the header can move without waiting for the next my-active poll. The poll stays
	 * the source of truth and the only path when no chat viewer has opened the socket.
	 */
	void applyRealtimeViewers(int count);

	/*
	 * Live/offline/title off the realtime stream (wired to XiveRealtime in plugin-main).
	 *
	 * helpers/stream_lifecycle.php publishes this the moment the row changes, which is what
	 * lets the my-active poll below drop from 10s to a 60s backstop. The poll is still the
	 * source of truth and still the only path when the socket is down — a dropped publication
	 * must not leave the dock believing a live creator is offline for the whole broadcast.
	 *
	 * ⚠️ IT DOES NOT TOUCH m_viewers. The payload carries no `viewer_count` on purpose (deriving
	 * it costs a presence query on a write path), and the count already has its own publication
	 * via applyRealtimeViewers(). Reading an absent field here would zero a live audience.
	 */
	void applyRealtimeStreamState(const QJsonObject &stream, bool live);

	using WriteCallback = std::function<void(bool ok, const QString &error)>;

	void saveMetadata(const QString &title, const QString &category, const QString &categorySlug,
			  WriteCallback cb);

signals:
	/* One coarse signal rather than a dozen fine-grained ones: every dock redraws a handful of
	 * labels, and there is no redraw cost worth optimising against here. */
	void changed();
	void failed(const QString &message);

private:
	void seedFromDetail();

	/*
	 * Who the signed-in creator is, independent of whether they have ever streamed.
	 *
	 * seedFromDetail() can only answer that when a broadcast exists to read it off, so a creator
	 * who has just signed in and never gone live would have no name and no share link. The
	 * channels route resolves the creator from the token alone and hands back both.
	 */
	void seedIdentity();

	QTimer *m_timer = nullptr;
	QString m_streamId;
	QString m_title;
	QString m_category;
	QString m_categorySlug;
	QString m_username;
	QString m_displayName;
	bool m_live = false;
	int m_viewers = 0;
	bool m_seeded = false;
	bool m_identitySeeded = false;
	/* Whether this object holds one XiveRealtime::start() reference. See start(). */
	bool m_realtimeHeld = false;
};
