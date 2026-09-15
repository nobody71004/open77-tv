# open77-tv

A web browser on a spawnable prop's screen, for Open77.

Spawn a television, give it a URL, and it renders inside the game world on a quad
attached to that prop — visible to everyone who can see the prop, with transport
and audio controls. The page is a real Chromium surface (CEF) hosted by the client
plugin, so it is not a texture the server pushes: it is a browser the world
happens to be looking at.

```
open77_media/          the resource -- this is the television
  open77.lua             manifest: permissions, the page, what ships to clients
  server/main.lua        authority: which screen exists, on which prop, showing what;
                         `media.move` / `media.rotate` nudge and turn a set
  server/config.lua      the operator's ad-blocklist seed, and where their edits are kept
  server/adblock.lua     the rule grammar a server pushes; the browser host validates
                         every rule again before it enforces it
  client/main.lua        binds a surface to a prop, keeps the quad on it, decides
                         which sets are worth materialising, releases pages on exit
  shared/records.lua     the catalogue: 38 records -- the game's real televisions,
                         monitors, panels, frames, a security monitor, and a
                         scaled cinema family (a 100 ft and a 150 ft 16:9 panel) --
                         each with a quad measured from its own mesh (see below)
  shared/placement.lua   where "nudge it left" points, as arithmetic
  web/tv.html|css|js     the page one television shows: YouTube's own player, a
                         framed third-party site, or the host's decode route for a
                         link this CEF build cannot play itself
  tests/                 four suites: records (1572 assertions), placement (114),
                         adblock (351), client (58)
native/
  MediaScreens.hpp|cpp   the host-side module: bind a surface to a prop, project
                         its screen quad, publish overlay items, line-of-sight test
  ScreenQuad.hpp         the quad arithmetic and the declared-front (facing) gate
  ScreenMotion.hpp       blending the last two projected corner sets at the frame
                         being presented, so a moving set's picture does not step
  PagePolicy.hpp         which page may load what -- the CSP directives and the
                         host's own request gate, as one decision in one place
  TranscodePlan.hpp      what to do with a link this build cannot decode: the
                         probe/stream decision and the decoder argv that carries it out
  Decoder.hpp            running the decoder tools: probe, stream, seek, cleanup
  FramePolicy.hpp        whether a pasted link may be framed at all, and which
                         page to frame instead when the link is a shell that
                         refuses framing around the app that really plays
  FrameResolver.hpp|cpp  the WinHTTP probe that answers that: response headers,
                         embed discovery, and the best page to frame
  AdBlock.hpp            the popup/navigation/request policy -- a window with no
                         click behind it is advertising, and a click is followed
                         only when the window stays inside the site that asked
  ScreenInput.hpp        which world screen holds the pointer and keyboard, and
                         where on that screen the page pointer has landed
  AudioSink.hpp|cpp      browser audio into the game's mixer
patches/                 TV-only hunks of the host-side seams (see docs/integration.md)
docs/
  integration.md         every seam, what it does, and the gotcha that bites
  webui-media-and-audio.md  what this CEF build can and cannot actually play
tests/
  MediaRecordsTests.cs          the catalogue suite inside the C# test host
  MediaPlacementIntegrationTests.cs  the placement path through the real resource
                                host and the real prop registry
  ScreenQuadTests.cpp, ScreenMotionTests.cpp   the two pure host modules
  TranscodePlanTests.cpp, DecoderE2ETests.cpp  the decode decision, and the real
                                decoder over real HTTP (skips if ffmpeg is absent)
  WebUiAssetTests.cs            the wire between the page and the host's frame
                                probe, pinned across the three languages it spans
  fixtures/                     a snapshot of open77_admin's prop-model aliases
tools/
  suite-runner/          run the four Lua suites and maintain the vendored
                         snapshot, with no Lua interpreter (KeraLua)
  run-suite.py           the same four suites through a real lua5.4
  extract-tv-patches.py  regenerate patches/ from a checkout
```

## The catalogue is measured, not estimated

The first version of this catalogue reached for whatever props already had host
entities -- signage, a painting, a vending machine -- and sized their screen
rectangles by eye. It spawned, and it was honest about what it was, but none of it
was a television: picking "Framed panel" put a web page on a painting.

These records are the game's own screens. Every `model` is a 2.31 mesh from the
cooked-archive inventory, and every quad is that mesh's **measured bounding box**:

```
WolvenKit.CLI extract -gp <game> -r '.*(television_|screen_device_|smart_frame_|...)' -o work -q
WolvenKit.CLI convert serialize work -o json -q      # read Data.RootChunk.boundingBox
```

Two shapes occur and they are quoted differently. A *screen* mesh is the display,
so the rectangle is the whole mesh and `offset` is where its geometry sits inside
its own origin (0.42 m up the television's body, for the 16:9 set). A *body* mesh
with a separate screen mesh beside it -- `television_a_16x9` plus
`television_a_16x9_screen_a` -- spawns the body and places the **screen mesh's**
box on it, which is only valid while the two share an origin; the suite asserts
containment so a re-authored asset cannot quietly paint outside the cabinet. A few
meshes are *housings* with no screen submesh at all (`tv_large_a`, the device
panels, the security monitor) and there the rectangle is the measured front face,
labelled as such rather than dressed up as a screen measurement.

### Aspect ratio is per record

A surface is one CEF surface mapped 1:1 onto the quad with UVs 0..1, so a surface
of the wrong shape stretches the picture. The game's real screens are 16:9, 21:9
(2.39), 4:3-ish, 2.35 ultrawide, 2:1, square, and portrait 9:16 / 3:4 / 9:21, so
the surface is derived from the rectangle by `Open77MediaSurfaceFor` -- long side
1280, short side rounded to the nearest even pixel -- and the client calls it with
the quad it was actually sent, so `media.quad` retunes the surface too. A 21:9
screen showing a 16:9 video letterboxes it, which is what a 21:9 screen does.

`server/main.lua` creates a television at volume 75 (`DEFAULT_VOLUME`), and the
page starts at the same 75 so the slider and the mixer agree from the first frame.
A pasted link auto-detects and plays: the page puts `autoplay=1` on the embed, and
the CEF host runs `--autoplay-policy=no-user-gesture-required` because the URL is
set by the *server* and the player looking at the set often cannot click it.

### A scaled screen is one number written twice

The `cinema.*` records are the catalogue's bare 16:9 display plane at a large
scale rather than a re-authored asset: `cinema.100ft` is 30.48 x 17.34 m and
`cinema.150ft` is 45.72 x 26.01 m, both exact 16:9, and both are priced the same
way a prop is -- the engine scales the **mesh**, and nothing scales our **quad**,
which is our own arithmetic. Write one of them wrong and a 1.16 m picture hangs in
the middle of a 30 m panel. `records_test.lua` therefore *derives* each scaled
record's expected quad from the same model's record at scale 1, so an edit to
either half fails in the suite instead of on a cinema screen in front of an
audience.

The other half is where it lands. The spawn stand-off used to be a flat 1.1 m in
front of the player, which for the 100 ft panel puts its centre almost 2 m
*behind* you -- standing inside your own screen, looking at the back of it. The
rule is now one screen-height in front of the picture's own centre, floored at the
old distance, and it lives in the tested placement module: every furniture record
lands within 3 cm of where it used to (`tv.large` 1.126 m, monitors floored to
1.100 m), while the cinema lands at 20.4 m and 30.6 m.

`streamingRadius` is set to 300 m and 400 m on those two so a panel that large
does not stop replicating at the default distance, and the client stops drawing a
screen past 150 m -- so on a big lot the far end of the audience sees nothing. The
picture is also 1280 px across 30 m: about DVD, because that is the surface long
side.

### A record only spawns once its host entity has been built

This is the part that bites. An alias in `Props.cpp` is not a spawnable prop until
the asset build emits `cyberm\entities\props\open77_prop_<slug>.ent` for it, and a
record whose host is missing does not fail -- the prop creation falls back to the
marker mesh, so you get a floor decal with a page stretched over it.

So adding a record is two steps, in this order:

```
pwsh scripts/build-prop-hosts.ps1 -WolvenKitCli <cp77tools> -GameRoot <game> \
     -OutputRoot <pack tree> -WorkRoot <work> -OnlyAlias <alias,alias,...>
# then a full build-assets.ps1 run to pack Open77.archive
```

and `EveryCataloguePropHasAGeneratedHost` in the server suite fails until it has
been run (`docs/generated/prop-hosts.json` is the asset build's own manifest).

## What actually plays

Measured against the CEF build the client hosts, not assumed — the full figures
and the method are in `docs/webui-media-and-audio.md`:

| Source | Result |
|---|---|
| YouTube (`watch?v=`, `youtu.be/`, `/shorts/`, `/live/`, `/embed/`) | works, and is controllable — it serves VP9/AV1 to a Chromium that says it has no H.264 |
| WebM / Ogg (VP9 + Opus, Vorbis) | works, with seek |
| Vimeo | works as an embed |
| plain `.mp4` / `.m4v` / `.mov` | **refused** — no H.264/AAC decoder in this build |
| `.mp3` / `.m4a` / `.aac` | **decoded by the host** through the transcode route, like `.mp4` |
| a link this build cannot decode (`.mp4`, `.m4v`, `.mov`, `.m3u8`, `.mpd`, `.ts`, `.flv`, `.mkv`) | **the host decodes it** — `/op77/media/probe` asks `ffprobe` what the link is, and `/op77/media/stream` hands the page the same link re-encoded to VP9/Opus WebM. The page shows the decoder's first picture, and the seek bar disables itself until the stream declares a duration rather than lying. Without the decoder staged the verdict is `disabled` and the screen says which tools are missing. |
| a site somebody pasted (`hdtoday`-style pages, anything without a media extension) | **framed, and its own player decides** — the media policy frames any `https:` origin, so the site's page is shown in a sandboxed frame and the log says `embed_framed` when a document arrived. If the link is a shell that refuses framing (`X-Frame-Options` / `frame-ancestors`) the host resolves it first and the *app inside* is framed instead — that is what `123movie-tv.it.com` turned out to be: 5.7 KB that refuses framing, wrapping two apps that do not. Whether its *video* plays is then the site's own business: these sites are JS shells over HLS/MP4 that is H.264 + AAC, the pair this build cannot decode, so a site whose player does no codec detection will show its UI and refuse the stream. Nothing about the site is the problem; the codec is. |
| Netflix, and any Widevine/PlayReady service | **impossible** — no CDM. A CDM cannot ship inside a process running under EAC, and Netflix additionally gates desktop playback on a hardware signature a CEF host cannot present. |

The refusals are deliberate and visible: the page names the codec and why, on
screen. Handing an `.mp4` to a `<video>` element produces a silent black
rectangle, which is the same picture as "the composite is broken" and costs an
afternoon to tell apart.

### The measured evidence, and the two real ways out

The figures above are from a probe run inside the real host, kept in
`docs/webui-media-and-audio.md`: `canPlayType('video/mp4; codecs="avc1…"')`
answers empty, `MediaSource.isTypeSupported('video/mp4; codecs="avc1.42E01E,
mp4a.40.2"')` answers false, a real `<video>` load fails with
`NotSupportedError`, and `EME` exists while both Widevine and PlayReady answer
`NotSupportedError`. WebM/VP9 answers `probably` and plays.

So an H.264 source can only reach a world screen after something has changed its
codec, and there are exactly two honest ways to do that:

1. **A transcode proxy in front of the page.** Resolve the page to its real
   stream and hand the screen a VP9/Opus version of it — `ffmpeg` will take an
   HLS/MP4 source and produce either a WebM file or an HLS playlist whose
   segments are VP9, which this build plays. That makes "paste a link" work for
   H.264 sites and changes nothing else, at the cost of a process and the CPU to
   transcode. It does not help DRM services, and it cannot: the key is never
   handed over.
2. **A CEF build with proprietary codecs.** The decoders are absent from this
   Chromium build, not from the platform, so a build with `proprietary_codecs`
   enabled plays H.264/AAC directly — no proxy, no transcode, no extra process.
   It is a bigger change (the runtime ships next to the host and is staged by
   the host's CMake), and it is the one that makes the *page* capable instead of
   making a *stream* compatible.

Neither route makes a paid service playable. Both make the free, unencrypted
H.264 web work, which is what a pasted link usually is.

## What changed after the first extraction

The repository started as a snapshot. Everything below landed afterwards, and
each one is in the suites as well as in the code:

* **`media.move` / `media.rotate`** — a set can be nudged along its own axes
  (forward/back/left/right/up/down) and turned on the spot, from the console or
  the menu. The prop is *patched*, never respawned, so every player watching
  sees the cabinet slide rather than disappear and reappear, and the screen needs
  nothing at all: the quad is in the prop's own frame, so it follows for free.
  The arithmetic (`shared/placement.lua`) is pinned by its own suite, and
  `tests/MediaPlacementIntegrationTests.cs` drives the path end to end through
  the real resource host and the real prop registry — the fourth seam, where a
  command can report success while the registry still holds the old transform.
* **A page no longer outlives its world.** The native half releases every screen
  when the world goes away; this resource is *not* stopped by a world change. That
  mismatch composited a television over the next loading screen. The client now
  releases on `open77:session:ended` and, once a second, drops and rebuilds any
  page whose native screen is gone.
* **The screen budget is by distance.** Six surfaces per client, and the ranking
  used to give every already-materialised screen absolute priority — so a set
  spawned at your feet could never win a slot while six others were up anywhere
  within 90 m, and it stood there as a prop with a page and no picture. Nearest
  first now, with a 5 m hysteresis so a page is not swapped away by a set that is
  marginally closer.
* **Volume and mute are reported.** The page says which channel carried a
  command and what the player ended up applying; the client logs the on-screen
  control at the click, before the round trip; the server answers a control that
  names a set it does not hold instead of returning silently. Without that, "the
  slider does nothing" and "the slider changed a television three kilometres
  away" look identical in a log.
* **A set's picture is anchored to the frame that is presented**
  (`native/ScreenMotion.hpp`). Corners are projected once per game tick (24–69
  Hz, jittery) while the overlay draws per presented frame — with frame
  generation, frames that sit *between* those ticks. The picture used to hold a
  corner set for up to 42 ms and then jump, which read as the image sliding
  around on its own cabinet while you walked.
* **A record can declare which side its picture is on** (`native/ScreenQuad.hpp`).
  Without it a screen is drawn from whichever side you stand on — "the television
  is playing on both sides" — and the snapshot now reports `facing` so the log
  says which of the three it is: behind it, in front of it, or undeclared.
* **A pasted site is framed, and the page can say so.** The media policy granted
  frames to YouTube and its no-cookie host and nothing else, so a website link
  was refused by our own header before the request left the process — the screen
  sat on the idle pattern and the log said only `embed_unverified`. It now frames
  any `https:` origin (never plaintext), the frame runs sandboxed with
  `allow-top-navigation-by-user-activation` so a click can follow a site's own
  player but a silent redirect cannot take the screen, and the frame's `load`
  event is reported as `embed_framed`: the one thing this side can honestly
  observe about somebody else's document. `webhost/tests/WebHostTests.cpp`
  serves the same page under both policies and asserts the pair — media frames,
  strict refuses — against the real host and real Chromium.
* **A link that refuses to be framed is resolved instead of given up on**
  (`native/FramePolicy.hpp`, `native/FrameResolver.cpp`). A pasted site is often a
  shell: a few kilobytes that send `X-Frame-Options: SAMEORIGIN` and wrap the
  application that actually plays. The page cannot see that — a refusal arrives
  as an error page and is invisible to the embedder, which is why the log said
  `embed_unverified` and the screen stayed empty — so the host is asked, from
  outside, by `/op77/web/frame`. The answer names both halves: whether the link
  may be framed, and, when it may not, the embed inside it that may. It is a
  probe and not a proxy — headers read, markup only when framing was refused,
  **no cookies sent** — so the app is then framed by CEF with the site's own
  origin and session. `webhost/tests/WebHostTests.cpp` resolves the real shell
  over real HTTP and frames what came back.
* **An advertisement cannot take the screen** (`native/AdBlock.hpp`).
  `OnBeforePopup` was unimplemented, so every window a page opened was created —
  and painted as an overlay, because this host implements `OnPopupShow`. The rule
  that separates an advertisement from a player is the **user gesture**: a window
  with no click behind it is advertising by definition, a window a click asked
  for is followed in place (a television has no tabs, and a player that opens its
  video in a popup has to play somewhere). Ad hosts are also cancelled as
  requests and refused as navigations, which is what stops the silent redirect
  that otherwise eats the page with no click and no way back.
* **One pointer, not two.** A screen session drew *two*: `WebUiService`'s cursor
  at the raw pointer's viewport coordinates, and a `Style::Dot` item at the
  projection of the pointed-at page pixel onto the panel. They coincide only when
  the panel fills the viewport, which a television in the world never does — so
  you aimed with one and clicked with the other. The panel now publishes where
  its quad landed (`native/ScreenInput.hpp`) and the cursor is drawn *there*; a
  tick that publishes nothing (prop unstreamed, camera behind the screen) falls
  back to the raw pointer, which is what happens with no session open.
* **A page is no longer mistaken for a dead stream.** The probe is where every
  unknown link goes (ffprobe is the only thing here that knows what a link is),
  and its `nothing` verdict used to be terminal: `probing (https://…/hdtoday/)`
  followed by `not_media (the decoder refused the link)`, with the frame that was
  waiting on the other side never built. The extension now decides which of the
  two it is — a link that names a media container and that nothing claimed is
  dead and says so; a link that names no container is a site and gets framed
  (`not_stream_site`, then the framed path).
* **A link this build cannot decode is decoded by the host**
  (`native/TranscodePlan.hpp`, `native/Decoder.hpp`). `ffprobe` answers what a
  link is; `ffmpeg` re-encodes what this Chromium cannot play into VP9/Opus WebM
  and serves it from inside the browser process, so the element, the transport,
  the volume and the seek logic are the same ones a native WebM uses. The decoder
  tools are staged next to the host and named at launch, so a host without them
  answers `disabled` with the reason instead of showing a black rectangle.
* **Remote content is one decision in two files.** A CSP directive is a
  *permission*; the host's `GetResourceRequestHandler` is the *gate*. The header
  described a privilege the host then cancelled, and the only symptom was a
  player script that said it had been refused. Both now come from
  `native/PagePolicy.hpp`.
* **`media.spawn` / `media.place` accept an omitted URL.** The usage string said
  `[url]`; the code refused nil with `url_must_be_a_string`, so "spawn a
  television here" failed with a message about an argument the operator had
  deliberately left out.
* **The host-name check derives instead of transcribing.**
  `tests/MediaRecordsTests.cs` used to carry a hand-written list of the host's
  functions, and it went stale: it named `Open77.props` as create/remove/all
  only, so the placement feature's `setTransform` — a real binding on both
  runtimes — failed as if it were a typo. It now parses the two registration
  sites, is **per runtime** (the client and the server surfaces differ, and the
  bug it exists for was a name used on the side that does not have it), and
  asserts on its own output so a parser that stops matching fails loudly.

### The base game's drive-in screen is not this

The drive-in in the Badlands (the Bushido film Johnny takes Rogue to) is a
level-local screen fed by the game's own movie system, not a CEF surface: the
film is shipped media played into a material the level owns. Hijacking it would
be engine-side work against a system this repository has not reverse-engineered,
and none of it is verifiable from here.

What *is* the same idea and is reachable: a **spawned** large screen prop — a
billboard, a 33:10 sign, one of the cinema-sized quads in the catalogue — placed
wherever the operator wants, running this feature's page, with the menu showing
which set is in front of you. Proximity is already how a client decides what to
materialise, so "walk up to it and it is the nearest set" is a property of the
existing code rather than a new system.

## The honest limitations

* **Occlusion.** The world overlay has no depth buffer, so a screen the producer
  decides is visible is painted over whatever geometry stands between the camera
  and it. `Api::MediaScreens` line-of-sight tests the screen's *centre* and the
  whole quad is then drawn or not, so a partially occluded screen is drawn whole.
* **Volume on a foreign embed.** A YouTube embed is driven through its
  `postMessage` transport, so volume, mute, pause and seek all work. Any other
  site's player lives in a document this page cannot reach: the slider moves and
  nothing happens, and the page says so rather than pretending.
* **Screen budget.** Eight CEF surfaces per resource, held by the client. A
  server may hold far more televisions than any one client will materialise, so
  the client decides which are worth building from the distances the server
  publishes.
* **Facing.** The screen's position is server-authoritative (read from the
  player's own snapshot), but the yaw is accepted from the caller and clamped,
  because nothing in the player snapshot publishes a heading.

## Running the suite

```bash
dotnet run --project tools/suite-runner                       # vendored snapshot
dotnet run --project tools/suite-runner -- --from /path/to/open77-base
```

No Lua interpreter is needed: `tools/suite-runner` carries Lua 5.4 through
KeraLua, the binding the monorepo's `Open77.Server.Tests` already uses, so the
four suites run anywhere `dotnet` does — and CI runs them on Linux and Windows
(`.github/workflows/ci.yml`), which is the first time these suites have been a
gate rather than something somebody remembers to run.

`tools/run-suite.py` runs the same four files through a real `lua5.4` (pass
`--lua /path/to/lua` if it is not on `PATH`). It refreshes the snapshot too. Both
runners write it byte for byte, so the file does not depend on which one you used.

The suite cross-checks every record's `model` against the prop-model aliases that
`open77_admin` publishes — the list the prop-host build above emits — which is why
it needs one of the two sources. The vendored snapshot lets it run with no
checkout, but only the `--from` form cannot drift, and that difference is real:
pointed at a checkout that predates this work, the records suite fails, because
38 records name 36 `electronics.*` aliases (9 `tv`, 14 `monitor`, 7 `screen`,
6 `frame`) that such a checkout does not publish. The snapshot is the list the
catalogue was written against; `--from` is how you find out whether a given
server can spawn these sets at all.

### The snapshot is generated, so it is checked

```bash
dotnet run --project tools/suite-runner -- --check-fixture
dotnet run --project tools/suite-runner -- --check-fixture --from /path/to/open77-base
dotnet run --project tools/suite-runner -- --refresh-fixture --from /path/to/open77-base
```

A generated file is only as good as its last regeneration, and the records suite
cannot tell you whether it is current: it proves the snapshot is *big enough* —
no record names an alias the list lacks — and never that it is *up to date*.
`--check-fixture` is that second question. It fails when the file is not in the
form the generator writes (a hand-edited alias, a count line left behind, a lost
CRLF ending, a truncated list), when an alias is listed twice, when it is missing
an alias this repository's own `open77_admin` hunk in `patches/` adds, and —
with `--from` — when it differs from the live list, naming both directions of the
difference. CI runs the first form on every push, before the suites, so a
malformed snapshot reports as a malformed snapshot.

The snapshot cannot be fresher than the tree it was taken from, and refreshing
from a tree that predates the television work is destructive rather than
informative: pointed at such a checkout today, `--refresh-fixture` would write the
185 aliases it has and drop the 37 it does not, after which every television
record fails its cross-check. Refresh from a tree that carries the work.

CI cannot see upstream growth by itself, because that needs a checkout of a
private repository: the snapshot is only ever as fresh as its last regeneration
from a tree that carries the work. `--check-fixture --from <checkout>` is how you
ask that question when you have one.

## Installing it into a server

The resource is self-contained; the **render** is not, because a browser on a
world quad is host-side work. `docs/integration.md` names every seam, and
`patches/` carries the TV-only hunks of each one.

Two things bite during install, both written up in that document:

1. A newly added resource is **not** picked up because its files appeared. The
   automatic rescan is opt-in, so either restart the server or run the console
   command `refresh` — and until that happens the server looks healthy while the
   resource simply is not there.
2. The menu tab lives in the **`freeroam`** resource, not in `open77_media`. Both
   have to be current, or the catalogue loads and no tab appears.

## Provenance

Extracted from `Open2077/open77-base` (branch `feat/cynosure-dev2`), which is
where this was developed and verified. `open77_media/` and `native/` are
verbatim; `patches/` holds only the television-relevant hunks of files that
carry other unreleased work, selected mechanically by
`tools/extract-tv-patches.py` and therefore not a clean patch series — apply
them by hand.
