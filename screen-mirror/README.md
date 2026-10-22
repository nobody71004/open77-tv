# open77 screen mirror

Watch **your own PC's screen, view-only, on the in-game television** — from
anywhere over the internet. You work on a second PC ("work") and its screen
appears at the game's TV, the same way the shared browser does.

This is **remote screen mirroring / remote desktop viewing**, like Parsec,
Moonlight or Discord screen-share. It captures the **normal desktop output**.

> **DRM note (important, and on purpose).** Protected video (Netflix and the
> other DRM services) shows **black** here: the OS protected output path blocks
> standard capture by design. Nothing in this module bypasses that. For watching
> those together, use the watch party (`../watch-party/`), where everyone plays
> in their own browser. This mirror is for your *own* content — your work,
> code, docs, a game you're playing.

## How it works

```
  your "work" PC                    small VPS                     in the game
  ┌─────────────┐    JPEG frames   ┌─────────────────┐   JPEG    ┌──────────────┐
  │ sender.html │ ── WebSocket ──▶ │ relay.mjs       │ ────────▶ │ receiver.html│
  │ (capture +  │   (outbound)     │ (relays frames, │  WebSocket│ (drawn on    │
  │  send)      │                  │  stores nothing)│  (outbound│  the TV)     │
  └─────────────┘                  └─────────────────┘           └──────────────┘
```

- **sender.html** — runs on the PC you want to watch. Captures the screen
  (`getDisplayMedia`) and pushes JPEG frames over one WebSocket.
- **relay.mjs** — runs on a VPS. Relays each frame from the one sender to every
  receiver of the same channel. It never looks at or stores frames. Both ends
  connect **out** to the VPS, so neither needs an open port or NAT rules — that
  is exactly why one small VPS works for "watch my screen over the internet."
- **receiver.html** — the "client page" the game's television frames (the same
  slot the shared browser uses). It joins a channel and draws what arrives.

**View-only.** Frames travel sender → relay → receiver only. No keystrokes, no
mouse, no control goes back. This is a mirror, not remote control.

## Why a JPEG-frame relay (and not WebRTC) for now

You chose **view-only**, so moderate latency is fine and a plain frame relay is
the simplest thing that reliably works across the internet through a VPS.
Latency is on the order of a frame interval + network (roughly 0.1–0.5 s), and
bandwidth scales with fps/quality. If you want **low latency / sharper text**,
the upgrade is WebRTC (H.264) with a TURN server on the same VPS — the game
already receives a WebRTC stream from the shared browser, so the receiver slot
fits it. Say the word and I'll switch the transport.

## Quick local test (already verified)

Proves the whole loop — sender → relay → receiver → a live, drawn picture — in a
real browser, no network, no VPS:

```
node screen-mirror/test.mjs
```

It starts the relay on a spare port, opens the sender in test-pattern mode (no
screen permission needed) and the receiver in Chromium, and asserts the receiver
draws frames, that they advance (not a stuck image), and that the canvas is not
blank. Current result: **PASS** (frames 5 → 20, ~92% of pixels lit).

The optional shared-secret gate is checked too: a wrong `?token=` is rejected,
the right one joins.

## Deploy to a VPS

HTTPS is required (the sender captures with `getDisplayMedia`, which browsers
only allow in a secure context), so the relay is fronted by Caddy for automatic
TLS. On a Debian/Ubuntu VPS, from the copied `screen-mirror/` directory:

```
sudo MIRROR_TOKEN=$(openssl rand -hex 16) ./deploy/up.sh mirror.yourdomain.com
```

That installs Node if needed, copies the relay to `/opt/open77-screen-mirror`,
installs `ws`, runs it as an unprivileged user under systemd, and puts Caddy in
front. It prints the sender and receiver URLs with the token. Files:

- `deploy/up.sh` — the installer (safe to re-run).
- `deploy/relay.service` — systemd unit (hardened, unprivileged).
- `deploy/Caddyfile` — Caddy 2 HTTPS + WebSocket proxy.

For the game box, which already has nginx on `xbuniverse.duckdns.org` instead of
Caddy, the installers are `deploy/install-live-nginx.sh` (the shared relay) and
`deploy/install-f2077.sh` (f2077's own — see the next section).

## The two relays on the game box

The xbuniverse box runs **two** of these relays, one per game server, so that a
link handed to one server's players cannot join the other server's channel. They
are separate processes with their own port, path, token and channel — nothing is
shared between them but the `mirror` user and the TURN/STUN settings:

| | shared (XyberRP) | f2077 |
|---|---|---|
| service | `open77-mirror.service` | `open77-mirror-f2077.service` |
| code | `/opt/open77-screen-mirror` | `/opt/open77-screen-mirror-f2077` |
| listens on | `127.0.0.1:8787` | `127.0.0.1:8788` |
| public path | `https://xbuniverse.duckdns.org/screen-mirror/` | `https://xbuniverse.duckdns.org/screen-mirror-f2077/` |
| channel | `WORK` | `F2077` |
| token file (root-only) | `/root/.open77-mirror-token` | `/root/.open77-mirror-f2077-token` |
| feeds the cinemas of | `open77-xbuniverse` (`/opt/open77-xbuniverse/.../opx_screenmirror/server/config.lua`) | `open77-f2077` (`/opt/open77-f2077/.../opx_screenmirror/server/config.lua`) |
| installed by | `deploy/install-live-nginx.sh` | `deploy/install-f2077.sh` |

**Why the token has to be per server.** The token is the only thing that makes a
channel unwatchable by everyone who merely knows its name. With one token for two
servers on the same box, a player handed f2077's capture link could join XyberRP's
channel by guessing `WORK` — and the reverse. Each token lives in a root-only file
in `/root` and never lands in this repository. `install-f2077.sh` prints its links
with a placeholder for the token; `install-live-nginx.sh` prints the full sender
and viewer URLs once, at install — treat that scrollback as a secret.

Both relays run as the unprivileged `mirror` user and read their TURN/STUN
settings from `/etc/open77-mirror.env`; both are mounted in front of the same
nginx site, additively, by inserting their `location` before its final 404. They
differ in how far they go: `install-f2077.sh` backs the site up to
`/opt/open77-backups`, puts it back if `nginx -t` refuses the new block, and then
proves the new path answers through nginx; `install-live-nginx.sh` backs the site
up beside itself, runs `nginx -t` and reloads, and proves only the relay's own
loopback `/health` — so check `https://xbuniverse.duckdns.org/screen-mirror/health`
after running it.

Writing the game's side is the one thing the two installers do differently:
`install-f2077.sh` writes f2077's `opx_screenmirror` config from its token itself
(0600 root:root, previous file kept under `/opt/open77-backups`), while
`install-live-nginx.sh` only installs the relay and prints the sender and viewer
links — XyberRP's config was written from that token when the relay was first set
up. Restarting the game server is what picks either up.

A difference worth knowing before re-running either: `install-f2077.sh` **reuses**
its token file when one exists, while `install-live-nginx.sh` takes
`MIRROR_TOKEN=` or generates a new one — re-running it as-is rotates the shared
token, so XyberRP's config has to be written again from the new one (the f2077
relay and its token are not touched either way).

Health, one line each, from the box (both measured 200 on 2026-10-22, and
`/healthz` still answering):

```bash
curl --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/screen-mirror/health
curl --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/screen-mirror-f2077/health
```

In game, the capture cinemas (`cinema.100ft.capture`, `cinema.150ft.capture`,
`linkFrom = "opx_screenmirror"`) are put up with whichever server's config that
server runs: `WORK` on XyberRP, `F2077` on f2077.

## Using it

1. On the **work PC**, open the sender URL in a browser and pick the screen:
   `https://<host>/?channel=WORK&token=<token>&source=screen`
2. In the **game**, show the receiver at the TV:
   `https://<host>/receiver?channel=WORK&token=<token>`

Both ends must share the same `channel` and `token`. One sender per channel; any
number of viewers.

## Game integration (the last mile)

The receiver page is deliberately shaped like the shared-browser client page the
game already frames, so the wiring is small: teach the television to recognize a
screen-mirror link and show it with `showSharedBrowser` (which frames a client
page), pointed at the receiver URL. That touches `open77_media/web/ttv.js`
(classify + show) and the server link handling, and needs a real in-game pass to
verify — I've left it as the next step so this core could be proven first.

## Security & privacy

- `MIRROR_TOKEN` (optional but recommended on a public VPS) gates every join; the
  channel name alone is otherwise the secret (like the shared browser's link).
- The relay stores nothing and doesn't decode frames; it forwards bytes.
- TLS end to end (Caddy). View-only, so a viewer can't drive the remote PC.
- Anyone with the link + token can watch that channel — treat the link as a
  password for whoever should see the screen.

## Limits (honest)

- **Latency/bandwidth**: fine for watching work; not for interactive control
  (that would want WebRTC + also a control channel, and is out of scope here).
- **One sender per channel**; a reconnecting sender supersedes the old one.
- **Text legibility** depends on JPEG quality/fps; tune `?quality=&fps=` (e.g.
  `quality=0.85&fps=15` for code). WebRTC would improve this.
- **VPS bandwidth** carries every frame (it's a relay, not P2P); size the VPS for
  your fps × quality × viewers.
