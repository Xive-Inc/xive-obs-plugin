#include "xive-channel.h"
#include "xive-api.h"
#include "xive-config.h"
#include "xive-log.h"
#include "xive-realtime.h"   // repoint the socket the instant a go-live changes the stream id

#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>
#include <QUrl>

namespace {
XiveChannel *g_instance = nullptr;
}

XiveChannel::XiveChannel(QObject *parent) : QObject(parent)
{
	m_streamId = XiveConfig::instance().streamId();
	m_username = XiveConfig::instance().username();
	m_displayName = XiveConfig::instance().displayName();

	m_timer = new QTimer(this);
	m_timer->setInterval(XiveConfig::instance().statusPollMs());
	connect(m_timer, &QTimer::timeout, this, &XiveChannel::pollNow);
}

XiveChannel *XiveChannel::instance()
{
	return g_instance;
}

void XiveChannel::createInstance()
{
	if (!g_instance)
		g_instance = new XiveChannel();
}

void XiveChannel::destroyInstance()
{
	delete g_instance;
	g_instance = nullptr;
}

void XiveChannel::start()
{
	if (!m_timer->isActive())
		m_timer->start();

	/*
	 * ⚠️ THE CHANNEL HOLDS ITS OWN REALTIME REFERENCE. Before 0.1.3 only the native chat, activity
	 * and surge docks started XiveRealtime, and with browser panels (the normal install) those
	 * docks do not exist — so the socket never opened, the go-live push never arrived, and
	 * live/offline came from the 60s my-active backstop alone. Confirmed in the access logs: exactly
	 * 60 plugin requests an hour and no uni_sse connection from panel installs.
	 *
	 * Held once, however many times start() runs (sign-in AND every applied credential fetch
	 * call it), or the refcount would leak and stop() could never close the socket.
	 *
	 * Stream id FIRST, then start: setStreamId() on an inactive client only records the id, so the
	 * one openConnection() inside start() subscribes to `stream:<id>` straight away rather than
	 * opening on the activity channel and immediately tearing that down to reopen.
	 */
	if (!m_realtimeHeld) {
		if (XiveRealtime *rt = XiveRealtime::instance()) {
			if (!m_streamId.isEmpty())
				rt->setStreamId(m_streamId);
			rt->start();
			m_realtimeHeld = true;
		}
	}

	/*
	 * Before the poll, not after it: this is the call that answers "who is signed in", and the
	 * dock has a label waiting on it. The poll answers "are they live", which the header can
	 * show as offline in the meantime without being wrong.
	 */
	seedIdentity();
	pollNow();
}

void XiveChannel::stop()
{
	m_timer->stop();

	/* Released on sign-out and on unload, the two paths that call this. A dock still watching
	 * keeps the socket open through its own reference. */
	if (m_realtimeHeld) {
		m_realtimeHeld = false;
		if (XiveRealtime *rt = XiveRealtime::instance())
			rt->stop();
	}
}

void XiveChannel::setStreamId(const QString &id)
{
	if (id.isEmpty() || id == m_streamId)
		return;

	m_streamId = id;
	m_seeded = false;
	/* A different channel's category is a different category; a slug carried over from the last
	 * one would file this broadcast under whatever the previous one was playing. */
	m_categorySlug.clear();
	XiveConfig::instance().setStreamId(id);

	/* The socket follows the channel. Only the native chat feed's tick used to do this, and with
	 * browser panels that feed never ticks. */
	if (XiveRealtime *rt = XiveRealtime::instance())
		rt->setStreamId(id);

	emit changed();
	seedFromDetail();
}

QString XiveChannel::channelUrl() const
{
	if (m_username.isEmpty())
		return QString();

	/*
	 * No "@" — Brett's call, 2026-08-12. The route strips leading "@" signs before doing
	 * anything with the segment (routing.tsx: `decodeURIComponent(segments[1]).replace(/^@+/,'')`),
	 * so both spellings land on the same page and the bare one is the tidier thing to paste into
	 * a Discord post.
	 *
	 * The handle is stored already stripped of "@" and percent-encoded here rather than pasted
	 * in raw: Xive handles are plain, but a link built by string concatenation should not be the
	 * thing that decides that.
	 */
	return XiveConfig::siteBase() + QStringLiteral("/channel/") +
	       QString::fromUtf8(QUrl::toPercentEncoding(m_username));
}

void XiveChannel::forgetUser()
{
	/*
	 * Everything here describes ONE creator, so it all goes together. Clearing the handle alone
	 * was the original bug: the stream id stayed, the next sign-in's poll found no broadcast of
	 * its own, and seedFromDetail() re-read the departed creator's stream — handing back their
	 * handle and putting their channel link back under the new account's name.
	 */
	m_streamId.clear();
	m_username.clear();
	m_displayName.clear();
	m_title.clear();
	m_category.clear();
	m_categorySlug.clear();
	m_live = false;
	m_viewers = 0;

	/* Both seeds run again for whoever signs in next. */
	m_seeded = false;
	m_identitySeeded = false;

	/*
	 * The socket is subscribed to `stream:<id>` for the broadcast that just stopped being ours.
	 * Repointed at nothing rather than left listening to a stream this OBS no longer has any
	 * business hearing about — setStreamId() reopens on the creator's own activity channel, so
	 * this does not take the connection down. See its comment.
	 */
	if (XiveRealtime *rt = XiveRealtime::instance())
		rt->setStreamId(QString());

	emit changed();
}

void XiveChannel::applyRealtimeViewers(int count)
{
	/* Only meaningful while live; an offline channel shows 0, set by the poll. A negative or
	 * unchanged value is nothing to react to. */
	if (!m_live || count < 0 || count == m_viewers)
		return;
	m_viewers = count;
	emit changed();
}

void XiveChannel::applyRealtimeStreamState(const QJsonObject &stream, bool live)
{
	const QString id = stream.value(QStringLiteral("id")).toString();
	if (id.isEmpty())
		return;

	/*
	 * An event about a stream that is NOT ours to show.
	 *
	 * The channel this arrives on is the creator's, not a broadcast's, so a creator who owns
	 * more than one stream row gets events for all of them. A LIVE one always wins — the server
	 * is authoritative about which broadcast is running, exactly as pollNow() treats my-active.
	 * A non-live one for some other row is simply not our business; reacting to it would take
	 * the dock offline in the middle of a broadcast.
	 */
	if (!live && id != m_streamId)
		return;

	const bool wasLive = m_live;
	const int oldViewers = m_viewers;
	const QString oldTitle = m_title;

	if (id != m_streamId) {
		/* A new broadcast. Same handling as the poll: the cached id is what is wrong, and the
		 * category/handle seed belongs to the old row. */
		m_streamId = id;
		m_seeded = false;
		m_categorySlug.clear();
		XiveConfig::instance().setStreamId(id);

		/*
		 * ⚠️ REPOINT THE SOCKET IMMEDIATELY, do not wait for a feed's next tick.
		 *
		 * XiveRealtime keeps its own copy of the current stream id — it filters activity
		 * pushes against it and builds the `stream:<id>` chat channel from it — and the only
		 * things that used to update it were XiveChatFeed::tick() and
		 * XiveActivityFeed::tick(). Both now run on backstop cadences while the socket is up
		 * (30s and 120s), so a go-live that changes the id — every native/WHIP broadcast,
		 * which mints a fresh row each time — would leave the socket subscribed to the
		 * PREVIOUS broadcast's chat and silently dropping the new one's activity events for
		 * up to half a minute. Instant go-live detection is what made that window visible.
		 *
		 * Safe to call from here because plugin-main connects streamState with
		 * Qt::QueuedConnection: this runs on its own event-loop turn, not inside the SSE
		 * read that delivered the push, so tearing that connection down is not reentrant.
		 */
		if (XiveRealtime *rt = XiveRealtime::instance())
			rt->setStreamId(id);
	}

	m_live = live;

	const QString t = stream.value(QStringLiteral("title")).toString();
	if (!t.isEmpty())
		m_title = t;

	/* Offline means nobody is watching. Mirrors the `else` branch of pollNow(); while live the
	 * count is left alone — see the header. */
	if (!m_live)
		m_viewers = 0;

	if (!m_seeded && !m_streamId.isEmpty())
		seedFromDetail();

	if (wasLive != m_live || oldViewers != m_viewers || oldTitle != m_title)
		emit changed();
}

void XiveChannel::pollNow()
{
	XiveApi *api = XiveApi::instance();
	if (!api)
		return;

	api->myActive([this](bool ok, const QJsonObject &body, int, const QString &error) {
		if (!ok) {
			/* A failed poll is not worth a visible error — the next tick may well
			 * succeed, and an error banner that flickers every 10s is worse than
			 * showing slightly stale numbers. Log it and move on. */
			xive_debug("my-active poll failed: %s", qUtf8Printable(error));
			return;
		}

		const bool wasLive = m_live;
		const int oldViewers = m_viewers;
		const QString oldTitle = m_title;

		const bool hasActive = body.value(QStringLiteral("hasActiveStream")).toBool();
		if (hasActive) {
			const QJsonObject s = body.value(QStringLiteral("stream")).toObject();
			const QString id = s.value(QStringLiteral("id")).toString();
			if (!id.isEmpty() && id != m_streamId) {
				/* The server is authoritative about which channel is live. If it
				 * disagrees with our cached id, the cache is what is wrong. */
				m_streamId = id;
				m_seeded = false;
				m_categorySlug.clear();
				XiveConfig::instance().setStreamId(id);
				/* Repointed here too — see setStreamId(). A no-op when the push already
				 * moved it. */
				if (XiveRealtime *rt = XiveRealtime::instance())
					rt->setStreamId(id);
			}
			m_live = true;
			m_viewers = s.value(QStringLiteral("viewer_count")).toInt();
			const QString t = s.value(QStringLiteral("title")).toString();
			if (!t.isEmpty())
				m_title = t;
		} else {
			m_live = false;
			m_viewers = 0;
		}

		if (!m_seeded && !m_streamId.isEmpty())
			seedFromDetail();

		if (wasLive != m_live || oldViewers != m_viewers || oldTitle != m_title)
			emit changed();
	});
}

void XiveChannel::seedFromDetail()
{
	XiveApi *api = XiveApi::instance();
	if (!api || m_streamId.isEmpty())
		return;

	m_seeded = true; /* set first: a failure should not retry on every poll */

	/*
	 * The id this seed is FOR, checked again when the answer lands.
	 *
	 * A sign-out (or a go-live) can happen while this request is in flight, and the reply would
	 * otherwise write the previous creator's title, category and handle over the state that was
	 * just cleared for the new one — persisting the handle to disk on the way past.
	 */
	const QString seededId = m_streamId;

	api->streamDetail(seededId, [this, seededId](bool ok, const QJsonObject &body, int,
						     const QString &error) {
		if (!ok) {
			xive_debug("stream detail seed failed: %s", qUtf8Printable(error));
			return;
		}

		if (m_streamId != seededId) {
			xive_debug("stream detail seed for %s is stale; discarding",
				   qUtf8Printable(seededId));
			return;
		}

		/* The detail route nests the row under `stream` in some shapes and returns it
		 * flat in others, so accept either rather than guessing wrong and showing an
		 * empty title. */
		const QJsonObject nested = body.value(QStringLiteral("stream")).toObject();
		const QJsonObject row = nested.isEmpty() ? body : nested;

		const QString t = row.value(QStringLiteral("title")).toString();
		const QString c = row.value(QStringLiteral("category")).toString();
		/*
		 * The SLUG, seeded alongside the name.
		 *
		 * Without it the picker opened every OBS session holding a category name and no
		 * catalogue reference, which is indistinguishable from free text — so it warned
		 * "not in the directory" about a category the creator had already picked, and the
		 * only way to clear it was to search for the same row again.
		 *
		 * The route resolves it through `category_id` (detail.php: category_slug /
		 * category_name are emitted off StreamCategoryRef), so `category` here is already
		 * the catalogue's own spelling rather than the deprecated free-text column. Both
		 * halves describe one row and are seeded together.
		 *
		 * Null on both when the stream is genuinely unfiled — nothing to seed, and the
		 * empty slug then means what it says.
		 */
		const QString slug = row.value(QStringLiteral("category_slug")).toString();

		/*
		 * The creator's handle, which is what the public channel link is built from.
		 *
		 * It rides along on a call the dock already makes, so the share link costs no extra
		 * request. detail.php emits it at `stream.profiles.username`; null there means no
		 * profile row resolved, which is not a state worth guessing around — the share row
		 * simply stays disabled rather than offering a link that would 404.
		 *
		 * Stripped of any leading "@" so the URL is assembled exactly the way the website
		 * assembles it (getProfileShareUrl, src/utils/profile/profilePageHelpers.ts).
		 */
		const QJsonObject profile = row.value(QStringLiteral("profiles")).toObject();

		QString handle = profile.value(QStringLiteral("username")).toString().trimmed();
		while (handle.startsWith(QLatin1Char('@')))
			handle.remove(0, 1);

		/* Rides along on the same payload — see displayName() for why the dock prefers it to
		 * the OIDC account label. */
		const QString display = profile.value(QStringLiteral("display_name")).toString().trimmed();

		bool touched = false;
		if (!t.isEmpty() && t != m_title) {
			m_title = t;
			touched = true;
		}
		if (!c.isEmpty() && c != m_category) {
			m_category = c;
			touched = true;
		}
		if (!slug.isEmpty() && slug != m_categorySlug) {
			m_categorySlug = slug;
			touched = true;
		}
		if (!handle.isEmpty() && handle != m_username) {
			m_username = handle;
			/* Persisted so the link is on screen at the next launch before this seed has
			 * had a chance to run — and so a failed seed falls back to the last handle
			 * that was known rather than to nothing. */
			XiveConfig::instance().setUsername(handle);
			touched = true;
		}
		if (!display.isEmpty() && display != m_displayName) {
			m_displayName = display;
			XiveConfig::instance().setDisplayName(display);
			touched = true;
		}
		if (touched)
			emit changed();
	});
}

void XiveChannel::seedIdentity()
{
	XiveApi *api = XiveApi::instance();
	if (!api || m_identitySeeded)
		return;

	/* Set before the call, so the two start()s a sign-in produces make one request and not two. */
	m_identitySeeded = true;

	api->creatorChannel([this](bool ok, const QJsonObject &body, int, const QString &error) {
		if (!ok) {
			/*
			 * Quiet, and retried. The dock falls back to the handle and then to the
			 * account label, so the worst a failure costs is a creator seeing the
			 * identity provider's spelling of their name — not worth an error banner
			 * over a dock they have not touched yet. But start() runs on sign-in and on
			 * an applied credential fetch, not on a timer, so clearing the flag is the
			 * only thing that gives a launch with no network a second chance.
			 */
			m_identitySeeded = false;
			xive_debug("identity seed failed: %s", qUtf8Printable(error));
			return;
		}

		const QJsonObject channel = body.value(QStringLiteral("channel")).toObject();
		if (channel.isEmpty())
			return;

		const QString display = channel.value(QStringLiteral("displayName")).toString().trimmed();

		QString handle = channel.value(QStringLiteral("username")).toString().trimmed();
		while (handle.startsWith(QLatin1Char('@')))
			handle.remove(0, 1);

		bool touched = false;
		if (!display.isEmpty() && display != m_displayName) {
			m_displayName = display;
			XiveConfig::instance().setDisplayName(display);
			touched = true;
		}
		if (!handle.isEmpty() && handle != m_username) {
			m_username = handle;
			XiveConfig::instance().setUsername(handle);
			touched = true;
		}

		if (touched)
			emit changed();
	});
}

void XiveChannel::saveMetadata(const QString &title, const QString &category,
			       const QString &categorySlug, WriteCallback cb)
{
	XiveApi *api = XiveApi::instance();
	if (!api) {
		cb(false, tr("The Xive plugin is not ready yet."));
		return;
	}
	if (m_streamId.isEmpty()) {
		cb(false, tr("No Xive channel is set up yet. Connect first."));
		return;
	}
	if (title.trimmed().isEmpty()) {
		cb(false, tr("A title is required."));
		return;
	}
	/* The server enforces these too; checking here turns a round trip into instant feedback. */
	if (title.trimmed().length() > 120) {
		cb(false, tr("Title must be 120 characters or fewer."));
		return;
	}
	/* The cap applies to free text only. A picked category travels as a slug and the server
	 * supplies its own name, which is allowed to be longer than any client may type. */
	if (categorySlug.trimmed().isEmpty() && category.length() > 64) {
		cb(false, tr("Category must be 64 characters or fewer."));
		return;
	}

	const QString t = title.trimmed();
	const QString c = category.trimmed();
	const QString slug = categorySlug.trimmed();

	api->updateMetadata(
		m_streamId, t, c, slug,
		[this, t, c, slug, cb](bool ok, const QJsonObject &body, int, const QString &error) {
			if (!ok) {
				cb(false, error);
				return;
			}
			m_title = t;
			/*
			 * Prefer the server's spelling of the category over ours. On a slug hit it
			 * rewrites `category` to the catalogue's own name, so "minecraft" comes back
			 * as "Minecraft" — echoing what we sent would leave the dock showing a name
			 * that is not what got stored.
			 */
			const QString echoed = body.value(QStringLiteral("category")).toString();
			if (!echoed.isEmpty())
				m_category = echoed;
			else if (!c.isEmpty())
				m_category = c;
			m_categorySlug = slug;
			emit changed();
			cb(true, QString());
		});
}
