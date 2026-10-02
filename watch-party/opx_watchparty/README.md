# opx_watchparty -- Netflix watch parties, in step, on everyone's own account

A watch party is a Netflix title and one shared playhead. **The film plays in each
viewer's own browser** (Edge or Chrome), signed in to their own Netflix account,
through the *Open77 Watch Party* extension, which keeps that browser where the party
is. In the game, the television the party is on shows what is playing, where the
film is and how many browsers are in step; `/watch` is the remote.

Nothing of the film is captured, re-encoded or relayed onto a game surface or to
another player. Netflix's video is protected and stays that way: a party shares only
the title number and where the film is.

## Using it

1. At a television (spawn one with the TV remote, F5), open **/watch**, paste a
   Netflix link (open the film in your browser, copy the address) and press
   **Start a watch party**. The TV switches to the party's screen.
2. Everyone else at the TV opens **/watch** and presses **Join this party**
   (or `/watch join <code>` from anywhere).
3. Each viewer presses **Copy browser link** and pastes it into Edge or Chrome,
   with the extension installed (see `opx_watchparty_extension/README.md`). Their
   Netflix opens the same title at the same moment and stays in step.
4. Anyone in the party moves it -- from the panel (play/pause, -30/-10/+10/+30, the
   bar, a typed time), from chat, or from their own browser's player: everyone
   follows within about two seconds.

Chat: `/watch netflix <link> [title]`, `/watch play | pause | seek 1:02:03 | +30 |
-10 | join <code> | leave | stop | link | status | help`.

## How it is put together

| Path | What |
|---|---|
| `shared/playhead.lua` | the shared playhead: pure arithmetic (play, pause, seek, the film's end, a follower's decision, times as typed) |
| `shared/netflix.lua` | a Netflix title number from any link shape; the browser link (title, time, and `#opxwatch=CODE.key`) |
| `server/party.lua` | the parties: members, browser keys, the television each is on, browsers' reports, tidying |
| `server/http.lua` | what a browser can ask (below) |
| `server/tvpage.lua` | the page a television shows (served by this resource) |
| `server/main.lua` | wiring: chat commands, the panel's requests, `Open77.http.listen`, the state every client draws |
| `client/main.lua` | the panel, which television a party goes on (open77_media's set list), "Copy browser link" |
| `web/panel.*` | the /watch remote |
| `tests/` | `lua tests/run.lua` (75 checks); `tests/e2e_bridge.lua` serves the extension's end-to-end test |

The television: open77_media owns the sets. A party started at one points that set at
`<public base>/v1/tv/<CODE>` through open77_media's own control message (the request its
remote sends when a link is pasted), and hands it back when the host ends the party.

### What a browser can ask

Served on the server's own inbound HTTP (`httpHandlers` in server.jsonc, loopback),
published under HTTPS by the web server in front of it
(`opx_watchparty_public_base`, default `https://xbuniverse.duckdns.org/opx-watch-xb-staging`):

| | |
|---|---|
| `GET /v1/health` | up, version |
| `GET /v1/party/<CODE>` | title, playing, position, length, the server's clock, who is in step |
| `POST /v1/party/<CODE>/report` | a browser's own position; answered with the party |
| `POST /v1/party/<CODE>/control` | `{ key, action: play/pause/toggle/seek/nudge, positionMs, deltaMs }` -- a member's browser, by its key (8 per 10 s) |
| `GET /v1/tv/<CODE>` | the television page |
| `POST /v1/selftest` | a two-minute test party with a key; from the server machine only, never through the web server |

Reading a party needs only its code (it is on the TV). Moving one needs a member's
key, which the server issues per player and per party and revokes when they leave.

## Server requirements

- `"httpHandlers": { "enabled": true, "listenUrl": "http://127.0.0.1:11843", "timeoutSeconds": 5 }`
  in server.jsonc (staging uses 11843), and the web server forwarding
  `/opx-watch-xb-staging/` to `http://127.0.0.1:11843/opx_watchparty/` with
  `X-Real-IP` / `X-Forwarded-For` set.
- `open77_media` (the televisions). Without it, parties still run, just with no screen.
- Without the HTTP listener, parties run in game (panel, chat) but browsers cannot
  follow and the TV page cannot load; the start banner says which.
