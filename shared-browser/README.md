# The shared browser: a real Chromium on a television

The game's own browser cannot play most video sites: it has no H.264/AAC decoder
and no HLS demuxer (see "What actually plays" in the top README). The shared
browser puts a real Chromium on the server instead and streams its picture and
sound to a television over WebRTC, as VP8 and Opus, which the game's browser does
play. Everyone at that television sees the same browser; whoever presses F8 at
the screen drives it with the mouse and keyboard.

## The link is a credential (read this before a server has accounts signed in)

The television's link (`tv-url.secret`, root-only, one per instance --
`/opt/open77-tvbrowser/tv-url.secret` for staging,
`/opt/open77-tvbrowser-xb/tv-url.secret` for production) carries
`?usr=open77&pwd=<the viewer password>`. That password is a whole session on the
room's desktop: **anyone who has the link can open that desktop from their own PC,
watch it and drive it** -- the mouse, the keyboard, and whatever is signed into
that Chromium. It is the same desktop the players at the television see, so the
link cannot be "not given out" to them; it is given out on purpose, inside the
frame the television loads.

What that means, and what was done about it (2026-10-16):

- `/browser` is ADMIN-ONLY. It was the one path that handed the link to any player
  who typed it; it now needs `command.media.lock` (`server/config.lua`,
  `adminAce`), and a player without it is told so instead of being handed the
  credentials. The browser cinemas are unaffected: they get the link from this
  resource's `link` export, which answers `open77_media` and nothing else.
- The page the television loads is the room's own desktop, so a player who is at a
  television can still see it, and the link is in that page's address. Treat it as
  **extractable** by anyone who is allowed to see the screen.
- So the accounts signed into that Chromium must not be personal ones: sign in with
  an account made for the room (a shared/free account), not the operator's own
  Netflix or Google. Signing out under a link that has leaked is the only thing that
  actually revokes it.
- ROTATE the viewer password after any leak (`neko.env`,
  `NEKO_MEMBER_MULTIUSER_USER_PASSWORD`), which means rebuilding the link and
  re-writing the servers' `opx_tvbrowser` config from it -- `server/up.sh` refuses to
  recreate a container whose `tv-url.secret` no longer matches `neko.env`, which is
  what stops a half-rotated install. Note that `up.sh` recreates the container, and
  the browser's own profile (its cookies and sessions) lives in that container: a
  rotation signs the room out, so do it when nothing is signed in rather than
  around one.

```
opx_tvbrowser/   the resource: /browser (admin-only) puts it on the TV in front of
                 you, /browser cinema puts up a 150 ft cinema showing it (/browser
                 cinema 100: a 100 ft one), /browser off; and the link the TV
                 menu's two browser cinemas are put up with
server/          the container beside the game server -- one instance per role,
                 staging and production (see "On the server"): neko's Chromium
                 image without Widevine, its browser policies, the four scripts
                 the image puts in the client page, and the scripts that build it
                 (up.sh), publish it (nginx.sh), point a game server at it
                 (install-tv-config.sh) and check it (test/)
```

## On a television

- **The TV menu** has two browser cinemas, "Browser cinema, 100 ft" and "Browser
  cinema, 150 ft" (open77_media's `cinema.100ft.browser` and
  `cinema.150ft.browser`). Each is its cinema's own panel -- the same scale, quad,
  reach and collider, which `records_test.lua` checks -- put up already showing
  the browser: the record names `linkFrom = "opx_tvbrowser"`, and open77_media
  asks that resource's `link` export for the link when the set is put up
  (`linkedUrl` in `open77_media/server/main.lua`). Only open77_media is answered,
  because the link carries the viewer password. A server without the resource
  refuses the spawn by name instead of putting up a blank screen.
- **The page** frames a link ending in `#open77-shared-browser` as this browser
  (`SHARED_BROWSER_MARK` in `open77_media/web/tv.js`): no probe, no frame check,
  kept across state updates, closed when the television leaves it. It asks for
  client page 2 (`open77=2` on the link), so a page a PC cached from an older
  image is not used: the client page is sent without a cache lifetime, and
  Chromium would otherwise keep it for hours.
- **F8** at the screen takes it: the mouse and keyboard go to the browser, whose
  own address bar and tabs are on the picture. New tabs open Google: the
  guest-style profile neko starts cannot load Chromium's own new tab page, which
  left a new tab blank.

## Pasting a link from your PC

Copy the link on your PC, press F8 at the screen, click the browser's address
bar, press **Ctrl+V** (or Shift+Insert), then Enter.

Two clipboards are involved, and that is why it is done this way. The client page
plays every key to the server's Chromium, Ctrl+V included -- and there, Ctrl+V
pastes the SERVER's clipboard. The client's own way of carrying a PC's clipboard
across reads it with `navigator.clipboard.readText()`, which the game's browser
never allows: its permission handler grants nothing but local network access.
That is right, not something to undo: a page that could read the clipboard could
read whatever a player copies, and a game server is not someone a player shares
their clipboard with. A `paste` event, though, carries the clipboard text with no
permission at all -- because the player just asked to paste.

So `server/build/open77-paste.js` (in the client page, ahead of the client's own
scripts) does a paste in two steps:

1. Ctrl+V on the picture is kept from the client's key handler, so the page's own
   browser pastes instead, and that paste event has the PC's clipboard text.
2. The text goes to the server as the shared browser's clipboard (the client's own
   `control/clipboard` message, on the client's own socket), and then Ctrl+V is
   played to the server's Chromium through the client's key handler, so it pastes
   that text. Ctrl is pressed and released there only if the player has already
   let go of it.

The clipboard is read only when the player pastes, and only that paste's text is
sent. **Right-click → Paste** is the server browser's own menu, so it pastes the
server's clipboard (the last thing pasted with Ctrl+V, by anyone); a right-click
on the picture shows a note saying to use Ctrl+V. A paste is reported to the
television's log as `browser_ice (paste ...)` -- which key, on what, how many
characters, or why nothing was sent -- and never the text.

## Volume and mute

The television's volume slider and mute button are the shared browser's too. The
client page is the server's, so the television (`open77_media/web/tv.js`) cannot
reach into it: it posts its level to the framed page -- to that page's origin only,
at every update and once the page has loaded -- and `server/build/open77-volume.js`
applies it. Each set has its own level, as every television does.

It applies it to the sound itself, through Web Audio, not to the stream's media
element. The game's browser takes a page's sound as the page produces it, and a
WebRTC stream's element volume is applied after that, by the audio device: in game
the element at 75, 90 and 100 was one loudness, and that one too quiet (2026-10-02).
So the stream's own audio track is disabled -- WebRTC plays a disabled remote track
at zero, which silences the element -- and a clone of it, which disabling the
original does not silence, goes through a leveler, a gain and a limiter to the
page's output. The leveler (a compressor at -42 dB, 6:1, with its make-up gain)
is there because twice the stream's level was still too quiet in game: a film on
a streaming site sits far below the game's own sound, and gain alone only made
its loud moments clip. It pulls everything above -42 dBFS into a narrow band near
-15 dBFS -- quiet passages raised by up to about 24 dB, loud ones held down. The
gain is `4.5 x (level / 100) ^ 1.25` on the leveled sound: 100 is +13 dB (about
-5 dBFS rms, as loud as sound gets without distortion), 75 is 3.15 (+10 dB), 50 is
1.90, 35 is 1.19, 0 and mute are silence; the limiter (-1.0 dB, 20:1) keeps the
peaks under full scale. Each step was a player's "still too quiet" in game: the
element's volume (no effect), twice the stream's level, a gentler leveler
at twice (-11.3 dBFS rms at 100, in game, 2026-10-02), then the leveler and curve
raised again after a live in-game test on 2026-10-16 (below: dialogue +5.8 dB on
the same page and tone). Until Web Audio may start (a game that holds a page's sound back
until a click) the element keeps its own sound, with the level on it; the first
click or key on the picture starts it. A new stream (the client reconnecting) is
taken over the same way, and a track something turns back on is turned off again.

The page reports where the level went (`volume_applied`/`mute_applied ... via the
shared browser's page`) and the client page what it did, when that changes:
`browser_ice (volume 75 on the stream's sound (gain 3.14, Web Audio running))`, or
`... on 1 element (Web Audio suspended)` before it can start -- and, five seconds
after a change, how loud it came out: `browser_ice (volume output -5.2 dBFS rms at
90 (gain 3.94))`. A page cannot go above full scale; sound that is still too quiet
with the output near -5 dBFS is the PC's mixer (the game's web host in Windows'
volume mixer) or the game being louder.

Before this the slider and the mute button changed nothing at all on the shared
browser: they were wired to YouTube's player and to plain video only.

## A box the picture does not fill

The television hands the framed client page the WHOLE screen box, edge to edge,
and the box is often not the stream's shape: the game's sets are 16:9, 21:9 and
2.35, the television's control strip takes its height out of the box while
someone is using it, and a client drives the surface with the picture across its
own viewport, whose shape is that PC's monitor (5120x1440, 32:9, measured
2026-10-16). The stream is one shape for all of them: the room's Chromium renders
1280x720 (`NEKO_DESKTOP_SCREEN`), and it is the same picture for everyone at the
television.

Four answers have now been measured on a television, and four were rejected:
**leaving the rest black**, which is what the bars were -- black stripes down the
sides of the set, the first thing reported; a **blurred copy** of the stream
("the two side bars are blurs": fog where a page's own chrome or a film's letterbox
has to read as the picture); the same stream **scaled to cover** the box and clipped
to the bar ("showing another part of the video... double letters": the video again in
the bar, and a second element can trail the picture by a frame); and finally the
picture's own **edge column, stretched** across the bar -- a one-pixel column has no
structure of its own, so stretched sideways it draws a horizontal smear per row, and
the television said so in one line: "it is like stretching the video", with a
screenshot of the cinema panel (2026-10-16).

The television's page does not touch the picture at all (`web/tv.js`: the frame is
the box, exactly, and the picture inside it keeps its shape). What happens inside
that box is the framed page's own business, and `open77-fit.js` in the image answers
it there, with one answer on every shape: **THE PICTURE FILLS THE BOX**. It is scaled
up, keeping its shape, until it reaches every edge of the box, and the overhang is cut
-- `object-fit: cover`, which is the browser's own uniform scale, so the picture is
cropped and never stretched. Nothing is drawn beside it, because there is nothing
beside it: no canvas, no layer, no second stream, no fill of any kind. The 8 px stripe
the cinema panel reported is simply gone, and so is every other gap.

What that costs is honest and measured: a 16:9 picture in a 2.39 box loses 26 % of its
height (13 % off the top and 13 % off the bottom), and on a 32:9 client's own surface
it loses half of it. That was reported too ("the top of the browser's own window gone")
-- and it is what a screen with no bars means. Five fills that left something beside the
picture were each measured on a television on 2026-10-16 and each was rejected as bars;
the one that keeps every pixel is the one with bars in it.

The cover is a **class** the script adds and takes away (`open77-cover`), never an
inline style: the picture's own style attribute belongs to the page -- neko sizes that
element itself -- so nothing here writes to it, and taking the class off restores the
page's layout exactly.

What that script does, and why it is put where it is:- The client page's black is `div.player` (`background: #000`) around a centred
  16:9 `div.player-container`, with the picture absolutely positioned inside it
  (measured in the room's own image, 2026-10-16). In the glow case that is exactly
  what is left alone; in the cover case the class puts the picture over the whole box,
  and the black is behind a picture that reaches every edge.
- Each glow gap is a canvas, placed exactly on that gap, holding the picture's own
  edge strip averaged into six stops and drawn as a gradient that follows the
  picture's own rows (or columns) -- so the wash and the picture meet row for row at
  the seam, and the colour at the picture's top row is the picture's own colour there.
  Drawing the strip SMALL is the averaging: the browser does it, and the script never
  reads the picture a pixel at a time.
- The bars carry **no second stream at all** (`streams: 0` is a check in both tests),
  and no filter anywhere (`filter: none`): the sound needs no care at all, and nothing
  in a gap can show the video again or trail the picture by a frame.
- Where the layer is put is the other half of the trick, and the first draft got it
  wrong: it goes **immediately before the picture inside the picture's own box**,
  sized to the viewport -- which is the box, since the television frames this page
  1:1. A child paints over its ancestors' backgrounds but under what follows it in
  the page's order, so a layer one ancestor further out (that draft chose the
  outermost ancestor that paints a colour and still covers the picture -- the
  client's `rgba(0,0,0,.4)` wrapper) sits UNDER the black that shows the bars and
  paints nothing at all. `test/fit-local.mjs` measures it in pixels and says so.
- In the glow case the picture is not touched: not its size, not its `object-fit`,
  not its place, and no crop. The one thing the script will do to a picture a client
  lays out in the document's flow rather than positioning is lift it
  (`position: relative`, no offsets, no z-index) so that the page's own order still
  puts it over the layer -- the client's overlays stay over the picture.
- Nothing is added where there is nothing to crop or fill (a box of the picture's own
  shape -- the 16:9 set), and nothing is done while the picture has no shape at all:
  before the first frames a picture reads as 0x0, which would be a gap on every side,
  and a wash of it would cover the whole box. The cover is taken off and the layer
  re-made if the client rebuilds the player (a reconnect, a source change) or the box
  changes shape (the control strip taking its height). What it did is reported to the
  television as `kind fit` (logged as `browser_ice`), once per box shape, and the
  television's page reports the box itself as `browser_box`.

The glow is meant to read as the picture's own light spilling past the edge of the
glass: what a player sees in a gap is the colour the picture has at that edge,
averaged and dimmed. On a dark page that is a dark wash -- it is that part of the
picture's own edge, and not a hole, but it is not a bright frame either.

**Measured** on 2026-10-16, on the real page in the real container
(`server/test/e2e-fit.sh`, Playwright on the VPS, the television's own link):

- the 150 ft cinema panel's own surface, 1280x728: **cover** -- the picture is the
  whole box wearing the class, `object-fit: cover`, and no layer exists at all. All ten
  gap samples, which read exactly `[0,0,0]` on the same page with the script's request
  refused, now read the picture (`[[0,11,9],[3,24,19],[5,27,20],...]`): the 8 px black
  stripe is gone.
- a 32:9 client's viewport, 5120x1440: **cover** too -- the picture is the whole box
  wearing the class with `object-fit: cover`, no layer at all, half of the picture's
  height cut (25 % off the top, 25 % off the bottom), and the same ten points, which
  read exactly `[0,0,0]` without the script, now read the picture.

The comparison is per gap and per stop -- the bar's own pixels against the edge strip
of the picture itself, drawn small and dimmed the way the script draws it -- and the
control is the same box on the same page with the script's request refused, whose gaps
read exactly `[0,0,0]` (the client page's own black; a decoded frame of video, however
dark, is not three zeroes). It is also the comparison the two rejected fills failed:
the overnight run before this one read the left pillar of that 32:9 box as
`[118,127,46], [255,255,253], [4,2,0]...` against the picture's own `[124,135,10]`
-- another part of the frame, exactly as the television reported it.

`server/test/fit-local.mjs` is the same set of questions against a local stand-in
shaped from the room's own CSS, with a real WebRTC stream: 70 checks over six box
shapes (1280x728, 1280x675 with the strip up, 2560x1080, 5120x1440, 720x1280, and the
16:9 box where nothing is to be cropped), each with the control run WITHOUT the script,
which must read black in exactly the places the cover fills, or the check proves
nothing. Every filled shape is measured as: the element IS the box, the computed
`object-fit` is `cover`, the class is on, NO layer and NO canvas exist, the box's own
edges are the picture, the synthetic stream's square white mark is still SQUARE
(78x78 on the cinema panel, 156x156 on the 21:9 box) and still the middle of the
screen (a stretched picture shows a rectangle off-centre), and the page's own inline
style attribute is byte for byte what it was before a stream existed -- the cover is a
class, and the only thing this script ever writes to that attribute is the lift a
client that lays its picture out in the document's flow needs. The last section puts
both scripts on the one page, as the room runs them, and holds the sound on the picture
element (`on: "stream"`): the cover adds no media element at all.

## When the picture does not come

The stream is WebRTC: UDP to that role's public port first -- 59100 staging, 59101
production -- and TCP to the same port when UDP answers do not come back (Chromium
tries that by itself). The page says what happened, in the client log:

- `browser_net`: whether a public STUN server's answer reaches this PC over UDP
  (candidate kinds and counts, never an address).
- `browser_ice (connected ...)`, from `server/build/open77-ice.js` in the client
  page: the candidates gathered and offered, every pair tried with the checks
  sent and answered, and the pair that carried the picture.
- `browser_ice (retry_tcp ...)` and `(gave_up ...)`: after 20 s without a picture
  the page reopens once with only the server's TCP candidate (and offers none of
  its own UDP ones); if that fails too, the television says the picture cannot
  reach this PC.

Measured on XBUNIVERSE staging, 2026-10-02, from a Playwright Chromium on the
server with firewall rules inside the test container only (`server/test/e2e-page.sh`):
nothing blocked -- UDP; UDP answers dropped -- TCP by itself in 0.2 s; TCP to
59100 dropped -- UDP; both dropped -- the retry over TCP, then the notice. In game
the first try stayed black with nothing that reached 59100 attributable to it;
every later one connected over UDP in about 0.1 s. A PC that stays black: look at
its firewall's rules for the game's web host,
`red4ext\plugins\Open77\web\Open77.WebHost.exe`.

## DRM via the host's own CDM

This browser's picture is captured and streamed to players, and protected video
(Netflix and the rest) plays in it through the normal Encrypted Media Extensions
path. The neko image ships Widevine; the `Dockerfile` still removes it --
Google's CDM binary is not baked into (or redistributed with) our image -- and
`server/up.sh` mounts the host's own Chrome copy read-only at run time
(`/opt/google/chrome/WidevineCdm` to `/usr/lib/chromium/WidevineCdm`). The policy
keeps component updates off, so nothing else is fetched back.
`server/drmcheck.sh` asks both images (stock and ours) for the Widevine key
system: on XBUNIVERSE staging, 2026-10-02, the stock image answered
`WIDEVINE_AVAILABLE` and ours `NotSupportedError`. With the host CDM mounted,
measured 2026-10-16, the room Chromium answers `WIDEVINE_AVAILABLE` -- so the
mount, not the image, is what the CDM comes from. Where a service refuses this
host anyway (Netflix's own device check), that refusal is the answer; the
honest fallback stays the watch party (`watch-party/`).

## Shared, and what that means

- Anything signed in to on it is seen by everyone at that television.
- A link pasted into it is the shared browser's clipboard afterwards: the next
  person to right-click → Paste gets it.
- `up.sh [staging|production]` recreates that role's container, which wipes that
  browser's own state (sessions, history); the other role's is not touched.
  Passwords stay in each instance's own `neko.env`.
- Downloads, file dialogs, developer tools, the password manager and browser
  sign-in are off (`server/build/policies.json`), as are notifications, pop-ups,
  camera and microphone. The one extension is uBlock Origin Lite.
- The viewer password is in the television's link, so every client that shows
  it holds it: it is a password for the players of that server, not a secret
  from them. It never reaches a client as a file: `opx_tvbrowser/server/config.lua`
  is written on the server at deploy and is not in the client package.

## On the server

Two instances, one per role, on the same machine. They are the same image and the
same `build/`; they differ in where they live, what they are called, and which
three numbers they take:

| role | install dir | container | prefix | client | WebRTC |
| --- | --- | --- | --- | --- | --- |
| staging | `/opt/open77-tvbrowser` | `open77-tvbrowser-xbs` | `/tv-browser-xb-staging` | `127.0.0.1:18080` | `59100` |
| production | `/opt/open77-tvbrowser-xb` | `open77-tvbrowser-xb` | `/tv-browser-xb` | `127.0.0.1:18081` | `59101` |
| f2077 | `/opt/open77-tvbrowser-f2077` | `open77-tvbrowser-f2077` | `/tv-browser-f2077` | `127.0.0.1:18082` | `59102` |

Each instance generates its OWN `neko.env` (viewer and admin passwords, API token)
and its own television link `tv-url.secret`, both root-only, so the two links --
and the two rooms behind them -- are separate. Recreating one role's container
wipes that browser's own state (sessions, history) and does not touch the other's.

1. Copy `server/` to that role's install dir (root, 700) and run
   `./up.sh [staging|production]` (default staging). The first run generates
   `neko.env` and `tv-url.secret`; later runs reuse them, so the link does not
   change. It builds `open77/tvbrowser-chromium:9`, checks the image (the four
   scripts in the client page, the policy, kiosk and the window manager, the
   profile library and the DevTools forwarder, no Widevine baked in) and the host CDM,
   and runs the container capped at 4 cores, 4 GB and 2 GB of shared memory, with
   the client on that role's loopback port and WebRTC on that role's port (UDP and
   TCP, published by Docker, so the host's INPUT rules are not changed), plus the
   host CDM mounted read-only for Widevine.
2. `./nginx.sh [staging|production]` adds that role's path (websocket) to the site,
   after backing it up and `nginx -t`, and leaves the enabled and available copies
   of the site in step. It then asks the new route through nginx, retrying for up to
   15 s: a graceful reload can still answer a first request from a worker holding
   the old configuration, and that one 404 is not a failure.
3. Install `opx_tvbrowser` in that game server's `resources/`, run
   `./install-tv-config.sh [staging|production]` -- it writes
   `opx_tvbrowser/server/config.lua` from THAT role's `tv-url.secret` (`url`,
   `cinemaRecord`, `cinema100Record`, `adminAce`; root, 0600), keeping the previous
   file -- and add `opx_tvbrowser` to `resources.load` in its `server.jsonc`
   (staging loads an explicit list). The copy of that config in this repository is
   a placeholder with `url = nil` on purpose: the link carries the viewer password.
4. `test/e2e.sh`, `test/e2e-page.sh`, `test/e2e-paste.sh`, `test/e2e-volume.sh`
   and `test/e2e-fit.sh` take the same role argument (default staging), and run
   against that role's container, `neko.env`, link and WebRTC port. They run a
   Playwright Chromium on the server, so it reaches the browser the way a
   player does: logged in from the television link, the picture over the public
   IP (in the four networks above), a link pasted into the address bar and
   opened, and the television's level measured in the sound it puts out (2026-10-02,
   image 6, a -24 dBFS tone played into the browser's own output: 2.43 times it at
   100, 0.50 at 35, silence muted, 1.58 at 75 -- the gain curve and the limiter's
   make-up gain, as computed). Those readings are the curve *before* 2026-10-16;
   the level went up that day (below), so a fresh run reads higher.

What is on the machine now (2026-10-16/17):

- staging went in first, on 2026-10-02, with the production servers untouched.
- production's instance was created on 2026-10-17: `./up.sh production` built image
  `open77/tvbrowser-chromium:7` from the same `build/`, started
  `open77-tvbrowser-xb` (healthy), and its client page answered HTTP 200 on the
  loopback at 00:55Z; `./nginx.sh production` published `/tv-browser-xb/` (HTTP 200
  through nginx at 00:56Z); and `test/e2e.sh production` passed at 00:59Z -- logged
  in from production's own link, the WebRTC picture connected and advancing,
  1280x720.
- both game servers on the machine run `opx_tvbrowser` 1.0.2 with `/browser`
  admin-only: each one's `server/config.lua` carries
  `adminAce = "command.media.lock"`, written from that role's own link.

Staging went through seven builds of `server/build/`, each a recreate with the
same `neko.env`, ports and link, the one before kept for rollback: 1, Widevine
removed and the policies; 2, `open77-ice.js` and the health check at the path the
server answers on (neko's own asked `/health`, which a server with a path prefix
answers under the prefix, so a healthy container reported unhealthy); 3,
`open77-paste.js`; 4, the paste notes and reports, and new tabs opening Google; 5,
`open77-volume.js` on the element; 6, the level through Web Audio; 7,
`open77-fit.js` -- the picture fills the bar, after the day the television's own
page filled it by cropping. Build 7 is in staging's running container by `docker cp`
and built as the image tag from `build/` -- the container's page files are the
image's, checked file by file -- and production's container was created straight
onto image 7 on 2026-10-17, so no recreate is left owing. Build 5 also
makes the policy and the scripts readable whatever modes the build context had: its
first rollout was built from files copied from Windows, which arrive root-only, so
for three minutes (nobody connected) neko answered 403 for the scripts and Chromium
could not read its policies. `up.sh` checks the modes in the image now. Build 6's
script was first copied into the running container (`docker cp`, the image 5 copy
kept in its /tmp), so the page a player had open in the shared browser stayed open;
the container is recreated on image 6 when nobody is watching. After build 6,
the container runs with the host's WidevineCdm mounted read-only (the image
still ships none); EME measured `WIDEVINE_AVAILABLE` on 2026-10-16, against
`NotSupportedError` without the mount.

The television's level went up again on 2026-10-16, after a live in-game test:
with a film playing in the room, the room's own output measured peak -7.9 dBFS /
rms -26.6 dBFS, and the curve of the day (3 at 100, 1.95 at 75) still sat well
under the game's own mix -- audible, but too quiet to follow dialogue over the
game. So the leveler's threshold went -36 dB to -42 dB and the level is now
`4.5 * volume^1.25` (100 is +13 dB, 75 is 3.15, 50 is 1.90, 35 is 1.19, 0
silence) instead of `3 * volume^1.5`. Measured with `server/test/volume-local.mjs`
on the same framed page and the same tone, before -> after: a -40 dBFS tone (film
dialogue) came out 22.8x -> 44.7x louder (+5.8 dB), the output at 90 read -6.7 ->
-5.2 dBFS rms, and a full-scale tone still stayed under full scale (peak 0.97, the
limiter doing it). That is the shape of the change on purpose: loud material was
already at the ceiling the limiter sets, while dialogue -- the part that was
unintelligible -- had the room to move. The script was copied into the running
container (`docker cp`, so the page a player had open stayed up) and the image tag
rebuilt from `build/`; a recreate keeps it.

The picture is 1280x720 at 30 fps (`NEKO_DESKTOP_SCREEN`); each viewer is one
WebRTC stream from the server.

## The room's page: sign-in splash, profiles, Android (image 9, 2026-10-22)

Image 9 makes the room a *screen* and gives it a page of its own. It is documented
where it lives, in `room/README.md`, and in short:

- **Kiosk, full screen.** Chromium runs `--kiosk`, so a cinema shows the page and
  nothing else -- no tab strip, no address bar. Full screen is the window manager's
  answer, not Chromium's: the stock image has openbox but never ran it, so the kiosk
  window opened at Chrome's default 1050x700 in the corner of a 1280x720 screen
  (measured). `build/openbox.conf` runs it, and the window is 1280x720 at 0,0.
- **The profile library**, from image 8: Chromium on `--user-data-dir=/profiles/active`,
  a symlink the room service re-points; profiles are directories on the host, so
  sign-ins survive a recreated container -- `room/test/e2e-profile-persistence.sh`
  recreates one with `up.sh` and proves it. Managed at `<room>/profiles`.
- **The room page** (`open77-room` on the game server, port 8002): the start page
  every cinema lands on. Sign-in tiles for seven services (with whether the profile
  has a session), the channels, the profile switcher, the room controls, and
  **Android** -- the emulator's ws-scrcpy stream, started on demand.
- **Android with a mouse and a keyboard.** The stream URL carries
  `captureKeyboard=true`; without it ws-scrcpy leaves keyboard capture off and only
  the pointer works (measured: Settings' search stayed empty however much was typed).
  A host presses **Android screen** in the `/watch` panel or types `/watch android`,
  and the room -- polling that game server's `GET /v1/pending` -- starts the
  emulator if needed and puts its stream on the cinema screen.
- **A second Android that is not emulated** (`android-native/`, 2026-10-22):
  Waydroid -- the Android system image as a container on the host's own kernel,
  no qemu guest -- served to the room as a VNC framebuffer (headless sway +
  wayvnc + noVNC on port 8001) because this GPU-less VM cannot encode scrcpy's
  H.264. The rooms open `android-native/screen.html` there rather than noVNC's
  own page: a canvas that fills the window, with no control bar in front of the
  picture. It runs beside the emulator, not instead of it;
  `android-native/switch-room.sh native|emulator` decides which one the
  televisions show, as environment for the room service rather than a patch.

Which Android a room shows is environment on `open77-room`, not code:
`ANDROID_PORT`, `ANDROID_PAGE_PATH`, `ANDROID_UDID`, `ANDROID_SERVICES` and
`ANDROID_LABEL` (defaults: the emulator, `8000`, `emulator-5554`).

The room service is `room/` (server, pages, unit, install script, tests); it is
installed per host, not per role, and serves all three rooms.

## Tests

```bash
cd opx_tvbrowser && lua5.4 tests/run.lua    # the link rule, the commands, which TV, the export (33 checks)
node server/test/ice-local.mjs              # open77-ice.js against real peer connections (16)
node server/test/paste-local.mjs            # open77-paste.js: keys, socket, notes, reports (31)
node server/test/volume-local.mjs           # open77-volume.js on a real WebRTC stream: gain, leveler, mute, reconnect, autoplay (20)
node server/test/fit-local.mjs              # open77-fit.js: the box's edges are the picture, six shapes, each with its control (41)
node tests/tv-page/run.mjs                  # from the repository root: the page's half, volume included (85)
```

`server/test/e2e*.sh` run on the server against one role's real container
(`[staging|production]`, default staging); `e2e-fit.sh` reads the bars in pixels on
the real page, with the script and without it.

The room service has its own four, run on the server where it runs (see
`room/README.md`):

```bash
python3 /opt/open77-room/test/e2e-room.py staging             # 15 checks: state, pages, profiles, navigation, Android
bash /opt/open77-room/test/e2e-game-request.sh staging        # 5 checks: the game asks, the room puts it up
bash /opt/open77-room/test/e2e-android-input.sh staging       # 5 checks: a click and typing, asserted in the emulator
bash /opt/open77-room/test/e2e-profile-persistence.sh staging # 14 checks: the container is recreated, the profile stays
```

Seen in game, 2026-10-02: the browser cinema from the TV menu, the picture over
UDP, F8 driving the browser. A Ctrl+V paste is verified end to end on the server;
in game it is the next thing to confirm.
