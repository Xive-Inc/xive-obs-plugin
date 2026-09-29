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

### The four docks in the middle of that table are the website

They are CEF panels pointed at the Creator Studio's popout routes — the same documents a browser
gets — so a dock and a studio tab cannot drift apart, and a feature shipped on the web is in OBS the
same day. That replaced a Qt rebuild of the same panels, which could only ever approximate the
site's look (Qt Style Sheets have no `box-shadow`, no `letter-spacing`, no text gradients) and had
to re-implement every feature by hand: its quick-actions card offered 2 of the web's 10, because
clips, markers, goals, battles, co-host invites and giveaways would each have needed a C++ port.

**You sign in once, in the Xive dock, and the panels follow.** The plugin has its own OAuth client
on its own Authentik application (`xive-obs`), which `api/app/helpers/jwt.php` accepts alongside the
web/mobile one — which means its access token is an ordinary Xive bearer. The plugin puts it on `window.__xiveObsDock`, and the page exchanges it for a session
through the app's own `login()` (`src/features/auth/obsDockSessionHandoff.ts`). No second sign-in,
and no C++ writing `xive-session` by hand: the session shape stays owned by the web.

The token is pushed, not read once — on every page load, whenever the creator signs in or out of
the plugin, and on a 10-minute timer, so a dock cannot quietly de-authenticate mid-broadcast. If
the handoff has nothing to hand over (signed out of the plugin too), the panel shows a sign-in
button and falls back to the ordinary redirect flow, with the docks' shared persistent CEF store
keeping that session across restarts.

**The token only ever reaches `https://thexive.com`.** A panel can leave the site — the identity
provider's pages on every web sign-in, a same-window link, a redirect — and the startup script runs
on every load, whatever it loaded. So it is guarded twice: the script itself checks
`location.origin` before it defines `window.__xiveObsDock`, and the plugin records each panel's
URL from `urlChanged` and only pushes a refreshed token into a panel that is on the site. A page
anywhere else sees no `window.__xiveObsDock` at all. `siteOrigin()` / `isOnSite()` in
`src/ui/xive-browser-dock.cpp`.

**Clicking a quick action opens a window, not an overlay.** In a browser the actions open a centred
modal, which is right for a full page and wrong for a 380px dock — the "overlay" would be the entire
dock, on top of the buttons just used. In a dock the plugin instead opens `/popout/action?do=<name>`
in a `QDialog` parented to the OBS main window, so it floats above OBS and closes with it. Sizes per
action live in `STUDIO_ACTION_WINDOWS` (`src/utils/streaming/openStudioPopout.ts`) and travel to the
plugin in the request, so a form's size stays beside the form.

**How the page asks.** A dock's page has no bridge to the plugin — `window.obsstudio` belongs to
browser *sources*, not panels — so the request rides on `document.title`, which obs-browser reports
through its `titleChanged` signal and which is invisible in a dock anyway. The protocol is
`src/features/obs/obsDockBridge.ts` on one side and `parseActionRequest()` on the other; both
directions carry a nonce, because the plugin is notified of a title only when it *changes*.

`window.open` was the obvious approach and is the wrong one, twice over: obs-browser stubs out
`window.close` on every panel frame, so a popup's Cancel button could never close it; and a popup's
browser has no `QCefWidget` behind it, so `shutdown()` could not close it and CEF would abort OBS on
exit — the very crash below. Popups stay blocked on every panel, which means a stray link still
opens in the creator's real browser.

**The go-live notification saves separately, on purpose.** Title and category belong to the
broadcast (`public.streams`, via `/api/streaming/creator/streams/metadata`); the notification
belongs to the channel (`public.stream_channels`, via `/api/streaming/creator/channel`) and is the
default for every future stream. One **Save** covers both, as two calls — and if the second fails
the dock says so rather than claiming nothing saved, which mirrors what the web does in the same
spot.

Reading it back is a `POST` with an empty body, which looks wrong and is not: the plugin has no
`profiles.id` to call the public `GET /api/streaming/channels/{creatorId}` with, and the stream
row's `creator_id` is an *auth* user id, not that. The write route resolves the creator from the
token, treats an omitted field as "leave alone", and answers with the whole row — so an empty body
is a read.

**They are closed explicitly at exit.** `teardownUi()` calls `XiveBrowserDock::shutdown()` before
anything else, which closes each panel's browser while OBS is still running. Without it OBS aborts
on every shutdown — `80000003` (a Chromium `CHECK`) on `CrBrowserMain`, because obs-browser shuts
CEF down inside `obs_shutdown()`, which is *earlier* than the main window destroying our docks. The
docks themselves are still left attached, so the saved layout is unaffected.

**Where they do not work:** CEF panels need the `obs-browser` module, and obs-browser refuses them
on Wayland. There the plugin falls back to the Qt docks it shipped before, and the Stream Manager
keeps its own two quick actions. Windows, macOS, the Ubuntu PPA and the Flatpak all get panels.

## Installing, and staying current

One command per platform. Each reads the same manifest — `obs/latest.json` on the CDN, written by
`scripts/publish-release.sh` — verifies the SHA-256 in it, and puts the module where that OS's OBS
looks for it. `XIVE_OBS_MANIFEST` points any of them at a staging manifest instead.

| Platform | Command | Installs to |
| --- | --- | --- |
| Linux (flatpak OBS) | `curl -fsSL https://media.xivecdn.com/obs/install-linux.sh \| sh` | `~/.var/app/com.obsproject.Studio/…/plugins` |
| Linux (distro OBS) | `curl -fsSL https://media.xivecdn.com/obs/install-linux.sh \| sh -s -- --allow-native` | `~/.config/obs-studio/plugins` |
| macOS (Apple Silicon) | `curl -fsSL https://media.xivecdn.com/obs/install-macos.sh \| sh` | `~/Library/Application Support/obs-studio/plugins` |
| Windows (PowerShell) | `irm https://media.xivecdn.com/obs/install-windows.ps1 \| iex` | `%ProgramData%\obs-studio\plugins` |

The distro-OBS row is **behind a flag because it is not a supported build**: the manifest marks
`linux-native-x86_64` as `supported: false` with `requires: Qt 6.11 / Fedora 44-era`, and the
installer says so and refuses without `--allow-native`. A module built against one libobs/Qt will
not load against another, and flatpak is the only Linux target whose toolchain is pinned.

The Windows row is **piped and not a downloaded file**, which is what keeps it a one-liner:
`ExecutionPolicy` governs script *files*, and `iex` over a string is not one, so the default
`Restricted` policy never applies; and since nothing executable is downloaded there is nothing for
SmartScreen to warn about. Saving those same bytes as `install-windows.ps1` and double-clicking it
*is* blocked — that is the `Unblock-File` / `-ExecutionPolicy Bypass` dance a creator would
otherwise have to be talked through. An unsigned `.exe` would be more friction, not less.
Uninstalling is the same script, which needs the uglier form because a piped script cannot take
arguments:

```powershell
& ([scriptblock]::Create((irm https://media.xivecdn.com/obs/install-windows.ps1))) -Uninstall
```

Before it downloads anything it refuses while OBS is running — **Windows locks a loaded DLL**, so a
copy over a live install fails partway and leaves a half-written plugin directory — and it refuses
an OBS whose major version is not 32, because OBS will not load a module built against a different
libobs major. A portable OBS it cannot find is a warning, not a refusal.

**It installs to `%ProgramData%\obs-studio\plugins`, and `%APPDATA%` is the trap.** Windows has no
per-user plugin directory: OBS's `AddExtraModulePaths()` calls `GetProgramDataPath()` there where
every other platform calls `GetAppConfigPath()`. So `%APPDATA%\obs-studio` — which really is the
OBS config directory, holding profiles, scenes, logs and `plugin_config` — is **never scanned for
modules**, and a plugin installed into it produces no error and no log line, just an OBS that
loaded nothing. The two roots OBS does read on Windows are `%ProgramData%\obs-studio\plugins\<module>\bin\64bit`
(plus `\data`) and the legacy `%ProgramFiles%\obs-studio\obs-plugins\64bit`; the
[OBS plugins guide](https://obsproject.com/kb/plugins-guide) recommends the first, which is what
this uses. It is machine-wide but usually needs no UAC prompt anyway — `C:\ProgramData`'s default
ACL lets any user create a folder and gives them full control of what they create. When that is
refused, because an admin or another user installed an earlier copy, the script says to re-run
elevated instead of failing with a raw exception.

Installing by hand still works if you would rather: extract
`xive-obs-plugin-<version>-windows-x86_64-obs32.zip` into `%ProgramData%\obs-studio\plugins` — the
zip's root is the plugin directory itself. Put it in one place only: OBS scans both roots, and
loading the same module twice is a failure rather than a preference.

### The update check

There is **no auto-update, deliberately**. OBS holds the module's binary open for as long as it is
running, so nothing can replace it from inside; and a plugin that installs code on a creator's
machine unasked is not a thing to do quietly. The rule is *never without being asked*, not *never*
— see the Windows button below. What the plugin does:

* Twenty seconds after the docks are up, and once a day after that, it reads `obs/latest.json`
  **unauthenticated** — so the check works signed out, and an expired session still learns its
  plugin is old.
* If nothing is newer, it says nothing at all. A background check that fails writes one log line.
  Only the **Check for updates** button in the Xive dock ever reports "you are up to date" or a
  failure, because only then is someone waiting for an answer.
* If something is newer, a card appears at the top of the Xive dock with the version and the one
  command that installs it, ready to copy. **Skip** hides that version until the next release
  supersedes it.
* **Tell me about plugin updates** in the dock turns the whole thing off; the preference and the
  skipped version live in `config.json` beside the rest.

On **Windows** that dialog also offers **Update and restart OBS**, which is the whole update in one
click: OBS closes, the update installs, OBS comes back. It is the same published
`install-windows.ps1` either way — the button spawns a detached PowerShell running it with
`-WaitForObs 300 -RelaunchObs <the obs64.exe that was running>`, then asks OBS to close. So one
script downloads, verifies the same SHA-256 and writes to the same directory whether a creator
pastes the one-liner or clicks the button, and fixing the installer needs no plugin rebuild.

Three things about that button are load-bearing:

* **It refuses while streaming, recording, the replay buffer or the virtual camera is active.**
  Closing OBS under a live broadcast is the one unrecoverable mistake available here.
* **Nothing in the spawned command comes from the manifest.** The URL is composed from the manifest
  URL this build was configured with, never from the JSON that came back, so a hostile manifest is
  exactly as (un)dangerous as it is for the copy-and-paste path.
* **OBS is closed by closing its main window**, not by `qApp->quit()` — that is OBS's own exit path,
  so the scene collection is saved and a creator's "confirm on exit" preference still applies. If
  they answer *no*, the waiting installer hits its deadline, says OBS is still running, and changes
  nothing.

**Copy command** stays next to it, for a locked-down machine, an install that needs elevation, or
anyone who would rather look at the command first. macOS and Linux have the copy path only.

Version ordering and manifest decoding are pure functions in `src/xive-release.cpp`, covered by
`scripts/test-update.cpp` — the fetch, the timer and the preference are in `src/xive-update.cpp`,
and the button is `XiveDock::startWindowsUpdate` in `src/ui/xive-dock.cpp`. A manifest with no
entry for this platform still reports the new version rather than staying silent.

## The launch check

Every time OBS starts, the plugin confirms the stored sign-in still works and pulls your ingest
credentials down again — so a stream key rotated on the website, or a session that quietly expired
while OBS was shut, surfaces now instead of at "Start Streaming".

It runs **silently**. A clean run touches nothing, and your dock layout is exactly as you left it:
the Xive dock stays open if you keep it open, stays closed if you closed it. Only something needing
a person opens it — an expired sign-in, no Xive channel yet, or Xive being unreachable — and then
the reason is on the panel. The checkbox in the dock ("Check this when OBS starts") turns the whole
thing off.

> An earlier build opened the dock up front and closed it again on success, which overwrote
> the dock's visibility on **every** launch. A creator who had docked Xive into their layout found
> it closed each time OBS started, and locking the UI did not help — the plugin was moving it, not
> OBS. The check now only ever opens, and never on a clean run.

## Where your docks go, and whether they stay there

All six docks are registered with `obs_frontend_add_dock_by_id` under stable ids
(`xive_account_dock`, `xive_chat_dock`, `xive_manager_dock`, `xive_activity_dock`,
`xive_alerts_dock`, `xive_actions_dock`). The first five are unchanged from the Qt docks they
replaced, deliberately — an existing creator's layout survives the upgrade to browser panels
because OBS keys that layout on the id, not on what is inside. That is the API that lets OBS remember them: it keys its saved dock state on
those ids, so where you drag a panel, whether it is tabbed, floating or closed, and OBS's own
**Docks → Lock UI** are all OBS state that survives a restart. The plugin stores none of it, and —
as of the changes below — neither overrides nor destroys it.

Renaming an id would orphan the saved entry and drop that dock back to its default position, which
is why the ids are constants rather than anything derived.

Three things broke this, found in that order:

1. **The launch check rewrote visibility on every start** (above). Real, and fixed — but not what
   made the docks vanish.
2. **Shutdown removed the docks before OBS saved.** `teardownUi()` called
   `obs_frontend_remove_dock()` for all five on `OBS_FRONTEND_EVENT_EXIT`. They are no longer
   removed — `add_dock_by_id` took ownership and OBS destroys them with the main window — which is
   correct regardless, but decoding the saved state showed this was *not* the cause either: the
   docks were in it all along.
3. **The real one: nothing restored the saved placement.** OBS applies its layout with
   `QMainWindow::restoreState()` during startup, which runs BEFORE any module is handed
   `FINISHED_LOADING` and gets to add its docks. Qt parks the entries it cannot match to a live
   widget as placeholders, and `QMainWindow::restoreDockWidget()` is the documented way to apply
   one to a dock created later. Nobody was calling it, so every Xive dock landed wherever OBS first
   put it — floating and hidden — however carefully it had been placed. `restoreDockPlacements()`
   in `plugin-main.cpp` now does, once all five exist.

### How that was pinned down

`[BasicWindow] DockState` in OBS's `user.ini` is base64 of `QMainWindow::saveState()`. Decoding it
either side of a place-it / lock / close / reopen cycle gave two blobs that were **byte-identical**
for all five Xive ids, with the one dock under test recorded exactly as it had been left:

```
xive_chat_dock      VISIBLE           geom=[27, 996, 191, 16777215]
xive_account_dock   hidden +FLOATING  geom=[0, 0, 100, 30]
xive_manager_dock   hidden +FLOATING  geom=[0, 0, 100, 30]
xive_activity_dock  hidden +FLOATING  geom=[0, 0, 100, 30]
xive_alerts_dock    hidden +FLOATING  geom=[0, 0, 100, 30]
```

A saved state that is correct, and a dock that comes back floating and hidden anyway, can only be a
restore failure — which is what ruled out (1) and (2) and pointed at (3). `[0, 0, 100, 30]` is the
geometry of a dock that has never been laid out; contrast the genuinely user-floated panels in the
same file, which keep real sizes like `[610, 440, 700, 237]`.

Plugin logs one line per dock on every start — `restored saved placement for '<id>'` or `no saved
placement for '<id>'` — so a layout that stops sticking can be diagnosed from the OBS log alone.

Two things it deliberately will not do on its own:

- **It never repoints OBS away from another service.** If your stream settings are set up for
  someone else, the check says so and changes nothing — pointing OBS at Xive stays a button you
  press.
- **It never creates a channel.** The manual button falls through to `obs/setup` when you have no
  channel, and setup *mints a new one*; a launch check has no business doing that behind your back.
  It calls `obs/refresh-credentials` only.

## Moderating from the dock

Click a name in chat — or right-click it — for delete message, mute 5 / 15 minutes, unmute, block
from chat, and make / remove moderator. Only "Block from chat" asks first; the web does not ask at
all, but this is a panel a broadcaster clicks in while looking at a camera, and a block is the one
action the viewer notices.

### Pinning, and how long for

Eight durations, the same eight the web offers (`PIN_DURATION_OPTIONS`): until you unpin, until this
stream ends, and 1 / 5 / 10 / 30 / 60 / 1440 minutes. They sit in a submenu rather than flat in the
message menu, which already carries edit, delete, five moderation items and the blocked list.

The dock previously had two of the eight and `XiveApi::chatPin` had no duration parameter at all —
`durationMinutes` was on the endpoint and supported by `StreamChatPins::expiryFromBody` the whole
time. Zero minutes is sent as an *omitted key* rather than a zero, which is what "no limit" actually
is on the wire; the server clamps anything else to 1..1440, so none of the eight can be refused for
being out of range. "Until this stream ends" is still offered only while live — the server answers
`CHAT_PIN_STREAM_NOT_LIVE` otherwise, because such a pin would be stored and hidden immediately.

### When someone is talking to you

A line containing your handle gets an amber rule and an amber wash, the same mark the web puts on
one. Two rules make it useful rather than noisy:

* **It is decided here, not sent down.** Every viewer receives the same chat rows over the same
  socket, so "is this addressed to me" has to be computed locally or everyone would see everyone
  else's highlight. `XiveChatRender::mentionsUser` is the web's regex ported value for value:
  `@ada` matches "hey @ada!" and does not match `@adamant`, `@ada_x` or `bob@ada`.
* **Never on your own messages.** A creator says their own handle constantly ("follow @ada") and
  lighting those up would make the mark worthless exactly when a real mention arrives.

A message that is both priority and a mention gets one rule in amber — the web applies both classes
and the later one wins on the properties they share, so the louder claim takes the colour.

### Editing your own message

**There is no edit endpoint anywhere on the platform.** An edit is a delete followed by a re-send —
that is how `editMessage()` works in `hooks/streaming/useStreamChat.ts`, and `chat_moderation.php`
says so in the comment above `handleStreamChatRecall`. The dock does the same thing and is explicit
about the two consequences, because both surprise anyone expecting an in-place correction:

* **The edit is a new message.** New id, new timestamp, and it lands at the *bottom* of chat. The
  banner over the composer says this before anything is sent — a broadcaster who finds their
  original gone and a duplicate further down would otherwise read that as the dock breaking.
* **The two calls are not atomic.** If the delete lands and the re-send does not, the line is gone
  from the broadcast and the only copy left is the text in the composer. So the composer is never
  cleared on failure, and `XiveChatFeed::editMessage` reports `originalDeleted` separately from
  `ok`: on that specific failure the dock leaves edit mode but keeps the text, because pressing
  Save again would try to delete a message that no longer exists and never reach the send. Pressing
  **Send** then posts the correction as a new message, which is the only thing left that can work.

Escape leaves edit mode, scoped to the composer rather than to the dock — an application-wide
Escape inside OBS would fight whatever else the creator has focused. Changing channel cancels it
too: the id belongs to the broadcast the dock just left.

### Blocked viewers, and why unblocking needs its own window

`chat/unban` was wired to the API from the start and reachable from **nowhere** — while the block
confirmation promised you could "unblock them". Every other action here hangs off a right-click on
the person's message, and unblocking cannot: `chat/ban` publishes a purge, so the moment somebody is
blocked their lines leave the feed. The person you want to unblock is by definition the one you
cannot right-click.

So **Blocked viewers…** sits at the bottom of the moderation menu — on every message, including your
own, because it is about nobody on screen. It reads `GET /chat/restrictions` and lists each ban with
who, when, who did it and why, one **Unblock** button each. The website has the identical constraint
and answers it the identical way (`StudioCommunitySection` reads that route, not the chat buffer).

Three things it deliberately does not do:

- **It never blocks anybody.** Blocking stays on the message menu where the target is unambiguous;
  picking the wrong row out of a list is a mistake this dock should not make easy.
- **It lists bans only**, though the route also returns moderators and live timeouts. Moderators are
  already visible per-message via the Mod tag, and a mute can be lifted from the person's message,
  which is still in the feed — neither has the unreachability problem that justifies the window.
- **It does not restore hidden messages**, and says so. Unbanning lets someone post again; the
  history hidden by the purge stays hidden. Neither does Twitch's.

> ⚠️ **Unblocking is wider than the list it is shown in.** A ban covers every channel its creator
> owns and `chat/unban` clears all of them, but `restrictions` returns only the channel asked about
> — deliberately, so one channel's roster does not display another's. Do not read the list as
> "everyone this creator has blocked".

The web reveals a "⋮" button on hover. A `QTextBrowser` cannot host a widget inside its text, so the
name itself is the target — it is an `<a href="xive-msg:…">` and the dock hit-tests for it. With
`setOpenLinks(false)` nothing is ever navigated to.

Your own messages offer delete and nothing else, exactly as the web hides its menu on your own
messages: the plugin signs in as the channel's own creator, so `is_creator` marks your own lines.

> 🔴 **These seven endpoints did not exist until 2026-08-09.** They were never ported off Deno, and
> the omission survived the migration because the work was scheduled off an nginx traffic
> classifier — and a route that 404s generates no traffic to classify. The website's own moderation
> menu calls the same paths, so chat moderation was dead platform-wide, not merely absent here.
> Ported in `api/app/streaming/chat_moderation.php` with `api-tests/test_chat_moderation.php`.

### What the dock still does not have

Named rather than left to be discovered, because every one of these exists on the website's chat and
somebody will go looking for it:

| Missing | Why it is not here yet |
| --- | --- |
| **Emotes** | No picker and no rendering. A `QTextBrowser` can show images, but each emote would have to be fetched, cached and installed as a document resource per message — a real subsystem, not a widget. |
| **Raid, co-host invite, manage goals** | Three separate features with their own pickers and editors. The manager dock has poll and ad break; the web's quick actions have these three as well. |
| **Chat identity picker** | Deliberate. Setting your own colour and badge is a set-once thing that belongs on the site; the dock renders what you chose. See [Chat identities](#chat-identities). |
| **Mute notice** | Not applicable. The composer's "you are muted until…" banner is driven by `viewer_timeout_expires_at`, and this dock is always signed in as the channel's own creator — who cannot be muted in their own chat. |

All four disappear at once if the chat dock ever becomes a CEF browser panel pointed at
`/popout/chat`, since that is the website's own `StreamChatPanel`. That trade is scoped separately.

## Chat identities

The name colours and badges people configure on the website are what the dock draws. There is no
second identity system here and nothing is guessed locally.

**Name colour.** A valid `#rrggbb` in `chat_name_color` wins outright. Everyone else falls to one of
eight colours chosen by hashing the user id — the same hash the web uses, so a given person is the
same colour in the dock as on the site. Both arms are live: most viewers have set no colour, so the
hash is not a dead path.

**Badges.** `chat_badge` carries one equipped badge, and the dock draws the catalog's own mark for
it. The ids come from `api/app/streaming/chat_identity_routes.php`:

| Badge | Drawn as | Earned by |
| --- | --- | --- |
| `staff_ceo` / `staff_exec` / `staff_dev` / `staff` | Labelled chip — CEO fuchsia, Exec gold, Dev red, Staff cyan | `admin_role` / `is_admin` on the profile |
| `pro` | ✦ purple | Xive Pro |
| `xive500` | `500` chip, amber | Founding Xive 500, with Pro still active |
| `verified` | ✓ sky | Verified account |
| `creator` | 🎬 amber | Platform creator |
| `subscriber` | ★ emerald | An active subscription to THIS channel |
| `level_10/25/50/100` | Numeral chip, violet | Account level |

The first three families render as a bare glyph, because the mark *is* the badge and the web draws
the same one. **The staff tiers keep a chip and a word**, and that is not a styling preference:
there is no glyph for "this person works at Xive", and a lone crown reads as a gift or a level to
anyone who has not been told otherwise. Drawing them at face value is safe because none of them can
be self-granted — the catalog only offers a tier to a caller whose own profile carries it, exactly
one tier per account, and the save endpoint refuses any badge that caller's catalog did not unlock
(403 `Badge not unlocked`).

> 🔴 **All four staff tiers used to fall through to a generic ★**, which made the CEO
> indistinguishable from an unrecognised badge id. Fixed 2026-08-19, along with the docs that said
> identity never arrived at all — see the correction under
> [Server-side notes](#server-side-notes-that-shaped-this).

**What is deliberately NOT drawn.** `is_pro` and `is_subscriber` are on every message and neither
renders: the web puts neither in a message row, and supporter standing reaches chat only through
`chat_badge`, which is the badge. `avatar_url` is `null` on every message the server shapes, so
there are no avatars in stream chat on any surface. `supporter` stays null platform-wide —
`SUPPORTER_CHAT_IDENTITY_ENABLED` is unset in every environment and the loader behind it is
deliberately unported.

**What the dock cannot show.** There is no way to *change* your own identity from OBS. The settings
live behind `GET`/`PUT /api/streaming/chat/identity`, which the plugin does not call — the picker is
a modal with a badge catalog, a colour swatch grid and per-channel overrides, and it is a page, not
a dock control. Sign in on the website, set it once, and every message from that account carries it
everywhere including here.

## Xive Surge

The banner over the chat, and the only part of the dock that is about what the **room** is doing
rather than what the broadcaster is doing. That is why it is on the chat dock and not in the stream
manager — the same call the website makes, where it sits over chat on the watch page and not in the
creator's studio.

A surge is a **chain** of contributions with no gap longer than five minutes, not a sliding window.
Gifts, Sparks, subs and gifted subs each score points and each buys another five minutes; a silence
longer than that breaks the chain. So the score only ever goes up while people are still giving, and
the countdown is "how long this room has to keep it alive" — which is why the meter refills on every
contribution instead of draining towards a finish line.

Three visible states, and the first of them is the point of the feature:

| The room | What the dock shows |
| --- | --- |
| Nobody has given | Nothing at all. This sits directly over the message list in a panel that is often a few hundred pixels tall; a permanent strip announcing that nothing is happening would cost two lines of chat for the whole broadcast. |
| Giving, but short of the threshold | One quiet line — "⚡ Support is building — 7 of 12 to start a XIVE SURGE." It is the only state anyone can still change the outcome of. |
| Surging | Tier and level, the window meter, `N people · N contributions · N pts · N to level M`, and the contributor belt with the **Surge Igniter** (👑) pinned at the front. |

`points` is a **weighted score**, not a count — one large gift is worth several small ones — so it
is labelled `pts` and never dressed up as a number of anything. `events` is the honest count and is
printed as one. This is the same rule the studio card follows.

### Where the number comes from

Four inputs, one answer, mirroring the web's `useStreamSurge`:

1. `GET /api/streaming/{id}/surge` whenever the channel changes. A socket carries only what happens
   after you subscribe, so without this a dock opened **mid-surge** shows nothing until the next
   contribution — which on a quiet stretch is minutes away.
2. The `stream:<id>` publication, emitted server-side on every contribution. This is what makes the
   level climb in real time, and it costs no extra connection: chat and the viewer count already
   listen on that channel.
3. A 30s poll, running **only while the socket is not connected**. It backs off to five minutes
   while the socket delivers rather than stopping — see below.
4. A local clock, ticking once a second and only while a surge is live.

> ⚠️ **Nothing is published when a surge lapses.** The last contribution already decided when the
> window closes, and no request arrives at that moment to write anything — a line posted late
> ("that surge ended four minutes ago") is worse than the banner simply going away. So input 4 is
> not a nicety: without it the dock sits showing a surge for a room that went quiet twenty minutes
> ago. It is also why the arriving state is re-checked against the clock before it is stored, since
> a read that spent four seconds in flight can describe a window that has already closed.

> ⚠️ **An inactive state is a real event, not an empty one.** The server publishes one on a
> contribution that does not qualify, and that publication is how a client which saw a surge start
> learns it has ended. Dropping it because it looks empty leaves the banner up until the poll
> catches up.

The backstop poll is why the socket does not stop it entirely, unlike chat's: a surge that ends
produces one publication and no more, and while the local clock still retires the banner on time, a
surge that then *started* again would be invisible until the socket happened to carry a later
contribution. Five minutes costs one request an hour.

### The constants are a third copy

`kStartThreshold`, `kLevelStep`, `kMaxLevel` and `kWindowSeconds` in `src/xive-surge.h` mirror
`api/app/helpers/stream_surge.php`, which itself mirrors
`src/components/streaming/studio/sections/studioXiveSurge.ts`. Three implementations of one number
is a real cost, accepted deliberately server-side; the plugin inherits it because the dock has to
say "3 more to level 4" and the gap is not on the wire.

Nothing here scores an event — the server is the only thing that computes a level — but a stale copy
still does not fail to build and does not throw. It quietly tells a broadcaster one number while the
banner every viewer is looking at says another. `scripts/test-surge.cpp` is the only thing standing
between those two, and it writes the server's values out longhand rather than deriving them from the
constants it is checking.

### The announcements in chat

The server posts the climb into chat as system messages — approaching, level 1, levels 2-5, then
every fifth level. Those arrive as ordinary `chat/messages` rows with `is_system` set, and the dock
renders them as platform notices. See the note on `is_system` under
[Server-side notes](#server-side-notes-that-shaped-this): they are tested **before** any content
parsing, because a notice is free text and one that happened to parse as a poll vote used to be
swallowed into nothing.

## Picking a category

The category field searches `public.stream_categories` — about 16,000 IGDB games plus hand-curated
categories — through `GET /api/streaming/categories`. Type to search, or press <kbd>↓</kbd> to
browse. It is the same catalogue the website's picker uses, so a category chosen here and one
chosen there are the same row.

What a pick buys you is `category_id`, not just a string: the broadcast is then listed under that
category in the directory. Free text still works and is not an error — the line under the field
says which of the two you have — and on save the server falls back to matching the display name,
exactly as every client did before the catalogue existed.

Two server details the plugin has to work around, both in `app/streaming/writes.php`:

- **A picked category is sent as a slug and nothing else.** The 64-character cap is applied to the
  `category` TEXT *before* the slug is resolved, and 23 listed categories have names longer than
  that (the longest is 108). Sending the name alongside the slug would 400 on exactly those rows —
  for a name the server itself supplied. On a slug hit the server rewrites `category` to the
  catalogue's own spelling anyway, so nothing is lost by omitting it.
- **`obs/setup` has no slug parameter**, and caps `category` at 64 the same way. The plugin drops
  an over-long category from that call rather than truncating it; setup reuses whatever is already
  on the row when the field is absent, so the category the manager dock wrote survives.

There is no slug to show when the dock first opens: the stream detail route returns `category` as
text and carries neither slug nor id, so the field starts as free text until the creator picks. It
resolves correctly on save either way.

## How sign-in works

OAuth 2.0 Authorization Code with PKCE and a loopback redirect (RFC 8252):

1. The plugin binds `127.0.0.1` on port **47525**, or 47526 / 47527 if that one is busy.
2. It opens your browser at `auth.thexive.com` with an S256 challenge.
3. Authentik redirects back to the loopback listener with a one-time code.
4. The plugin exchanges the code for an access token (1 hour) and a refresh token (30 days), and
   refreshes silently from then on.

The listener is reachable by any page in your browser while a sign-in is waiting, so it trusts
nothing it is sent. The `state` is checked **before** anything else — including an `error=`
response — so a request that does not carry this sign-in's `state` is answered and ignored, and
cannot cancel the sign-in or feed it someone else's code. Everything it writes back into its
confirmation page is HTML-escaped. It accepts one code, then stops listening, and gives up after
five minutes.

Those three loopback URLs are registered as **strict** redirect URIs on the Authentik `xive-obs`
provider. They are not optional — sign-in fails with `redirect_uri` mismatch without them.

### Two deliberate decisions worth knowing

**It has its own client id and its own Authentik application.** It originally reused `xive-local`,
the web and mobile client, purely so the server needed no change — at the cost of an OBS session
being unrevokable without also killing that user's web and mobile sessions. As of 2026-08-20 it has
its own application, `xive-obs`, and that cost is gone.

The price is paid on the server. Authentik derives `iss` from the application slug, so a separate
application means a separate issuer as well as a separate audience, and `api/app/helpers/jwt.php`
checks both. `OIDC_ISSUER` and `OIDC_AUDIENCE` are therefore comma-separated lists holding the
web/mobile application and this one. **Changing the client id here without adding it there gets
every API call rejected with `invalid_audience` while sign-in itself still looks like it worked.**
If per-device revocation is wanted later, that is a new Authentik provider plus a multi-audience
change to `jwt.php`.

**The refresh token is stored in plaintext, mode 0600**, in
`~/.config/obs-studio/plugin_config/xive-obs-plugin/config.json` (Linux) or the macOS equivalent.
This is the same exposure OBS itself accepts for the credentials in its `service.json`, and there is
no cross-platform secret store available to a plugin without a per-OS dependency. Anyone who can
read the user's home directory can obtain a 30-day Xive refresh token.

## Building

### Linux

Needs `libobs` development headers and Qt 6. **The OBS major version must match the OBS you intend
to run it in** — OBS refuses to load a module built against a different libobs API version, so a
plugin built against OBS 31 headers will not load in OBS 32.

A container is the least invasive way to get a matching toolchain:

```bash
podman build -t xive-obsdev -f Containerfile .
podman run --rm -v "$PWD":/src:z -w /src xive-obsdev \
    bash -c 'cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build'
```

Then `cmake --install build` drops it into `~/.config/obs-studio/plugins/`.

`scripts/loadtest.sh` goes further: it starts a real OBS on a virtual X server and asserts from
OBS's own log that the module loaded and the docks registered. Building is not the same as loading,
and this catches the difference.

### Checking the logic, and checking the look

Two things that need no OBS at all, both run inside the flatpak SDK from the project root:

```bash
flatpak run --devel --filesystem="$PWD" --command=sh com.obsproject.Studio -c './scripts/run-tests.sh'
flatpak run --devel --filesystem="$PWD" --filesystem=/tmp --command=sh com.obsproject.Studio \
    -c './scripts/preview-chat.sh'          # -> /tmp/xive-chat-preview.png
```

`run-tests.sh` asserts the pure logic — the activity classifier, and the chat renderer's colour
hash, gift parsing, badges and quick-action markers. None of that fails loudly in production: a
colour hash off by one index recolours every name in chat, and a marker the parser rejects leaks
`XIVE_POLL_VOTE:…` into a live broadcaster's dock as text.

`scripts/test-service-detect.sh` is the third one, and it needs a real OBS because the answer comes
out of libobs. It writes a profile `service.json`, starts OBS on a virtual X server, and reads back
whether the plugin recognised the configured service as Xive's — across five shapes, including the
named `rtmp_common` service whose `server` setting is empty. Run it from the host, not inside the
flatpak. It exists because getting this wrong is silent: the first version tested the server URL for
`thexive.com`, which is the **API** host and not the **ingest** host, so every correctly-configured
creator was told they were set up for another service.

`preview-chat.sh` covers what assertions cannot. It runs the real message HTML through a real
`QTextDocument` with the real stylesheet, over a sample of every message shape, and writes a PNG —
so you can see that Qt quietly ignored a CSS property and left a card with no border, without a
broadcast to talk into. Qt's rich text is an HTML 3.2 subset and it drops what it does not
understand in silence; `border-left` on a block and `border-collapse` on a table are both already
casualties.

### macOS

macOS has no libobs development package, so two things get fetched by hand:

```bash
# 1. libobs headers, at the SAME version as the OBS you are targeting
git clone --depth 1 --branch 32.2.1 https://github.com/obsproject/obs-studio.git ~/obs-studio-headers

# 2. Qt — the obs-deps build, i.e. the same Qt OBS itself ships
curl -sL -o /tmp/qt6.tar.xz \
  https://github.com/obsproject/obs-deps/releases/download/2026-07-15/macos-deps-qt6-2026-07-15-arm64.tar.xz
mkdir -p ~/obs-deps-qt6 && tar -xJf /tmp/qt6.tar.xz -C ~/obs-deps-qt6

brew install cmake ninja simde

cmake -S . -B build-mac -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$HOME/obs-deps-qt6" \
  -DXIVE_OBS_SOURCE_DIR="$HOME/obs-studio-headers"
cmake --build build-mac
cmake --install build-mac
```

Three macOS-specific traps, all already handled in `CMakeLists.txt`:

- **Use obs-deps Qt, not Homebrew Qt.** Homebrew's Qt 6.11.1 does not ship `qtmochelpers.h`, which
  moc 6.11.1 emits an unconditional `#include` for, so every moc-generated file fails to compile.
- **`brew install simde` is required.** `libobs/util/sse-intrin.h` includes `<simde/x86/sse2.h>`
  because Apple Silicon has no real SSE intrinsics. Linux distros pull SIMDe in automatically as a
  dependency of the libobs devel package; macOS does not. The include path deliberately points at
  the Cellar directory rather than `/opt/homebrew/include`, because a machine with `qt@5` linked has
  Qt 5 headers there that would shadow Qt 6.
- **Nothing is linked** — `-undefined dynamic_lookup` resolves libobs and Qt from the host process
  at load time. OBS.app loads its own Qt frameworks; a plugin that also linked a second Qt would put
  two QtCore copies in one process, which crashes rather than degrades. Build Linux first: it links
  everything properly and so catches undefined symbols before macOS can turn them into a load
  failure.

`cmake/macos-shim/obsconfig.h` stands in for a header OBS's own CMake generates. Its values are
irrelevant to a plugin; read the comment in the file.

## Publishing

`scripts/publish-release.sh`, **on xive-web** — the R2 credentials are in `/opt/xive/.env` on that
box and the script reads them itself, so they never reach a shell history or a transcript. It
takes the version from `CMakeLists.txt` and the files from `/opt/xive/streamplugins/dist`, so
stage all of these there first:

```
xive-obs-plugin-<version>-linux-x86_64-FLATPAK-obs32.so
xive-obs-plugin-<version>-linux-x86_64-obs32.so
xive-obs-plugin-<version>-macos-arm64-obs32.plugin.tar.gz
xive-obs-plugin-<version>-windows-x86_64-obs32.zip     (optional — produced by windows-build.ps1)
en-US.ini
SHA256SUMS                                             (optional; checked when present)
```

`--dry-run` prints the manifest and every upload without touching the bucket. Windows is optional
because it is built on a different machine and is not always staged at the same time; when it is
absent the manifest simply carries no `windows-x86_64` entry, which the plugin's update check
handles by saying so.

### Re-publishing a version that already exists

`obs/<version>/<file>` is served `immutable` — a promise that those bytes never change. Publishing
the same version with different bytes breaks it in the worst way: the manifest's sha256 changes,
edge caches keep serving the old file for up to a year, and the installers verify the checksum and
refuse. The script **refuses on its own** when the version directory is already in the bucket.

Bumping the version is the normal answer: a version the bucket has never seen publishes without
hitting the guard. `XIVE_REPUBLISH=1`
is the escape hatch for the other case — replacing the bytes of a version that is already up, for a
release line nobody has installed yet. **The purge is what makes that safe**, so if `CLOUDFLARE_API_TOKEN` /
`CLOUDFLARE_ZONE_ID` are not set, purge `obs/<version>/*` by hand before telling anyone to install;
the script says so too.

### Publishing the source

The plugin's source has to be public. **Publish a copy, never this repo** — its history carries
everything below. The copy is the tracked files of this directory with these taken out:

- `scripts/windows-setup.ps1` — enables SSH on the Windows build machine and carries our public key.
- `scripts/publish-release.sh` — names xive-web, where the R2 and Cloudflare credentials live, and
  the bucket layout.
- In this README, everything from `## Publishing` to the end: the release process, the untested
  paths listed below, and the server-side notes.
- In `scripts/windows-build.ps1`, the lines that name `/opt/xive/streamplugins/dist`, xive-web and
  `windows-setup.ps1`.

The copy made on 2026-09-29 is `/home/bazzite/xive-obs-plugin-public`. Before re-publishing, grep
the copy for `xive-web|/opt/xive|/etc/xive|\.env|R2_|CLOUDFLARE_|store-machine|bazzite` and expect
only harmless mentions of the removed script's name in comments.

A security review of the public source on the same date found no secrets (the client is public and
PKCE-only), and led to the token-origin guard and the loopback `state`-first check described above.
The one open item is that the installers verify a SHA-256 from the same CDN they download from —
integrity, not authenticity — so write access to `media.xivecdn.com/obs/` is install access to
every creator who runs the command. Signed releases would close that.

## What is not verified

Honest gaps, so nobody assumes more than was tested:

- **No authenticated call has run.** Every endpoint path was probed unauthenticated and confirmed to
  reach a real handler (401/400/404-from-handler, never a routing 404). But sign-in needs a human in
  a browser, so the token exchange, refresh, credential fetch, chat send and metadata write have not
  been exercised against a live session.
- **The loopback ports are unverified against Authentik.** The authorize URL was confirmed to accept
  port 47825 and reject an unregistered port, but that was the OLD port set. 47525-47527 have not
  been exercised, and will fail with `redirect_uri_mismatch` until they are registered as strict
  redirect URIs on the provider.
- **The launch check has only run signed OUT.** It is confirmed to open the dock on start (the dock
  widget really is findable by its id) and to leave it open asking for a sign-in. The paths that
  matter — a live token, a rotated key being pulled down, the dock closing itself — all need a
  session, so none of them have run.
- **Mostly no GUI check.** The docks are confirmed to register without crashing. The chat dock's
  rendering has been reviewed as a PNG (`scripts/preview-chat.sh`); the other three have not been
  looked at at all, and their layout and wording are unreviewed.
- **Nothing added on 2026-08-19 has been built or run.** Xive Surge, the system-notice branch, the
  staff badges, message editing and the blocked-viewers window are source-only: the pure logic has assertions (`scripts/run-tests.sh`, now four
  suites) and the two new chat shapes are in `scripts/preview-chat.cpp`, but neither has been
  compiled here — this checkout has no Qt and no compiler. The surge banner and the blocked-viewers
  dialog are widget stacks whose layout nobody has looked at, and the live paths (a real publication
  arriving, the countdown ticking down, the banner retiring itself, a roster coming back with rows
  in it) need a broadcast with money moving through it, which is the same gap every authenticated
  path below has.
- **The pin durations and the mention highlight are source-only too.** The mention regex and the
  eight durations have assertions (`scripts/run-tests.sh`) and the highlight is in
  `scripts/preview-chat.cpp` so its colours can be looked at, but no pin with a duration on it has
  ever been sent from this dock.
- **The edit failure path is unexercised.** The recoverable case — recall lands, re-send fails, the
  composer drops out of edit mode still holding the text — is reasoned from the two endpoints'
  contracts and has never been made to happen. It needs the send to fail *between* the two calls,
  which is not something the dock can arrange on demand.
- **Linux Flatpak IS covered** (corrected 2026-08-09). `com.obsproject.Studio` runs on
  `org.freedesktop.Platform`, not `org.kde.Platform`, and ships the libobs and Qt6 CMake configs, so
  the plain `find_package` path builds against it. Plugins load from
  `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/`, and a flatpak-built module is
  verified loading there. A module built against Fedora's libobs still will not.
- **Windows is not built or tested from this checkout.** The zip in `dist/` comes off a separate
  machine via `scripts/windows-build.ps1`; this box has no Qt, no compiler and no Windows.
  `scripts/install-windows.ps1` has been reasoned through line by line and never executed — the
  OBS-running refusal, the version probe, the checksum compare and the extraction are all unrun.
  The one that would bite quietly is the zip's backslash-separated entries; the script records
  .NET normalising those into real directories as verified, and it searches for the DLL anywhere
  under the staging directory rather than asserting a path, so if that ever stops holding it is a
  clear error rather than a silently broken install.
- **The one-click update has never run, in either half.** `-WaitForObs` / `-RelaunchObs` and
  `XiveDock::startWindowsUpdate` are source-only: no compiler here, and no PowerShell to so much as
  parse the script. Worth watching the first time, in this order — that the spawned window appears
  *before* OBS goes away, that answering "no" to OBS's exit prompt ends in the deadline message
  rather than a hang, that the install lands in `%ProgramData%` and not beside a stale `%APPDATA%`
  copy, and that OBS comes back with `[xive] plugin ready` in its log. The active-broadcast refusal
  is the one to test deliberately rather than wait to meet.
- **The token-origin guard and the loopback hardening (2026-09-29) are source-only.** Not
  compiled or run. Check after the next build: a fresh sign-in, all four docks picking it up, the
  hourly refresh reaching them, sign-out clearing them — and a dock sent off-site (devtools, or a
  same-window link) must show no `window.__xiveObsDock`.
- **No code signing or notarization on macOS.** Fine for a locally built copy; required before
  distributing to anyone else.

## Server-side notes that shaped this

- `obs/refresh-credentials` is called **before** `obs/setup`, always. Setup refuses to reuse a row
  that is currently live and mints a *new* channel instead — which on 2026-08-09 bound a creator's
  studio to an empty channel while their real broadcast carried on elsewhere. Refresh is read-only,
  resolves by creator, and prefers a live channel.
- The stream key is **permanent per creator** by design, so rotation is genuinely destructive: any
  other encoder or device still holding the old key stops being able to publish. Hence the
  confirmation dialog.
- `chat/messages` answers HTTP 200 with `success: false` and an empty list when it fails internally,
  so a chat widget shows no history rather than an error over the video. The dock distinguishes that
  from "chat is quiet" instead of conflating them.
- `chat_name_color` and `chat_badge` **arrive populated** (corrected 2026-08-19). They used to be
  null on every message: `ChatIdentity::ENRICH_CHAT_IDENTITY` was off for parity with a Deno API
  whose profile load silently threw `operator does not exist: uuid = text` and swallowed the error
  to an empty map. Deno is retired, the cast is fixed, and the flag is on — so the identity a
  viewer configures is what the dock draws. The plugin needed no change for that; it needed the
  four **staff** badges, which the catalog serves and which used to fall through to a generic ★.
- `is_system` marks a line the PLATFORM wrote — Xive Surge announcements, and whatever an
  administrator posts. It is tested BEFORE any content parsing, on both surfaces, because a notice
  is free text: one that happened to look like a poll vote was previously swallowed and never
  drawn at all.
- The gift leaderboard is scoped to the current broadcast server-side. It has to be: the OBS stream
  row is permanent, so an unscoped read would show every gift the creator ever received as if it
  arrived tonight.
- Applying credentials is refused while OBS is streaming. Repointing mid-stream does not move the
  live broadcast; it silently changes where the *next* "Start Streaming" goes.
