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

Chat: `/watch netflix <link> [title]`, `/watch cinema <link> [100]` (starts the
party AND puts it on a screen -- one command from a link to a cinema), `/watch
play | pause | seek 1:02:03 | +30 | -10 | join <code> | leave | stop | link |
status | android | help`, `/watch cinema [100]` (screen for the party you are in),
`/watch android` (the emulator on the room's screen, below).

## Its own cinema screen

`/watch cinema` puts the player's party on the room's 150 ft panel in front of
them (`cinema.150ft` in open77_media -- the open77-TV-ROOM record; `100` for the
100 ft one, and a Cinema screen button in the panel). The same panel from the TV
menu shows the room's home screen; a party page reaches it because the record
names `linkClaimFrom = { "opx_watchparty" }`, which makes open77_media ask this
resource to vouch for the page the spawn brought (`claim` export). /watch cinema
names the player's own party, so it works even with several running. Either way
the film never appears on the panel -- every viewer plays it in their own
browser, on their own Netflix account.

## Android on a cinema screen

The panel has an **Android screen** button (and `/watch android` does the same):
the Android emulator on the *room's* screen. That is the shared browser cinema on
the game server -- the emulator runs on that host too, so the room's own Chromium
is the one screen that can reach its stream, and the television already shows the
room over WebRTC. Nothing of the room's link is involved, and no player is handed
a URL.

How it travels: the game records a request (`{ id, by, at }`, the id only goes up)
and the room service on the host polls `GET /v1/pending` every two seconds. A new
id and the room starts the emulator if it is stopped, then swaps its tab to
ws-scrcpy's stream with `captureKeyboard=true` -- so a game played there takes the
mouse **and** the keyboard. A room that restarts adopts whatever id it finds
instead of replaying an old request.

Controls: only a party's **host** (or a player with no party) may take the room's
screen; a guest is told whose party it is rather than quietly interrupting the
film, and the panel hides the button for them. From the machine itself,
`POST /v1/android/selftest` makes the same request (that is what the end-to-end
test drives), and it is refused from anywhere but loopback.

## Where the secrets come from

Nobody signs in anywhere shared: each viewer signs in to their own Netflix
account in their own browser, and the extension only keeps that browser's player
where the party is. A member's browser key travels in the link's fragment (never
sent over the wire), is wiped from the address bar on arrival, dies when the
membership ends, and moves the party at most 8 times in 10 s. Party codes and
keys come from a generator seeded once at resource start from the millisecond
timer mixed with a heap address -- the sandbox withholds io/os/debug, so there
is no wall clock or urandom to read. That is prediction resistance for 60-bit
short-lived single-scope secrets, documented here as not-a-CSPRNG, and the
tests hand the registry its own dice so production seeding never touches them.

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
| `tests/` | `lua tests/run.lua` (108 checks); `tests/e2e_bridge.lua` serves the extension's end-to-end test |

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
| `GET /v1/pending` | the newest "Android on the room's screen" request: `{ id, by, at }`, id only goes up |
| `POST /v1/android/selftest` | the same request, from the server machine only (never through the web server) |
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
