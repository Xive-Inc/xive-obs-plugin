# Xive for OBS

An OBS Studio plugin that signs a creator in to Xive, configures their stream key for them, and
puts chat, the stream manager and gift activity in OBS docks.

Status (2026-09-29): **v0.1.6 is published** (released 2026-09-28, media.xivecdn.com/obs/
latest.json), and `CMakeLists.txt` still says 0.1.6 — **but the source is ahead of it.** The
token-origin guard and the loopback hardening (commit 63c1f95f, 2026-09-29) are security fixes that
are in no release yet.

**Bump to 0.1.7 before building them.** The `obs/0.1.6/` directory is already in the bucket and
served `immutable`, so the publish script refuses to reuse the version, and forcing it would break
every installer that has cached the old checksum — see [Publishing](#publishing).

## What it does

| Dock | What it gives you |
| --- | --- |
| **Xive** | Sign in / out, which channel is yours, and one button that writes Xive's ingest URL + stream key into OBS's own stream settings. Also rotates the key, behind a confirmation. |
| **Xive Chat** | `/popout/chat`. The live chat panel from the Creator Studio, unchanged. |
| **Xive Activity** | `/popout/activity`. Follows, gifts, Sparks and supporter unlocks as they happen. |
| **Xive Quick Actions** | `/popout/actions`. The studio's action grid — poll, ad break, raid, clip, giveaway, goals, battle invite, co-host invite. Clicking one opens it in **its own window** rather than inside the dock (see below). |
| **Xive Gifts** | `/popout/gifts`. Top gifters and credit totals for this broadcast and all time. |
| **Xive Stream Manager** | Title, category (searched against the Xive directory), the go-live notification (on/off plus an optional message), live viewer count, and your channel link. Native Qt — the studio's stream-details panel carries the ingest credentials, so it has no popout route to defer to. |

