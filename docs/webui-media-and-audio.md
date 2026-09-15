# What the in-game TV can actually play — measured (2026-09-14)

Research note for the CEF "TV / url player" feature. Everything below was measured
against the libcef build this repo ships (`cef_binary_144.0.32+g5ce7d26+chromium-144.0.7559.258_windows64`),
on the development machine, with the game closed. No part of it is read off a build
config.

The question it answers is not academic: the feature promise was "plays youtube videos
netflix any link". Two of those three are impossible in this runtime, and a TV that
silently shows a black rectangle for half of them is worse than one that says why.

## How to reproduce

```bash
# codec / DRM / reachability probe (page reports; host writes the file)
artifacts/Release/web/Open77.WebHost.exe \
  --open77-self-test-probe=<out>.json \
  --open77-self-test-url=http://127.0.0.1:<port>/index.html

# media playback probe: a VP9+Opus sample and an H.264/AAC sample, six seconds of
# real playback, page report on the first line and host PCM observation on the second
artifacts/Release/web/Open77.WebHost.exe \
  --open77-self-test-probe=<out>.json \
  --open77-self-test-url=http://127.0.0.1:<port>/media.html

# the audio sink on its own: needs neither Chromium nor the game
artifacts/Release/web/Open77.WebHost.exe --open77-self-test-audio=3
```

The probe pages and the two generated samples used for the run recorded here are the
`tone.webm` (vp9+opus) / `tone.mp4` (h264+aac) pair produced by `ffmpeg` from the same
sine and the same colour source, so codec pair is the only difference between them.

## Result

| Capability | Measured | Meaning |
|---|---|---|
| `video/webm; codecs="vp9,opus"` | `probably` | the free codec pair is present |
| VP9+Opus playback | **playing**, `readyState=4`, 6.16 s of a 634 s stream | works |
| YouTube (full player, real stream) | **playing**, `quality=medium`, `currentTime=5.97` | works |
| `video/mp4; codecs="avc1.42E01E,mp4a.40.2"` | `""` (empty) | absent |
| H.264/AAC playback | `DEMUXER_ERROR_NO_SUPPORTED_STREAMS` | refused at the demuxer, not merely "not advertised" |
| MSE `video/mp4; codecs="avc1…"` | `false` | no MSE path around it either |
| MSE `video/webm; codecs="vp9"` | `true` | the WebM path is real, not a fallback |
| HLS (`application/vnd.apple.mpegurl`) | `""` | no native HLS |
| MediaSource | `true` / `mediaSourceMp4=false` | WebM-only MSE |
| EME present | `true` | the API exists… |
| `requestMediaKeySystemAccess(widevine)` | `refused: NotSupportedError` | …but no Widevine CDM ships in libcef |
| playready | `refused: NotSupportedError` | as above |
| page reachability to YouTube | `true` | the network path is fine |

Netflix is out for two independent reasons: no CDM in this runtime, and — even with a
CDM present (Edge's `widevinecdm.dll` exists on this machine, so `--widevine-cdm-path`
is technically reachable) — Netflix gates L3 desktop playback on a VMP signature that a
CEF host does not have. Neither can be solved by configuration, and neither CDM can be
redistributed with a game that runs under EAC.

## Audio

Windowless CEF owns no audio device: it decodes and hands the PCM to the client, and a
client that returns no `CefAudioHandler` gets a page that plays perfectly and a machine
that stays silent.

The first probe run is the evidence for that, in the form of its own absence:
`OP77PROBE` reported `webmState=playing` with **no `OP77AUDIO` line at all**, because the
probe wrote its report and closed the browser ~1 s in — before any audio thread had
delivered a packet. Holding the session open through six seconds of real playback
produced, on the same build and the same page:

```
OP77AUDIO:{"streams":1,"stops":0,"packets":263,"frames":269312,"channels":2,
           "sampleRate":44100,"peak":0.062833,"error":""}
```

263 packets × 1024 frames = 269,312 frames = 6.1 s of stereo 44.1 kHz float PCM, and the
peak is `0.0628` — the generated 0.125-amplitude sine at the page's `volume = 0.5`,
exactly. So the page's volume and mute controls are honoured *in the PCM*, not merely
stored.

The sink that plays this PCM is `webhost/src/AudioSink.cpp`. Its self-test:

```
Open77 audio self-test: device="Speakers (Echo Dot-JBM)" rate=44100 channels=2 float32
Open77 audio self-test: ok (pushed=132096 played=145971 expected~132300)
```

`pushed` matches wall clock (3.0 s at 44.1 kHz); the surplus `played` is the 100 ms
silence the device is primed with plus the 300 ms drain after the last push, which is
also what the residual `underrun` counts. Steady-state underrun is zero.

One design detail worth keeping: a shared-mode WASAPI client **cannot choose its own
sample rate**, so the device format is probed first and handed back to CEF from
`GetAudioParameters`. Answering with CEF's 44.1 kHz default on a 48 kHz device would
play every page fast and sharp.

## Decisions taken with the user

* **Ship the WebM/YouTube path.** YouTube, Vimeo and any VP9/WebM+Opus source work with
  sound. H.264 MP4 links and DRM services are refused *with a reason shown in the TV UI*
  instead of a silent black screen. No proprietary-codec build, no CDM, nothing to
  license or redistribute.
* **Audio is produced in the webhost process.** One `AudioSink` per surface, so each TV
  has its own gain and mute and the audio engine mixes them on one device. World
  positioning (falloff with distance from the TV) is a separate, later milestone: it
  needs the PCM in the game process, not just the host's.

## A refusal that is NOT a property of the runtime: YouTube error 153

The television page can be driven outside the game — `open77_media/web/tv.html` with a
small stand-in for the `Open77` bridge, served over `http://127.0.0.1:<port>` — and in
that browser the YouTube embed refuses with

```
Error 153: "Video player configuration error"
```

This is worth recording because it points the wrong way. 153 is a *referrer* complaint,
and the obvious reading is "the embed needs a referrer the game page does not send, so
YouTube can never work on a television". Three things were measured before believing
there is anything to fix:

* Removing the `origin=` parameter from the embed URL changed nothing (still 153).
* Pinning the referrer with `<meta name="referrer" content="unsafe-url">` changed
  nothing in that browser either.
* The **host's own CEF build plays the same video** from the same kind of origin —
  `youtubeProbe` in `evidence/webui-media-2026-09-14.json`: `isPlayable: true`,
  `state: playing`, `currentTime 5.97`, `quality: medium`, on a page served from
  `http://127.0.0.1:<port>`.

So 153 is this automation browser refusing the player, not a limit of the runtime or of
the page. `tv.html` still pins `unsafe-url` — not as a repair, but so the referrer is the
page's decision (and a constant) rather than the surrounding browser's, since the page now
reports the refusal verbatim and an unpinned variable would make that report ambiguous.

The lasting fix is on the reporting side rather than the player side: the page originally
announced `playing` the moment it set an `src`, which is a claim about something it had not
observed. It now asks the player and repeats the answer — `loading` until the player says
`playing`, and `youtube_error <code>: <meaning>` (with the code's own text, 101/150
"embedding disabled" and 153 included) if it refuses, on screen and in the resource log.
A television that cannot show a link says which link and which reason.

## The refusal a page cannot report: the page policy (2026-09-14, live session)

A television was spawned, its screen drew every frame, and the link put on it never
played. The resource log said, in order:

```
television 3: loading (youtube embed (player api))
television 3: youtube_api_failed (the player script refused to load)
television 3: loading (youtube embed (no player api))
television 3: youtube_loaded (8scL5oJX6CM)
```

and then nothing: no `player_state`, no error, no picture — the set sat on the page's own
idle colour bars. Every one of those lines is the page reporting the *consequence* of a
header it was never allowed to read. The header was

```
default-src 'self'; img-src 'self' data:; media-src 'self';
style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline';
connect-src 'none'; object-src 'none'; frame-src 'none'; base-uri 'none'
```

which the web host wrote for every resource page. `script-src 'self'` refuses
`https://www.youtube.com/iframe_api`, and `frame-src 'none'` then refuses the bare embed
the fallback builds — so the fallback reported `youtube_loaded` off the *blocked* frame's
error page. Reproduced outside the game with `_tvpolicy/probe.py`, which serves the same
page under either policy, the browser says it in one line:

```
[error] Refused to load the script 'https://www.youtube.com/iframe_api' because it
        violates the following Content-Security-Policy directive:
        "script-src 'self' 'unsafe-inline'"
```

and under the media policy the same page reports `player_state playing at 11.7s` and
`playing (youtube embed)` with the frame on screen.

The fix is a closed set of client-owned policies (`webui/include/op77/WebUI/
PagePolicy.hpp`): every page is served `Strict` unless its surface definition asks for
`Media` by name, and the directives behind each name are edited in exactly one file. A
resource may choose a policy; it can never write one. The lesson worth keeping is the
shape of the failure, not the two directives: **a page cannot report a policy it was
served under**, so "the script refused to load" has to be checked against what the host
was willing to serve before anything on the page is changed.

## Handing the pointer and the keyboard to a screen in the world (2026-09-14)

A television shows a *page*, and a page is worth nothing on a quad if it cannot be
clicked. Two facts make that harder in the world than in a menu.

**CEF rasterises the page into a texture and the HOST paints the cursor.** A menu
surface has a DOM cursor drawn from the mouse event CEF produces, which is why the
menu's pointer needs no work. A page on a world quad has no such thing: nothing draws
a pointer into the picture, so the client does, as one `WorldOverlay::Style::Dot`
interpolated across the same four world corners the picture is composited with — in
texture space, so a panel that is mirrored or turned over is not a special case.

**Cyberpunk pins the system cursor to the centre of the screen.** The pointer therefore
cannot be read from the OS: it is accumulated from raw input, seeded once when the
session opens, and the same accumulator feeds both CEF and the drawn dot, so the dot
and the click cannot drift apart. The seam between the two halves is a fraction of the
page — `WebUiService::ScreenInput` carries it, `Api::MediaScreens` draws it — and the
state lives in a header with no platform includes because the drawing side runs on the
game thread and must not pull in a window header and a graphics API to read two floats.

Taking a screen is not a second input path: it is asking for the focus the WebUI
service was already built around. That focus is what suppresses the game's raw input
(the same gate the chat key, free look and the perspective toggle already read) and what
clips and hides the system cursor, so the browser gets the mouse and the keyboard and the
player does not walk while typing a URL. The session ends with the screen that owns it —
explicitly on a world change, and on release through `MediaScreens::Release` — because a
focus that outlives the thing it was taken for is a player who cannot look around with no
television on screen to explain why.

### Two pointers, and which one is left (2026-09-15)

Players saw **two** pointers on a taken screen, and the report was exact: *"seams to be a
cursor then a dot we only need cursor"*. Both were ours, from two modules that had each
solved half the problem and never been told about each other:

* `WebUiService` sets `io.MouseDrawCursor` for any surface that is not `open77_shell` and
  positions it at the raw pointer's viewport coordinates. That is the **cursor** — the
  overlay's own arrow, at the place the mouse "is".
* `Api::MediaScreens` published the `Style::Dot` item, at the projection of the pointed-at
  page pixel onto the panel. That is the **dot** — and it is where the click lands.

The two only coincide when the panel fills the viewport, which it does not: a television
in the world is a quad somewhere in front of the camera. The dot was the truthful one
(computed from the same four corners the picture is composited with, from the same
accumulator the click is mapped through) and the cursor was the familiar-looking one, so
neither could simply be deleted without losing something real.

The fix keeps one pointer and makes it both: the panel publishes **where its quad landed**
as a viewport fraction (`ScreenInput::PublishPointerOnScreen`, called once a tick from the
same arithmetic that already placed the dot, cleared at the top of every tick so a screen
that stops being built cannot leave last tick's cursor on the wall), and the WebUI places
its **cursor** there instead of at the raw pointer. So the arrow a player aims with is the
arrow a click lands under, and there is exactly one of them. A tick with no published
position — prop unstreamed, quad refused, camera behind it — falls back to the raw
pointer, which is what happens with no session open at all.

`WorldOverlay::Style::Dot` is left in the overlay's closed set: nothing produces it now,
and removing a style that a future producer may want is a worse trade than one unused
enum value.

### The key, and the collision that chose it

The first cut bound the handoff to **F7**. That is the client's own perspective toggle
(`kPerspectiveKeyVirtualKey` in `ClientResourceHost.cpp`, which exists because
`open77_perspective` already claims F6). The two polls sit on different gates — the
perspective one is gated on the web focus, and this one is what *takes* that focus — so
the take press fell through both and the camera flipped a frame before the screen took
the keyboard. The binding is now **F8**: unbound in the base game's control map and in
this plugin, while F1/F6/F7/F10 are taken. The general rule is the one worth keeping: a
key that is read on one gate and consumed by another does two things by construction, and
that reads to a player as a broken key.

### Verifying it

The client cannot be exercised without the game, so the diagnostics carry the load.
`open77.log` records `screen input: surface N taken (pointer at u, v; page WxH)` and
`screen control refused: …` naming the reason (nothing under the crosshair, a surface
that is not presenting), and the `webui.input` debug row grew a `screen=#N u… v…` field
next to the focus it already reported. The release path that is reachable without the
game is the build: `Open77.Client` compiles, and the 35 CTest targets — including
`Open77.ScreenQuad.Tests` and `Open77.ScreenMotion.Tests`, which cover the projection
and orientation arithmetic the dot interpolates — pass.

## Cinema-scale screens (2026-09-14)

### What the drive-in actually is

The drive-in is a named location, not a prop: `base\worlds\03_night_city\sectors\
c_westbrook\japan_town\loc_sq031_drive_in_cinema` (Japan Town, Westbrook). The content
archive carries only its four `envprobe` files — the screen itself is placed by the
world's own sector data, which is not in `archive/pc/content`, so there is no asset to
point a quad at and no coordinates to read out of it*.

The film shown there is drawn by the **Ink** system, not by a texture: the quest's device
is `base\quest\main_quests\part1\q110\devices\q110_cinema_screen.ent`, and beside it ship
`base\items\quest\q110__misc\q110_cinema_screen.mesh` (measured 1.16 x 0.66 m, so the
*asset* is monitor-sized and the sector scales it) plus `q110_cinema_screen.inkwidget` —
the thing that actually plays the movie. That is a UI pipeline, and a CEF page cannot be
hosted inside it: there is no texture to write. Hence the shape of this feature — we do
not borrow the game's screen, we project our own quad where we want one.

### The large screens the game does ship, measured

Extracted with `WolvenKit.CLI extract -r '<regex>'`, serialised with the deprecated
`cr2w -s` (the `export` path refuses meshes without a configured depot), then read from
`Data.RootChunk.boundingBox`:

| mesh | size (m) | size (ft) | what it is |
|---|---|---|---|
| `billboard_screen_4x3_huge_a` | 28.635 x 0.700 x 21.477 | **93.9 x 70.5** | the biggest flat screen mesh in the content archive |
| `billboard_lighting_ext_w1400_h600_ab_frame` | 13.395 x 1.995 x 0.947 | 43.9 x 6.5 | a lighting frame, not a display |
| `billboard_screen_a_straight_w600_h800` | 5.997 x 0.090 x 8.000 | 19.7 x 26.2 | portrait panel (`w600_h800` = 6 m x 8 m: the names are decimetres) |
| `billboard_screen_9x21_huge_a.ent` | 3.600 x 0.140 x 8.400 | 11.8 x 27.6 | 9:21 portrait panel |

So the game's own largest billboard is **93.9 ft**, a 4:3 panel — close to the target and
the wrong shape for anything a person watches.

### What was built instead

A scaled **prop**, not a new asset. `build-prop-hosts.ps1` bakes one host entity per
catalogue alias, and `Open77.props.create` already accepts `scale` (passed straight
through to the entity component by `props = { create = ... }` in
`LuaResourceRuntime.cs`), while the screen quad is our own arithmetic in the prop's local
frame. So a cinema screen is a record that reuses an existing hosted model —
`electronics.tv.screen.16x9`, a bare 16:9 display plane 1.160 x 0.660 m with a declared
front — at 26.28x and 39.41x, giving **100 ft (30.48 x 17.34 m)** and **150 ft
(45.72 x 26.01 m)**. No asset build, no archive change, no EAC catalog re-sign: the same
mesh, the same host, a bigger prop.

The invariant that makes it safe is that the scale and the quad are one number written
twice, and they must agree — the engine scales the mesh, nothing scales our quad. That is
now a test rather than a comment: `records_test.lua` derives each scaled record's expected
quad from the record for the same model at scale 1 and fails if any of width, height or
the offset disagrees.

One thing that had to change with it: the spawn stand-off. A flat 1.1 m was right for
everything furniture-sized, and for the 100 ft panel it puts the *centre of the picture*
1.9 m behind the person who spawned it — they would be standing inside their own screen.
The rule is now one screen-height in front of the picture's own centre, floored at the old
1.1 m, at which every furniture record lands within 3 cm of where it already was
(`tv.large` 1.126 m, every monitor floored to 1.100 m) and the cinema lands at 20.37 m and
30.56 m. It lives in `shared/placement.lua` and is pinned by `placement_test.lua`.

Known limits, named rather than discovered: the picture is 1280 px across 30 m (**42 px/m,
about DVD**) because that is `Open77MediaSurfaceFor`'s long side, and the client stops
drawing a screen past **150 m** (`kMaximumDistance` in `Api::MediaScreens`), so the far
end of a large lot sees nothing.

\* `loc_sq031_drive_in_cinema` is also the wrong lever even if it were reachable: the
board is scenery, and the film on it is Ink.

### A paused set is not a black set (2026-09-15)

Report: *"100ft cinema screen isnt working showing black."* The session log names it, and
the screen was not the fault:

```
television 2 (cinema.100ft): drawing              <- the quad IS drawn
OK media=2 ... url=https://youtu.be/Qv1... volume=63 muted=false paused=true
television 2: player_state (unstarted at 0.0s)   <- eleven times, never `playing`
television 2: youtube_error ...                  <- none
television 3 (cinema.150ft) ... paused=false
 television 3: player_state (playing at ...)     <- the same link, on the 150 ft set
 television 3: player_state (ended at 334.8s)
```

Two screens, one link, one black and one playing; the only state that differs is
`paused`. Three things made that state permanent, and all three are ours:

* `startApiPlayer` built the player with `autoplay: 1` and then `applyPaused` immediately
  sent `pauseVideo` -- a player paused before it has ever started never leaves
  `unstarted`, and an unstarted YouTube player paints nothing at all.
* The page's own recovery refuses to touch a paused set: `watchYouTube` nudges only
  `while (!state.paused)`.
* Setting a link did not clear the pause, so re-pasting the link -- the one thing a watcher
  in front of a 30 m panel can do from the menu -- could not start it either.

Fixed in the two owners of the rule: `web/tv.js` now builds the player with
`autoplay: state.paused ? 0 : 1` and cues it (`cueVideoById`) when paused, so a paused set
shows the video's poster rather than a black rectangle, and `server/main.lua`'s `url` action
clears `paused`, because pasting a link is a play request. Pinned by
`APausedTelevisionShowsAPictureRatherThanNothing` in `WebUiAssetTests`, with a mutation
control (removing the server line fails it).

Two follow-ons the same session surfaced. The ad-block receipt handler read `os.time()` in a
sandbox that withholds `os` (`LuaResourceRuntime` removes `io`, `os`, `debug`, `package`), so
the server logged `script error: open77_media/server/main.lua:399: attempt to index a nil
value (global 'os')` and dropped the receipt on every player -- the clock is now the
runtime's own `GetGameTimer()`, and `MediaServerScriptsNameNoWithheldLibrary` fails if the
resource names a withheld library again. And `TelevisionResourceUsesRealHostApiNames` -- the
per-resource API check -- is what caught the first attempt at that fix, because the *server*
prelude publishes `GetGameTimer`, not the client's `Open77.time.monotonic`; several other
resources' `server/main.lua` files still call the client spelling, which is the same class of
name that fails silently at runtime.

## A link that is a shell, and the advertising around it (2026-09-15)

The report was two sentences about one site -- *"make this website compatible so i can play
shows on it also add an adblocker i cant even navigate these pages from random popups"* --
and both sentences turned out to be measurable facts about what such a site is.

### What the link actually was

```
$ curl -sSI https://123movie-tv.it.com/
HTTP/1.1 200 OK
server: cloudflare
x-frame-options: SAMEORIGIN        <- the whole reason nothing appeared
x-xss-protection: 1;mode=block
```

and the document behind it, which is 5,770 bytes and contains no content of its own:

```
5 x   <script src="https://focusameneducation.com/<32 hex>/invoke.js">
1 x   <iframe src="https://v3.freemovies.lol/?logo=…&brand=Soap2day&color=11c5b9">
1 x   <iframe src="https://moviestv.my/">
```

So the refusal was never about the content. Probing the two nested origins the same way:

```
https://v3.freemovies.lol/   no x-frame-options, no frame-ancestors   -> frameable
https://moviestv.my/         no x-frame-options, no frame-ancestors   -> frameable
```

A shell that refuses to be framed, wrapping two applications that are happy to be. That is
not one site's quirk; it is what an aggregator *is*, which is why the fix is a general one
rather than a rule about this domain.

### Why the page could not work this out for itself

`X-Frame-Options` is read by the browser from the response, and its refusal is invisible to
the embedder on purpose: the iframe loads an error page, `load` fires, and no line the page
can write distinguishes *"the site said no"* from *"the site is empty"*. The page had been
saying `embed_unverified` for two sessions for exactly that reason.

Outside the browser the answer is one request away, so the question is asked there -- by the
host, which is the only side that can read a response's headers.

### A probe, not a proxy

`/op77/web/frame?u=<link>` is a third same-origin route beside the two transcode ones, and
it is deliberately the weakest thing that solves the problem:

* it fetches the link's **headers** (and, only when framing is refused *and* the content type
  is markup, up to 256 KB of body);
* it never renders that body, never rewrites it, never replays it, and **sends no cookies** --
  a proxy that carried the player's session through a process a resource can talk to would be
  a much larger thing, and this is not it;
* the application it finds is then framed **directly by CEF**, with the site's own origin, its
  own cookies and its own network;
* it only exists for a media-policy surface, which is the only surface that could have framed
  anything in the first place.

It runs off CEF's IO thread (a probe waits on the network; blocking the thread that dispatches
this process's file requests behind an internet round trip is how a browser stalls), and the
page treats its failure as "no host to ask" and frames the link as it always did.

Every judgement is pure and lives in `webui/include/op77/WebUI/FramePolicy.hpp`: which
headers refuse framing (every `X-Frame-Options` value does -- `DENY` by name, `SAMEORIGIN`
because a television is never the same origin, `ALLOW-FROM` because no browser has honoured
it since 2015 -- and `frame-ancestors` unless it names `*`, a scheme source, or this
surface's own origin), how a reference resolves against the document it was written in, and
which of a page's nested documents are candidates. That last one is a single left-to-right
scan for `<iframe>`, `<frame>`, `<embed src>` and `<object data>`, honouring the page's own
`<base>`, skipping anything the blocklist refuses, and **casting no vote** on the rest of a
page's policy: the framed document runs under its own CSP, exactly as it would in a tab.

### The advertising, which is the other half of the report

Five `invoke.js` scripts whose entire job is to open windows. In a game there is no window to
open -- CEF draws a popup as an overlay on the same surface, which is what `OnPopupShow` and
`OnPopupSize` in this host exist to paint -- so what a player sees is somebody else's
advertising dropped on the film, with no title bar, no tab and no close button to get out of
it. Hence *"i cant even navigate these pages"*.

`CefLifeSpanHandler::OnBeforePopup` was simply not implemented, so every one of them was
created. It is now, and the policy it applies is in `webui/include/op77/WebUI/AdBlock.hpp`:

| the window | what happens | why |
|---|---|---|
| a blocked host | refused | a click on an advertisement is still an advertisement |
| not `http`/`https` | refused | `javascript:`, `about:`, `data:` are not window requests |
| **no user gesture** | refused | a window with no click behind it is advertising, by definition |
| anything else | **loaded in the frame that asked, in place** | a player that opens its video on a click has to play somewhere, and a television has no tabs |

The gesture rule is the reason this is a policy and not a filter: it is what separates the
five windows an on-load script opens (all refused) from the one the play button opens (kept,
in place). `OnOpenURLFromTab` -- CEF's other spelling of the same request, for
`target="_blank"` -- is decided by the same rule, because a site whose links are all
`_blank` is otherwise unusable here.

The blocklist is the second half, and it is enforced twice more where a window rule cannot
reach: `OnBeforeResourceLoad` cancels a blocked host's *requests* (a script that never loads
cannot ask for anything), and `OnBeforeBrowse` refuses a *navigation* to one -- the case with
no window and no click, where a framed site redirects the frame itself and the page behind
the advertisement is simply gone. The list is 48 host suffixes plus 12 host tokens
(`popads`, `popunder`, `adservice`, …), spelled out so every entry can be argued with, and it
is deliberately not EasyList: it is paired with the behavioural rule precisely because a
hand-maintained list is always incomplete. One entry matters more than the rest --
`googlevideo.com` is **not** blocked even though `googlesyndication.com` is, and that
asymmetry is pinned by a test, because blocking YouTube's media to block its advertising
would be the worst possible trade.

That list is compiled in, which is the right default and the wrong ceiling -- a popup network
that registers a domain on Tuesday must not have to wait for a build. So it has a second
layer, and that layer is the next section.

### Verification

Game-free, in `Open77.WebCore.Tests`: the host parsing, the suffix matching and both ways a
suffix match goes wrong (`notfocusameneducation.com`, `focusameneducation.com.evil.test`),
the `googlevideo` asymmetry, all four popup rules, every framing verdict (including two
`Content-Security-Policy` headers, where both apply and the *first* must not be read as the
answer), URL resolution with dot segments, and the shell scan against a reduced copy of the
real page's markup.

End to end, in `Open77.WebHost.IPC` -- the page's own `fetch`, the route, a real WinHTTP
request to the real origin, the verdict, the markup, and then the found application actually
framed. The last line is the one that matters; a verdict on its own would be a claim about a
page nobody showed:

```
stage=frame_grant  media_framed=1 strict_refused=1
stage=frame_resolve surface=20 json={"ok":true,"frameable":false,
  "violation":"x-frame-options","via":"embed",
  "best":"https://v3.freemovies.lol/?logo=https://123movie-tv.it.com/icon.png&brand=Soap2day&color=11c5b9",
  "candidates":1,"bestLoad":true,"bestViolation":"","note":"best-load"}
```

`bestLoad:true` with an empty `bestViolation` is a document from `v3.freemovies.lol` arriving
in the frame under our own media policy. The wire between the page and the host -- two
languages, two processes, neither runnable without a game -- is pinned separately in
`WebUiAssetTests.TelevisionAsksTheHostBeforeFramingASite`, so an edit to one side alone fails
there instead of on somebody's television.

### Limits, named rather than discovered

* **Google and Netflix still show nothing**, and this does not change that: they refuse
  framing by their own header, and Netflix additionally needs a CDM no build under EAC can
  ship. What the page now does in that case is say so -- `embed_refused` with the header that
  refused it -- instead of showing the idle pattern with no explanation.
* **A login still cannot be completed** on a site that requires one. The probe carries no
  cookies, and the framed site gets the browser's own, so a site that demands a session gets a
  fresh one; signing in is a keyboard on a television screen, which is what the F8 handoff is
  for.
* **The blocklist is a list.** A campaign that rotates through fresh domains is not caught by
  a host rule -- but its *windows* are, by the gesture rule, which is why the two are shipped
  together.

## The operator's layer: rules a server can add (2026-09-15)

Everything above describes what the *build* refuses. This describes what a **server** can add,
which is the property that makes the blocklist usable: a popup network is a domain registered
on Tuesday and seen in the log on Tuesday evening, and a television that can only be protected
by shipping a build, a signed integrity catalog and every client updating is a television that
shows advertising for a fortnight.

### One list, one owner, and it only ever adds

```
  server/config.lua          server/adblock.lua                (the grammar)
        │  seed                     │
        ▼                          ▼
  data/adblock.json  ──►  server/main.lua  ──► open77:media:adblock ──► client/main.lua
  (the live list)                                                            │
                                                       Open77.webui.blocklist│(hosts, tokens, revision)
                                                                             ▼
                                                              client plugin (WebUiService)
                                                                             │  AdBlockPolicy  (v7, new)
                                                                             ▼
                                                       browser host (HostRuntime::SetAdBlockPolicy)
                                                                             │
                                  Ads::Blocklist = compiled 48 + operator N │  validated HERE
                                                                             ▼
                                  OnBeforePopup · OnBeforeBrowse · OnBeforeResourceLoad · FrameResolver
```

Four decisions in that picture are the feature:

* **The layer ADDS.** An operator rule joins the compiled list; nothing a server sends can
  remove one. A client that accepted a list which could unblock `doubleclick.net` would be a
  client whose policy is controlled by whatever it connected to, so the direction is enforced
  in the host and not asked for by the protocol. `Ads::Blocklist::IsBlocked` consults the
  compiled list *first* and `ClearOperatorRules` can only clear the operator's half.
* **The rules are validated where they are matched.** `Ads::NormaliseRule` runs in the browser
  host, on the way in -- a client must not trust a list that arrived over a network. The same
  grammar is written a second time in Lua (`server/adblock.lua`) so an operator hears about a
  typo at the console *while they are looking at it*; the receipt is what catches the two
  disagreeing. A rule of `com` or `co.uk` is refused by name in both, because one typo
  meaning "any subdomain of my own site" must not blank every `.com` or a whole country.
* **The server owns the list; each client owns delivery.** The server validates what an
  operator types, keeps it under the resource's own `data/` directory, and pushes it whole to
  every client whenever it changes (and on join, with the screen snapshot). The client hands it
  to the host and reports back -- so "we sent it" and "it is in force" are different facts that
  the server can tell apart.
* **The push is a replacement, not a delta.** The list is small (bounded at 256 hosts + 256
  tokens on the wire, 512 in the encoder), a client applies it in one call, and a delta stream
  is one more place for a late joiner to end up enforcing something the server stopped
  believing hours ago.

### The seam, and why it is a new message rather than a file

The browser host is a separate process with no socket, no session and no identity: it cannot
ask a server anything. The one thing that *can* reach it is the client plugin, which already
owner of the pipe -- so the policy travels as protocol v7's `AdBlockPolicy` (game → host) and
`AdBlockState` (host → game), and **not** as a resource file the host might read. A file would
have meant two things deciding what the list is (the resource on disk and the host's own
parsing), no reply to report, and a policy that silently reverts if a directory is cleaned.

The protocol version constant deliberately does **not** move for this: v6 exists because
`CreateSurfacePayload` gained a *field*, which an old peer would answer by refusing every
surface, whereas a pair of new *message types* is exactly the case a peer is allowed to ignore
-- an older host falls through its dispatch default, keeps the compiled list, and the client
reports the missing receipt instead of pretending the rules are in force.

### What an operator types, and what they are told

```
> media.adblock
adblock revision 3 -- 2 host(s) + 1 token(s) from data/adblock.json
  host  newads.test
  host  tracker.bad.test
  token newads
> media.adblock.add co.uk
refused "co.uk": public_suffix
> media.adblock.add token popads
adblock revision 4: token popads (2 host(s) + 2 token(s)) from data/adblock.json
> media.adblock.status
adblock revision 4, 2 host(s) + 2 token(s)
  player 1: revision 4 enforced -- 2 hosts + 2 tokens (+60 compiled), 0 refused
```

`.reload` re-reads the *store* (so a hand-edited `data/adblock.json` is picked up) and never
the config file, which is why the two never argue: `server/config.lua` is the seed a server
that has never been told anything reads, and `data/adblock.json` -- written by every command
-- is the live list. One owner at a time, never both.

### Verification

Three layers, each pinning something the layer above cannot see.

**Pure, in `Open77.WebCore.Tests`**: normalisation against the shapes a person actually pastes
(a whole URL, a leading `*.`, a trailing dot, mixed case, a port) and every refusal *by name*
(`needs_a_domain` for `com`, `public_suffix` for `co.uk`, `empty_label` for `a..b.com`,
`invalid_hyphen`, `token_is_a_host`, `registry_label`), the subdomain-vs-substring asymmetry
(`notnewads.test` is not covered by `newads.test`), and the two invariants that make the layer
safe: clearing the operator's rules does not clear the compiled list, and a payload refusing
to encode at 513 rules or a 300-byte rule is what keeps a server from stalling a client.

**The grammar and the store, in the server suite** (`TelevisionAdBlockSuitePasses`, running
`server/adblock.lua` + `tests/adblock_test.lua` through KeraLua): 352 assertions over the
grammar, the add/remove/clear verbs, the wire bounds, the payload's refusals, and the store's
tolerance for a hand-edited file. The client half of the same grammar is deliberately *not*
pinned there -- two implementations are the point, and the receipt is what reconciles them.
The client's half of the *delivery* is pinned in `TelevisionClientSuitePasses`, against a stub
of the native surface: that a host which cannot take the rules is reported as
`webui_blocklist_unavailable`, that nothing is reported before the host answers, that a
receipt for the pushed revision is reported with its counts and its refusals, and that a host
which never answers is reported as unconfirmed at the deadline -- with the older revision it
*does* hold attached, so "not yet told" and "never answered" stay different sentences.

**End to end, in `Open77.WebHost.IPC`** -- one rule pushed to a real host, then the real
browser refusing the real origin, with the same page as its own control:

```
stage=frame_grant      media_framed=1 strict_refused=1        ← the control run, before the push
stage=adblock_receipt  revision=1 hosts=1 tokens=0 compiled=60 refused=0
stage=adblock_frame    blocked=1 json={"violation":"","load":false,"frames":0,"note":"timeout"}
stage=adblock_resolve  json={"ok":true,"frameable":false,"violation":"blocked","best":"",…}
```

The rule names `example.com`, which the compiled list does **not** refuse (asserted in the
test, so the stage cannot quietly turn into a measurement of the build). The first media
surface framed that origin and reported `load:true`; the third -- the same page, same policy,
one pushed rule -- never loaded it, and the host's own trace says why, which is the part a
screenshot cannot: `adblock_policy:1:1 hosts + 0 tokens + 60 compiled` followed by
`adblock:navigation https://example.com/`. The fourth surface asks the *resolver* about the
same origin and gets `"violation":"blocked"` -- the verdict the television page uses to
decide, before it frames anything, that there is nothing to frame.

### Limits, named rather than discovered

* The compiled list cannot be *narrowed* by a server, which is deliberate (above) and does mean
  that a false positive in the build needs a new build. The operator's layer is the mitigation
  for the other direction only.
* Rules are hosts and host-substrings, not paths or regexes: `Ads::IsBlocked` reads the
  authority and nothing else, and the payload cannot express more than that. A campaign
  addressed by path is out of reach of this layer, not hidden by it.
* The list is pushed on join and on change. A client that joins a second *before* an operator's
  change gets it at the next push, and a client whose plugin predates the message reports
  `no_blocklist_receipt` -- which `media.adblock.status` shows per player, so "nobody confirmed
  it" cannot be mistaken for "it is live".

## A television's own cookie jar (2026-09-15)

A television could not stay signed in to anything, and the reason was structural rather than a
page defect: every surface was created with a `CefRequestContext` and an **empty**
`cache_path`.

`cache_path` is how CEF selects a storage partition, and an empty one selects the **global**
partition -- the host's own profile directory, the `root_cache_path` that `Main.cpp` points at
the same `--open77-data` tree the host uses for everything else. Three consequences, all of
them wrong for a player:

* every television in the world shared one jar, so signing in on the screen in your apartment
  signed you in on the one in the bar, and two players signing in to two accounts of the same
  site overwrote each other;
* the cookies landed in the host's profile, beside the host's own data, rather than in a place
  owned by the television that earned them;
* `persist_session_cookies = false` was already set, which keeps *session* cookies in memory,
  but does nothing about the global partition's persistent cookies reaching the disk.

### What a jar is now

`CookieJar.hpp` (`op77/WebUI`, header-only and CEF-free so a game-free test can pin it) decides
the policy; `SurfaceClient::CreateBrowser` applies it:

* **per television** -- the path is `cookies/<resource>-<surface>`, so the pair (resource,
  surface id) *is* the identity. A generation change keeps the jar; a different television
  never sees it.
* **beside the profile, not inside it** -- under the host's data root, in a directory the host
  never reads and only ever hands to CEF as a path. The location also matters for a second
  reason: the client passes `--open77-data` as `<plugin>/cache/web/instances/<pid>`, under a
  `cache/` tree the EAC integrity catalog already excludes, so a jar cannot become an
  integrity failure at launch.
* **session-scoped** -- `persist_session_cookies` stays off, so the cookies a sign-in relies on
  are never written at all, and `HostRuntime::Start` sweeps the whole `cookies/` directory
  before the first surface exists. Nothing signed in during a previous run of the host reaches
  into this one.
* **dropped with its surface** -- `OnBeforeClose` releases the context and removes the jar, so a
  despawned television leaves no signed-in cookie store for the next surface to reuse the id.

The resource name is *input* -- it arrives inside a `CreateSurface` payload -- so it is reduced
to a single path-safe component before it becomes a directory name (`..`, `/` and `\` become
`_`, leading dots are dropped, the length is capped). Without that, a resource named `../..`
would have put a swept-and-deleted directory outside the directory meant to hold jars.

### Verifying it

* `Open77.WebCore.Tests` -- the naming, the traversal reduction, the two-directories property,
  and `Sweep`/`Remove` against the real filesystem: one television's jar is removed while its
  neighbour's survives, and the sweep does not reach outside `cookies/`.
* `Open77.WebHost.IPC` -- against the real host and real CEF, the line
  `stage=cookie_jars one=open77_webhost_test-1 two=open77_webhost_test-2`, and the assertions
  behind it: both surfaces were given a jar, the two are different directories, and their
  parent is the cookies root rather than the host's profile.

### Limits, named rather than discovered

* The jar is the host's half of the property. That CEF then keeps each surface's cookies in the
  partition its `cache_path` names is CEF's contract for that field; the test proves the paths,
  not the cookie exchange inside them.
* A jar is per surface, so a *respawned* television -- a new surface id -- starts signed out.
  That matches "stay signed in while it is on screen"; making a sign-in survive a respawn means
  keying the jar by resource instead, a one-line change in `NameFor`.
* Nothing here makes a site that refuses a non-standard user agent, or that decides to trust
  the browser on more than cookies, work. This removes one reason a sign-in could not last; it
  does not vouch for any particular site.

## The `sandbox` attribute that stopped the players (2026-09-15)

Report, from a session: *"not being able to play videos on the website hdtoday or 123movie
because of sandbox issues — it says it cant play in sandbox."*

The message is the player's own, and the gate behind it is small enough to quote. The player
these sites ship (`vidsrc.buzz/embed/...`, reached from `123movie-tv.it.com` by way of
`v3.freemovies.lol`, and from `hdtoday` by way of `moviestv.my`) decides whether it is allowed
to play like this:

```js
function opaqueOrigin(){
  ...
  try{ window.localStorage.getItem('_sb'); }catch(e){ return true; }
  try{ var d=document.domain; document.domain=d; }catch(e){ return true; }
  return false;
}
function sandboxVerdict(){
  if(opaqueOrigin())return 'sandboxed';
  ...
}
var SB = sandboxVerdict();
if(SB==='sandboxed'){ sandboxDeny(); return; }   // stopMsg('Sandbox is not allowed', ...)
```

`document.domain = document.domain` is a no-op on an ordinary page, and **Chromium refuses it
inside any sandboxed frame** — including one whose sandbox grants `allow-scripts
allow-same-origin`, which is what this page's frame carried for every framed site. The site's
own verdict was therefore `sandboxed`, and it replaced the film with "Sandbox is not allowed /
Remove the sandbox attribute from the iframe to play the video."

### Measured, both settings, through this host

A page served from one local origin framed a second one whose document evaluates those two
functions verbatim (they use nothing site-specific), and the host's media probe
(`--open77-self-test-probe`) wrote back the verdict it computed:

| frame | `document.domain` | `localStorage` | site's verdict |
|---|---|---|---|
| `sandbox="allow-scripts allow-same-origin allow-forms allow-popups …"` (what `tv.js` set) | `throw:SecurityError` | ok | **`sandboxed`** → refuse |
| no `sandbox` attribute | `ok` | ok | `unknown` → play |

`frameElement` is `null` in both rows — which is what a cross-origin parent always gives, so
the ancestor walk cannot be the verdict either way — and the attribute's *only* effect was the
`document.domain` refusal. The player's second gate, a popup probe on the first trusted click,
is disabled on these sites by their own state (`"pp":false` in the page's serialised state),
so it is never reached; the host refusing popups (measured: `window.open('about:blank')`
returns `null`) does not therefore matter here.

To reproduce: serve any two-origin pair of pages, frame the second from the first with and
without the attribute, have the inner page evaluate `opaqueOrigin()`/`sandboxVerdict()` and
report on the console channel, and run
`Open77.WebHost.exe --open77-self-test-probe=<out>.json --open77-self-test-url=<outer>`.

### The fix, and what actually bounds the frame

`buildEmbedFrame` in `resources/system/open77_media/web/tv.js` sets no `sandbox` now, and the
three host-test fixture pages that framed a site the same way were brought in line. What bounds
a framed site is the surface's page policy, the request blocklist and the popup policy — none
of which needed to change — plus the origin itself, which is what the attribute was never what
enforced anyway. `AdBlock.hpp`'s claim that the sandbox was the boundary is corrected there.

## What this note does not cover

This note was written as the browser half only — what the runtime can play, and the audio
path that makes it audible. The in-world half has since been built and lives in
`resources/system/open77_media`: a spawnable TV prop whose records carry the screen quad,
placement and nudge controls, and the catalogue the freeroam menu serves. What that half
still owes is recorded there rather than here, so that this note keeps to what was
measured on the surfaces it names.
