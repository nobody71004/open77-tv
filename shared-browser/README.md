# The shared browser: a real Chromium on a television

The game's own browser cannot play most video sites: it has no H.264/AAC decoder
and no HLS demuxer (see "What actually plays" in the top README). The shared
browser puts a real Chromium on the server instead and streams its picture and
sound to a television over WebRTC, as VP8 and Opus, which the game's browser does
play. Everyone at that television sees the same browser; whoever presses F8 at
the screen drives it with the mouse and keyboard.

```
opx_tvbrowser/   the resource: /browser puts it on the TV in front of you,
                 /browser cinema puts up a 150 ft cinema showing it (/browser
                 cinema 100: a 100 ft one), /browser off; and the link the TV
                 menu's two browser cinemas are put up with
server/          the container beside the game server: neko's Chromium image
                 without Widevine, its browser policies, the two scripts the
                 image puts in the client page, and the scripts that build it,
                 publish it and check it
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
puts it on the stream's media element. The client keeps a volume of its own (saved
in the browser, and copied back from the element whenever the element's changes),
so the script also puts the television's level back whenever anything else moves
it; the client then saves the television's level as its own. Each set has its own
level, as every television does. The page reports where the level went
(`volume_applied`/`mute_applied ... via the shared browser's page`) and the client
page what it did (`browser_ice (volume 35 on 1 element ...)`), when either changes.

Before this the slider and the mute button changed nothing at all on the shared
browser: they were wired to YouTube's player and to plain video only.

## When the picture does not come

The stream is WebRTC: UDP to the server's port 59100 first, and TCP to the same
port when UDP answers do not come back (Chromium tries that by itself). The page
says what happened, in the client log:

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

## No DRM, on purpose

This browser's picture is captured and streamed to players, so protected video
must not be able to play in it at all. The neko image ships Widevine; the
`Dockerfile` removes it and the policy turns component updates off, so it is not
fetched back. `server/drmcheck.sh` asks both images (stock and ours) for the
Widevine key system: on XBUNIVERSE staging, 2026-10-02, the stock image answered
`WIDEVINE_AVAILABLE` and ours `NotSupportedError`. Netflix and the other DRM
services do not play in it; for Netflix, use a watch party (`watch-party/`).

## Shared, and what that means

- Anything signed in to on it is seen by everyone at that television.
- A link pasted into it is the shared browser's clipboard afterwards: the next
  person to right-click → Paste gets it.
- `server/up.sh` recreates the container, which wipes the browser's own state
  (sessions, history). Passwords stay in `neko.env`.
- Downloads, file dialogs, developer tools, the password manager and browser
  sign-in are off (`server/build/policies.json`), as are notifications, pop-ups,
  camera and microphone. The one extension is uBlock Origin Lite.
- The viewer password is in the television's link, so every client that shows
  it holds it: it is a password for the players of that server, not a secret
  from them. It never reaches a client as a file: `opx_tvbrowser/server/config.lua`
  is written on the server at deploy and is not in the client package.

## On the server

Done on XBUNIVERSE staging (2026-10-02); the production servers on the same
machine are not touched.

1. Copy `server/` to `/opt/open77-tvbrowser` (root, 700) and run `up.sh`. The
   first run generates `neko.env` (viewer and admin passwords, API token) and the
   television link `tv-url.secret`, both root-only; later runs reuse them, so the
   link does not change. It builds `open77/tvbrowser-chromium:5`, checks the
   image (the three scripts in the client page, the policy, no Widevine), and runs the
   container capped at 4 cores, 4 GB and 2 GB of shared memory, with the client
   on `127.0.0.1:18080` and WebRTC on `59100` (UDP and TCP, published by Docker,
   so the host's INPUT rules are not changed).
2. `nginx.sh` adds `/tv-browser-xb-staging/` (websocket) to the site, after
   backing it up and `nginx -t`.
3. Install `opx_tvbrowser` in `resources/`, write `server/config.lua` from
   `tv-url.secret` (`url`, `cinemaRecord`, `cinema100Record`; root, 600), and add
   `opx_tvbrowser` to `resources.load` in `server.jsonc` (staging loads an
   explicit list).
4. `test/e2e.sh`, `test/e2e-page.sh`, `test/e2e-paste.sh` and `test/e2e-volume.sh`
   run a Playwright Chromium on the server, so it reaches the browser the way a
   player does: logged in from the television link, the picture over the public
   IP (in the four networks above), a link pasted into the address bar and
   opened, and the television's level read back from the stream (2026-10-02, image
   5: 100, then 35, 35 muted and 75, as asked).

Staging went through five builds of `server/build/`, each a recreate with the
same `neko.env`, ports and link, the one before kept for rollback: 1, Widevine
removed and the policies; 2, `open77-ice.js` and the health check at the path the
server answers on (neko's own asked `/health`, which a server with a path prefix
answers under the prefix, so a healthy container reported unhealthy); 3,
`open77-paste.js`; 4, the paste notes and reports, and new tabs opening Google; 5,
`open77-volume.js`. Build 5 also makes the policy and the scripts readable whatever
modes the build context had: its first rollout was built from files copied from
Windows, which arrive root-only, so for three minutes (nobody connected) neko
answered 403 for the scripts and Chromium could not read its policies. `up.sh`
checks the modes in the image now.

The picture is 1280x720 at 30 fps (`NEKO_DESKTOP_SCREEN`); each viewer is one
WebRTC stream from the server.

## Tests

```bash
cd opx_tvbrowser && lua5.4 tests/run.lua    # the link rule, the commands, which TV, the export (33 checks)
node server/test/ice-local.mjs              # open77-ice.js against real peer connections (16)
node server/test/paste-local.mjs            # open77-paste.js: keys, socket, notes, reports (31)
node tests/tv-page/run.mjs                  # from the repository root: the page's half, volume included (54)
```

`server/test/e2e*.sh` run on the server against the real container.

Seen in game, 2026-10-02: the browser cinema from the TV menu, the picture over
UDP, F8 driving the browser. A Ctrl+V paste is verified end to end on the server;
in game it is the next thing to confirm.
