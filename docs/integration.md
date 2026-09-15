# Integrating the television into an Open77 tree

`open77_media/` is self-contained. The **render** is not: a browser composited
onto a world quad is host-side work, so the resource needs a handful of seams in
the plugin. This document names each one, in the order they matter, and records
the two things that will otherwise cost you an evening.

`patches/` holds the television-relevant hunks of every modified seam file.
Those files also carry other unreleased work, so the patches are **not a clean
patch series** — apply them by hand, reading each one.

---

## 1. The new modules

The feature-only files, all copied whole into `native/` (the ones whose seam is
not the client are named in the section they belong to — the decoder in 6c, the
page policy and the frame probe in 6a, audio in 6b):

**`client/src/api/MediaScreens.hpp` / `.cpp`**

Binds a WebUI surface to a world prop, projects that prop's screen quad every
tick, publishes one `WorldOverlay` item per visible screen, and decides
visibility by testing line of sight to the screen's centre. It holds no entity:
the binding is `{"this surface, on that prop's screen"}` and it is dropped when
the prop is not projected. That is what lets a screen be bound before its prop
has streamed in, and lets the prop move without the screen caring.

**`client/src/api/ScreenQuad.hpp`** — the quad arithmetic, and the facing gate.
A record may declare which side its picture is on; without that a screen is drawn
from whichever side you stand on, which in game is "the television is playing on
both sides". The gate also has to answer for a record that declares *nothing*, and
it says so in the snapshot (`facing.known`) rather than guessing.

**`client/src/webui/ScreenMotion.hpp`** — the picture is anchored to the frame
that is presented, not to the tick that projected it. Corners are produced once
per game tick (24–69 Hz in the log this came from, and jittery); the overlay
draws per presented frame, which under frame generation includes frames that sit
*between* two ticks. Holding a corner set for up to 42 ms and then stepping reads
as the image sliding around on its own cabinet. The module blends the two most
recent samples, interpolates only (a fraction of 1 reproduces the producer's
numbers bit for bit), and refuses to blend across a sample older than 250 ms — a
screen switched back on draws where it is, not sweeping in from where it was.

**`client/src/webui/ScreenInput.hpp`** — which world screen holds the pointer and
the keyboard, and where on that screen the page pointer has landed. This exists
because a screen session used to draw **two** pointers: `WebUiService` put its
overlay cursor at the raw pointer's viewport coordinates, while `MediaScreens`
published a `Style::Dot` item at the projection of the pointed-at page pixel onto
the panel. They coincide only when the panel fills the viewport, which a
television in the world never does, so you aimed with one and clicked with the
other. The panel now publishes where its quad landed as a viewport fraction
(`PublishPointerOnScreen`, called from the same arithmetic that placed the dot and
cleared at the top of every tick, so a screen that stops being built cannot leave
last tick's cursor on a wall) and `WebUiService` draws its cursor there. A tick
that publishes nothing — prop unstreamed, quad refused, camera behind it — falls
back to the raw pointer, which is what happens with no session open.

`WorldOverlay::Style::Dot` stays in the overlay's closed set: nothing produces it
now, and removing a style a future producer might want is the worse trade.

The debug bridge carries the session as `webui.input`'s `screen=#N u… v…` field,
because handing the pointer and keyboard to a television is the one input state
with no window to look at — it is the difference between "the key did nothing" and
"the key worked and the click is landing somewhere else".

**`client/Plugin.cpp`** is the lifecycle: `MediaScreens::Initialize` at plugin
load, `OnRunningEnter` / `OnRunningUpdate` / `OnRunningExit` on the game
application, `ReleaseAll` on shutdown. `OnRunningUpdate` runs *after* the
resource host and after `Props`, so a screen bound or re-aimed this frame is
projected against this frame's camera and a prop spawned this tick is already
visible to the lookup. `OnRunningExit` is load-bearing for a different reason —
see "a page no longer outlives its world" below.

`client/CMakeLists.txt` adds the new sources to `Open77.Client`:

```cmake
src/api/MediaScreens.cpp
src/api/MediaScreens.hpp
```

## 2. The Lua surface

**`scripting/src/ResourceHost.cpp`**, **`scripting/include/op77/Scripting/ResourceHost.hpp`**

The host publishes an `Open77.media` table:

| Lua | behaviour |
|---|---|
| `Open77.media.bind{ prop=, surface=, offset=, right=, up=, width=, height=, label= }` | returns a screen id, or `nil, reason` |
| `Open77.media.update(id, {…})` | moves / relabels a screen |
| `Open77.media.unbind(id)` | releases one screen |
| `Open77.media.clear()` | releases every screen this resource owns |
| `Open77.media.list()` | the snapshots, for diagnostics |

All of it is gated by the **`world.props`** capability, not a new one: a screen
with no prop has nothing to be a screen on, so it is the same authority.

`GameBackend` grows the matching contract — `MediaScreenQuad`,
`MediaScreenDefinition`, `MediaScreenSnapshot`, and five `std::function` hooks:
`bindMediaScreen`, `updateMediaScreen`, `unbindMediaScreen`,
`releaseMediaScreens`, `mediaScreenSnapshot`. On the **server** these are null,
so the natives answer `media_backend_unavailable` rather than pretending.

## 3. The client supplies the backend

**`client/src/scripting/ClientResourceHost.cpp`**

Wires the five hooks to `Api::MediaScreens::Bind/Update/Unbind/ReleaseAll/…`,
translating a binding failure into the string the Lua caller sees.

## 4. The render seam

**`client/src/webui/WorldOverlay.hpp` / `.cpp`** — a new `Style::Screen`. This is
the one overlay style that is not a shape from the atlas: the content is a page
CEF has already rasterised into a texture the client holds a descriptor for, and
what the overlay supplies is the one thing a page cannot supply for itself —
**where on the world the rectangle goes**. Read the comment on the enum member
for the occlusion caveat; it is the same text as the README.

**`client/src/webui/WebUiService.hpp` / `.cpp`** — two additions:

* a **surface texture accessor**, so the overlay can sample the CEF texture for
  a given surface;
* **`hudSuppressed`**, per surface, on the create options. Without it a
  television's page is composited onto the world quad *and* blitted fullscreen
  across the viewport, which looks like a bug in the game rather than a bug in
  this feature. The resource's manifest asks for it with
  `web_ui_auto_create false` plus the flag on the create call.

## 5. The server has to load it

The resource list is per-config, so add `"open77_media"` alongside the other
`resources.list` entries. In the tree this came from that is:

```
server/server.jsonc
server/server.pvplab.jsonc
templates/freeroam/template.json
```

Its manifest declares `auto_start true`, `reload_policy "local"`, a
`web_ui_page "web/tv.html"` that is never auto-created, and exactly two
capabilities: `network.events` and `world.props`.

## 6. The menu tab

**`resources/gamemodes/freeroam/`** — `web/index.html` (the TV tab),
`web/app.js` (catalogue, spawn, live-screen list, the nudge and turn controls),
`client/main.lua` (the client half: applies the state snapshot, binds surfaces,
forwards a control to the server).

Two things about the list are worth stating, because both were bugs first:

* It is **nearest-first**, with every row saying which set it is — `ON SCREEN ·
  1.4 m away` versus `DRIVING A SET ELSEWHERE · 3120.5 m away`. The list carries
  *every* set on the server, and a row three kilometres away used to look
  identical to the one in front of you, which is how a volume change read as
  "the slider does nothing" while it was in fact changing a television in
  another district.
* It shows the sets this client is actually rendering separately from the rest,
  because only the first group has a picture here to look at.

This is the second gotcha below: the tab **is not in `open77_media`**.

## 6a. The page policy, and the host's other gate

**`webui/include/op77/WebUI/PagePolicy.hpp`** (copied whole into `native/`),
**`webhost/src/SurfaceClient.{hpp,cpp}`**, **`webhost/src/Main.cpp`**

A page that shows a third-party player needs two things, and having one without
the other is the failure this seam was written in response to:

* a **policy** — the CSP directives that permit a remote frame and script for the
  one page whose job is to play a pasted link, and refuse them everywhere else;
* a **request handler that does not cancel the requests anyway**. The host (this
  predates the feature) sets `aDisableDefaultHandling = true` on its resource
  request handler and cancels any navigation off the page's own origin, so every
  request the host does not serve itself simply fails — and `frame-src
  https://www.youtube.com` in a header described a privilege that was never
  granted. The symptom was a player script reporting that it had been refused,
  which reads as "the embed is blocked" and sends you looking at the header.

Both halves now come from the same file, and `webui/tests/WebCoreTests.cpp` pins
the directives. Nothing in the resource can name a policy; it can only ask for
one.

### The frame grant, which is the half a *site* needs

`frame-src` is the one directive in that header that is a grant rather than a
narrowing, and it took two corrections. It started as `'none'`, which refused the
embed the page builds for a YouTube link. It then became YouTube plus its
no-cookie host — right for YouTube, and still fatal for the case the feature
exists for: a site somebody pasted. That link was refused before the request left
the process (the site itself permitted framing; our header was the refusal), and
a page cannot see a directive it was never served, so the only evidence was
`embed_unverified` on a screen showing its own idle pattern.

The media policy now carries `frame-src https:` and the strict policy — every
menu, HUD and panel — still carries `'none'`. `webui/tests/WebCoreTests.cpp`
asserts that as one sentence: the grant present in exactly one policy, and the
strict policy naming no `https:` origin at all. Plaintext framing stays
impossible. The frame is then sandboxed — `allow-scripts allow-same-origin
allow-forms allow-popups allow-popups-to-escape-sandbox
allow-top-navigation-by-user-activation allow-presentation` — because the first
two are what let a site's own player run, a click may follow that player to
another URL, and the silent redirect that would otherwise take the screen (and
the player's controls with it) cannot.

The page reports the frame's `load` event as `embed_framed`, and that line is the
difference between two failures that look identical on screen: a frame our policy
refused never navigates and never fires it, while a site that refuses to be
framed (`X-Frame-Options`, `frame-ancestors`) still arrives as an error page and
does. It means "a document was allowed to arrive" — never "there is a picture",
which stays unobservable from here.

`webhost/tests/WebHostTests.cpp` proves the pair inside the real host: the same
page served to two surfaces, one per policy, each reporting which refusal it hit
(`{"violation":"","load":true,…}` against
`{"violation":"frame-src","load":false,…}`). The pair is the point — a probethat can only ever answer "blocked" would satisfy a media-only assertion and mean
nothing.

### The link that refuses to be framed, and the popups

**`webui/include/op77/WebUI/FramePolicy.hpp`** and
**`webhost/src/FrameResolver.{hpp,cpp}`** (both copied whole into `native/`),
**`webui/include/op77/WebUI/AdBlock.hpp`** (likewise), and their uses in
**`webhost/src/SurfaceClient.{hpp,cpp}`** and **`resources/system/open77_media/web/tv.js`**

Framing a pasted site is not enough for most pasted sites. The one this was
written against answers `x-frame-options: SAMEORIGIN` on a 5,770-byte document
whose entire content is five advertising scripts and **two iframes, both of which
probe as frameable**. The refusal is the shell, never the content — that is what
an aggregator *is* — and the embedder cannot observe it: a refusal arrives as an
error page, so the page's own `load` event fires either way and the screen shows
nothing with no error to read.

So the question is asked from **outside**, by the host, which is the only side
that can read response headers. `/op77/web/frame?u=<link>` is a third same-origin
route beside the transcode ones, and it is answered **off CEF's IO thread** — a
probe that blocks the thread dispatching the page's own files stalls the browser
it is answering. It reads headers, and the markup only when framing was refused,
and it **sends no cookies**. It is a probe and not a proxy: the app it finds is
then framed directly by CEF with the site's own origin and session. The page asks
before it builds a frame, and a late answer to a link the player has already
replaced is dropped rather than framed (`siteToken`).

The ad blocker is the same seam seen from the other side. `OnBeforePopup` was
unimplemented, so every window a page asked for was created — and painted as an
overlay, since this host implements `OnPopupShow`/`OnPopupSize`. The rule that
separates an advertisement from a player is the **user gesture**: a window with no
click behind it is advertising by definition, while a window a click asked for is
followed *in place*, because a television has no tabs and a player that opens its
video in a popup has to play somewhere. The blocklist is enforced twice more
where a window rule cannot reach — cancelled **requests** and refused
**navigations**, which is what stops the silent redirect that otherwise eats the
page.

`webui/tests/WebCoreTests.cpp` pins the framing verdicts (including two CSP
headers where both apply), the dot-segment URL resolution, and the four popup
rules — with `googlevideo.com` pinned as **not** blocked while
`googlesyndication.com` is, because a blocker that eats the stream is worse than
no blocker. `WebUiAssetTests.cs` pins the wire itself: the route string the host
serves and the page asks for, the branch that frames the host's answer, and the
fact that the judgement comes from the tested header rather than being
re-implemented in the host's C++.

## 6b. Audio

**`webhost/src/AudioSink.{hpp,cpp}`** (copied whole into `native/`)

Browser audio into the game's mixer: the host stream-copies or resamples the
interleaved audio it receives into the device, and reports the peak it saw, which
is how "the page is playing" is told apart from "the page is playing *silently*"
in the self-test. `webhost/src/WebHostApp.cpp`, `SelfTestApp.{hpp,cpp}` and
`webhost/CMakeLists.txt` carry the rest of the wiring, including the media
capability probe that answers "what can this build actually play" with
measurements rather than assumptions.

## 6c. The decoder, for links this build cannot play

**`webui/include/op77/WebUI/TranscodePlan.hpp`** and **`Decoder.hpp`** (copied
whole into `native/`), **`webhost/src/HostRuntime.{hpp,cpp}`**,
**`webhost/src/Main.cpp`**, **`webhost/src/SurfaceClient.{hpp,cpp}`**,
**`client/src/webui/WebUiService.cpp`**

This Chromium ships no H.264 and no AAC decoder, so an `.mp4` or an HLS playlist
is a black rectangle handed to a `<video>` element. The host can decode it, and
the seam is small because the page keeps its own element:

* `/op77/media/probe?u=…` runs `ffprobe` on the link and answers with the
  container, the codecs and a verdict — `playable`, `transcoded`, `disabled` or
  `nothing`. It is served by `SurfaceClient::GetResourceHandler`, the same
  handler that serves the page's own files, so it is same-origin, **in-process
  and socket-free**: no listener, no port, no token, nothing reachable from
  outside the browser that is rendering the page.
* `/op77/media/stream?u=…` runs `ffmpeg` and answers with VP9/Opus WebM. The
  page's element gets the *route* as its `src`, so transport, volume and the
  seek bar are the same code paths a native WebM uses; the duration is unknown
  until the stream declares one, and the bar disables itself rather than lying.

Two things about that argv are worth keeping, because both were measured as
failures and both are silent: `-dash 1` fails the WebM header and writes **zero
bytes**, and an unescaped comma inside the scale filter terminates the
filtergraph. `webui/tests/TranscodePlanTests.cpp` pins the strings;
`tests/DecoderE2ETests.cpp` runs the real `ffprobe`/`ffmpeg` over real HTTP and
skips (rather than fails) when the tools are not staged next to the test.

The decoder tools live in `<plugin>/decoder/` and the path reaches the host as a
launch switch — `Main.cpp` parses it, `HostRuntime` holds it, `WebUiService`
passes it at host start, and a host started without it answers `disabled` with
the reason instead of showing a black rectangle. Nothing here helps a DRM
service, and nothing can: the key is never handed over.

## 7. The suites

Three Lua suites, and the order matters:

* `open77_media / records` — the catalogue. `tools/lua-test/run.lua` preloads
  `open77_admin/shared/config.lua` **before** `shared/records.lua`: the suite
  cross-checks every record against the admin prop-model aliases.
* `open77_media / placement` — where "left" points on a set that is not
  axis-aligned. In game a wrong answer is a cabinet that slides the wrong way
  while the operator holds the button, with no log line anywhere, on a set that
  may be kilometres away.
* `open77_media / client` — which screens a client materialises and what it
  releases when the session ends. Runs **last**, deliberately: it installs
  process-wide `Open77`, `CreateThread` and `Wait` stubs so the resource can be
  loaded outside the game, and a suite that ran after it would see those instead
  of nothing.

And two C# ones:

* `tests/MediaRecordsTests.cs` — the catalogue suite inside the C# test host,
  plus the check that every `Open77.…` name the resource uses is published by the
  runtime that file actually runs in. That check **derives** both surfaces from
  the registration sites (`scripting/src/ResourceHost.cpp` and the server's Lua
  prelude) rather than carrying a hand-written list, and it is per-runtime — the
  two surfaces differ, and the bug it was written for was a name used on the side
  that does not have it.
* `tests/MediaPlacementIntegrationTests.cs` — the placement path through
  `ServerResourceHost` and a real `PropAuthorityService`: spawn, nudge, turn,
  then assert on the registry the replication layer reads *and* on the resource's
  own report of where the set is. The refusal cases are the control (an invented
  direction and a negative distance must leave the prop exactly where it was),
  and two sets are used so that "the one that moved" is never assumed.
* `tests/WebUiAssetTests.cs` — the wire between the page and the host's frame
  probe. Three files in three languages and no way to run any of them without a
  game, so what is pinned is the pair of strings that has to agree
  (`/op77/web/frame`), the branch that frames the host's answer, and the fact
  that the judgement comes from `WebUI::Framing` rather than being re-implemented
  in the host's C++. This one exists because its failure is silent in the worst
  way: if the page stops asking, every shell-shaped site goes back to showing
  nothing, with no error anywhere.

`tools/run-suite.py` in this repository runs all three Lua suites with no
monorepo: it preloads the vendored alias snapshot (or a live checkout with
`--from`), stages the resource in a temp directory for the client suite's
`OPEN77_REPO_ROOT`, and fails if any suite reports zero assertions as well as if
one fails.

Four C++ suites live here too, and each one exists because its subject fails
silently in game: `tests/ScreenQuadTests.cpp` (the quad arithmetic and the
facing gate), `tests/ScreenMotionTests.cpp` (the two-sample blend, including the
staleness refusal), `tests/TranscodePlanTests.cpp` (the probe verdicts and the
decoder argv, down to the quoting `CommandLineToArgvW` has to accept) and
`tests/DecoderE2ETests.cpp` (the real decoder over real HTTP: probe answers,
the stream is VP9/Opus WebM, `-ss` seeks). The frame grant is asserted twice on
purpose — as directives in the policy test, and as behaviour in
`patches/webhost__tests__WebHostTests.cpp.diff`, where the real host, the real
header and real Chromium are all in the loop.

Three more live in `patches/` rather than as copies, because their files carry
other work: the framing verdicts and the four popup rules in
`webui/tests/WebCoreTests.cpp` (pure — `googlevideo.com` must **not** be blocked
while `googlesyndication.com` must), the resolve stage in
`webhost/tests/WebHostTests.cpp` (real WinHTTP to the real origin, then the app it
found actually framed), and the single-pointer rules in the client's own CTest
targets.

---

## The two things that bite

### A resource that exists is not a resource that loaded

A resource is discovered at **startup** and by an explicit `refresh`. The
automatic tree rescan is opt-in via `resources.watchIntervalMilliseconds`, and if
it is absent or zero, adding a directory changes nothing at all.

The failure is silent in the worst way: the server is healthy, players connect,
the resource's files are on disk, its name is in the config — and the resource is
not running. There is no error line, because nothing went wrong.

Check the server's own log for the resource announcing itself. `open77_media`
prints on load:

```
[resource:open77_media] discovered
[resource:open77_media] open77_media ready -- 36 television records
[resource:open77_media] started generation=1
```

No such line means it is not loaded. Fix by restarting the server, or — with a
running server and no restart — by running the console command `refresh`, which
re-runs discovery and starts everything marked `auto_start`, and republishes the
client resource set.

Adding `open77_media` to the config **and** starting the server before the
directory existed is exactly how this was hit: the config said one thing, the
running process had read another, and the fix was a restart, not a code change.

### The tab is in `freeroam`

With the resource loaded and the catalogue answering, a player can still see no
TV tab — because the tab ships in the `freeroam` gamemode, not in the
television resource. Deploying a new resource does not deploy a changed
gamemode.

Verify by file, not by assumption:

```bash
# on the server, against a checkout
diff <(cd <checkout>/resources/gamemodes/freeroam && find . -type f -exec md5sum {} +|sort) \
     <(cd <server>/resources/gamemodes/freeroam   && find . -type f -exec md5sum {} +|sort)
```

`freeroam` reloads itself when its files change and the watcher is on; the log
line to look for is `[resource:freeroam] reloaded generation=N`, followed by
`Resource set published generation=N`.

## Proving it without launching the game

Everything below is reachable with the server running:

```bash
# is the resource in the signed set the client downloads?
curl -s http://<server>:11802/resources/v1/set | python -m json.tool | grep -o open77_media

# does the package the client would fetch contain the page?
#   /resources/v1/packages/<digest> is CBOR; the file paths appear verbatim
curl -s http://<server>:11802/resources/v1/packages/<digest> | strings | grep web/tv
```

A resource's client package carries `client/`, `shared/`, `web/` and declared
`files` — **not** `server/main.lua`. That is the design, not a packaging bug, and
it is worth knowing before you go looking for the missing file.

The in-game render is the one step that needs a launch: spawn a set from the TV
tab and look for `MediaScreens: bound screen … prop=… surface=…` in the client
log, then `player_state (playing at Ns)` from the page.

### The failure that is not a failure

Two states look identical in a log and are worth separating before you spend an
afternoon on either:

* **`not drawing -- occluded`** with `facing: undeclared`. The record does not say
  which side its picture is on, so the screen is drawn from both — which is what
  "the television is playing on both sides" actually is.
* **`not drawing -- occluded`** with `facing: behind`. You are standing behind
  the cabinet. That is the gate working.

The client logs the reason whenever it changes, so the first line that differs
from the previous one is the answer.
