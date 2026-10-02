# The shared browser: a real Chromium on a television

The game's own browser cannot play most video sites: it has no H.264/AAC decoder
and no HLS demuxer (see "What actually plays" in the top README). The shared
browser puts a real Chromium on the server instead and streams its picture and
sound to a television over WebRTC, as VP8 and Opus, which the game's browser does
play. Everyone at that television sees the same browser; whoever presses F8 at
the screen drives it with the mouse and keyboard.

```
opx_tvbrowser/   the resource: /browser puts it on the TV in front of you,
                 /browser cinema puts up a 150 ft cinema showing it, /browser off
server/          the container beside the game server: neko's Chromium image
                 without Widevine, its browser policies, and the scripts that
                 build it, publish it and check it
```

The television page frames a link ending in `#open77-shared-browser` as this
browser (`SHARED_BROWSER_MARK` in `open77_media/web/tv.js`): no probe, no frame
check, kept across state updates, closed when the television leaves it.

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

1. Copy `server/` to `/opt/open77-tvbrowser` (root, 700) and run `up.sh`: it
   generates `neko.env` (viewer and admin passwords, API token; root-only),
   builds `open77/tvbrowser-chromium:1`, runs the container capped at 4 cores,
   4 GB and 2 GB of shared memory, with the client on `127.0.0.1:18080` and
   WebRTC on `59100` (UDP and TCP, published by Docker, so the host's INPUT rules
   are not changed), and writes the television link to `tv-url.secret` (root-only).
2. `nginx.sh` adds `/tv-browser-xb-staging/` (websocket) to the site, after
   backing it up and `nginx -t`.
3. Install `opx_tvbrowser` in `resources/`, write `server/config.lua` from
   `tv-url.secret`, and add `opx_tvbrowser` to `resources.load` in `server.jsonc`
   (staging loads an explicit list).
4. `test/e2e.sh` runs a Playwright Chromium on the server with host networking,
   so it reaches the browser the way a player does. Staging, 2026-10-02: logged in
   from the television link, ICE connected over the public IP, 1280x720 picture
   advancing; the browser opens on YouTube.

The picture is 1280x720 at 30 fps (`NEKO_DESKTOP_SCREEN`); each viewer is one
WebRTC stream from the server.

## Tests

```bash
cd opx_tvbrowser && lua5.4 tests/run.lua    # the link rule, the commands, which TV (23 checks)
node tests/tv-page/run.mjs                  # from the repository root: the page's half
```

Not yet seen in game: the television showing the stream, its sound through the
television's speaker, and F8 driving the browser.
