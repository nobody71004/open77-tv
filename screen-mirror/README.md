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
`deploy/install-relay.sh` (one room's own — see the next section).
`deploy/install-f2077.sh` is the bespoke script that did f2077's before
`install-relay.sh` existed; both write the same thing.

## The relays on the game box

The xbuniverse box runs **three** of these relays, one per room that has a Mirror
app, so that a link handed to one server's players cannot join another server's
channel. They are separate processes with their own port, path, token and channel
— nothing is shared between them but the `mirror` user and the TURN/STUN settings:

| | shared (XyberRP) | f2077 | fr-us |
|---|---|---|---|
| service | `open77-mirror.service` | `open77-mirror-f2077.service` | `open77-mirror-fr-us.service` |
| code | `/opt/open77-screen-mirror` | `/opt/open77-screen-mirror-f2077` | `/opt/open77-screen-mirror-fr-us` |
| listens on | `127.0.0.1:8787` | `127.0.0.1:8788` | `127.0.0.1:8789` |
| public path | `/screen-mirror/` | `/screen-mirror-f2077/` | `/screen-mirror-fr-us/` |
| channel | `WORK` | `F2077` | `FRUS` |
| token file (root-only) | `/root/.open77-mirror-token` | `/root/.open77-mirror-f2077-token` | `/root/.open77-mirror-fr-us-token` |
| feeds the room of | `open77-xbuniverse` | `open77-f2077` | `open77-freeroamusa` |
| installed by | `deploy/install-live-nginx.sh` | `deploy/install-f2077.sh` | `deploy/install-relay.sh fr-us 8789 /screen-mirror-fr-us FRUS /opt/open77-freeroamusa` |

Each one writes that server's own `resources/opx_screenmirror/server/config.lua`
(0600 root:root, previous file kept under `/opt/open77-backups`), which is the
file the room service reads for its Mirror app.

**Why the token has to be per server.** The token is the only thing that makes a
channel unwatchable by everyone who merely knows its name. With one token for two
servers on the same box, a player handed f2077's mirror link could join XyberRP's
channel by guessing `WORK` — and the reverse. Each token lives in a root-only file
in `/root` and never lands in this repository. `install-f2077.sh` and
`install-relay.sh` print their links with a placeholder for the token;
`install-live-nginx.sh` prints the full sender and viewer URLs once, at install —
treat that scrollback as a secret.

All three run as the unprivileged `mirror` user and read their TURN/STUN settings
from `/etc/open77-mirror.env`; each is mounted in front of the same nginx site,
additively, by inserting its `location` before the final 404. They differ in how
far they go: `install-f2077.sh` and `install-relay.sh` back the site up to
`/opt/open77-backups`, put it back if `nginx -t` refuses the new block, and then
prove the new path answers through nginx (and that the other paths still do);
`install-live-nginx.sh` backs the site up beside itself, runs `nginx -t` and
reloads, and proves only the relay's own loopback `/health` — so check
`https://xbuniverse.duckdns.org/screen-mirror/health` after running it.

Writing the game's side is the one thing they do differently: `install-f2077.sh`
and `install-relay.sh` write their server's `opx_screenmirror` config from the
token themselves, while `install-live-nginx.sh` only installs the relay and prints
the sender and viewer links — XyberRP's config was written from that token when
the relay was first set up. Restarting the game server is what picks it up (and,
the first time, what starts `opx_screenmirror` at all: a server whose
`resources.load` list does not name it will not discover it, which is why the
resource has to be added to that list as well as installed).

A difference worth knowing before re-running one: `install-f2077.sh` and
`install-relay.sh` **reuse** their token file when one exists, while
`install-live-nginx.sh` takes `MIRROR_TOKEN=` or generates a new one — re-running
it as-is rotates the shared token, so XyberRP's config has to be written again
from the new one (the other relays and their tokens are not touched either way).

Health, one line each, from the box (all three measured 200 on 2026-10-23, and
`/healthz` still answering):

```bash
curl --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/screen-mirror/health
curl --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/screen-mirror-f2077/health
curl --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/screen-mirror-fr-us/health
```

In game, the mirror is an app on the room itself: open the room's home screen on
the television (the `open77-TV-ROOM` records, `cinema.100ft` / `cinema.150ft`) and
press **Mirror**. The room service reads whichever server's config that room
belongs to — `WORK` on XyberRP, `F2077` on f2077, `FRUS` on fr-us — and opens the
receiver inside the room, so the token is never in the page's HTML. Every room
that has a config has the app; a room that has none (a new one, before its relay
is installed) simply has no Mirror app. The capture cinema records are gone: the
room is the screen.

## Using it

1. On the **work PC**, open the sender URL in a browser and pick the screen:
   `https://<host>/?channel=WORK&token=<token>&source=screen`
2. In the **game**, press **Mirror** on the room's home screen (spawn one of the
   `open77-TV-ROOM` panels first) — or open the receiver directly:
   `https://<host>/receiver?channel=WORK&token=<token>`

Both ends must share the same `channel` and `token`. One sender per channel; any
number of viewers.

## Game integration

Done, in the room. `shared-browser/room/server.mjs` reads the `url` out of this
server's `opx_screenmirror/server/config.lua` and answers `/mirror?room=<role>`
with a redirect to it; the room's **Mirror** app is that route. The room's own
Chromium is the client, so the picture is on the television the same way the
room's page is, and the room is the only thing that ever sees the link. Tested
with `room/test/e2e-room.py`, which asserts the redirect (https, right channel)
and that the app is absent where no config exists.

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
