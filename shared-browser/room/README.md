# open77-room -- the page a shared browser opens on

A browser cinema is a real Chromium on the server, streamed to a television.
This service is the *page* that Chromium opens on, and the only thing that can
change what it is showing:

| | |
|---|---|
| `web/splash.html` | the home screen: a rail of app icons, a shelf describing the focused one, a clock, and the who's-watching sheet |
| `web/profiles.html` | the settings screen: the profile list, the selected one's detail, its actions, and create |
| `web/inter.woff2` | the typeface both pages are drawn in, served by this service |
| `server.mjs` | the HTTP API those two pages call, the profile library on disk, and the room's own DevTools client |
| `test/e2e-room.py` | the service end to end: state, pages, profile lifecycle, navigation, Android, the mirror (`18 checks`) |
| `test/e2e-game-request.sh` | a game-asked Android request reaching the room's screen (`5 checks`) |
| `test/e2e-android-input.sh` | mouse and keyboard through the room into the emulator (`5 checks`) |
| `test/e2e-profile-persistence.sh` | a profile, its cookies and Chromium itself survive `up.sh` recreating the container (`14 checks`) |

It listens on `0.0.0.0:8002` on the game server (loopback and the Docker bridge
only; the host's firewall has no rule for it). The room's Chromium reaches it at
`http://172.17.0.1:8002/?room=<role>`, and that URL is the browser's start page
(`build/policies.json`), so a cinema that is put up lands on the splash by itself
-- nothing has to be injected into the page.

## The home screen

It is drawn as a television's home screen, because that is what it is: the first
thing a browser cinema shows, and the thing a player stands in front of. One rail
of large app icons, a wide shelf under it that describes whatever is focused, a
clock, and the profile picker behind the avatar. There are no image assets -- each
icon is a brand colour, a wordmark or an inline SVG -- and no blur, because the
room's Chromium paints on a GPU-less host.

- **The rail.** **Chrome** first -- the web, on the room's own browser, in a
  window with no address bar; then one icon per service, with a green dot when this
  profile has a session on it; then **Mirror** where the room's server has one;
  then Android; then Settings. Signing in from here is signing in on the
  television: the session lives in the profile's Chromium directory on the host,
  so it survives the container being recreated.
- **The mirror.** The room service reads the `url` out of that game server's
  `opx_screenmirror` config (relay page, channel and token) and `/mirror` answers
  with a redirect to it, so the room shows a remote PC's screen with the token
  never in its HTML. All four rooms have one: production and staging share the
  XyberRP relay (`WORK`), f2077 has `F2077`, fr-us has `FRUS`. A room whose server
  has no mirror config simply has no Mirror app.
- **The shelf.** The focused icon's colour washes the shelf, which says what it is
  and what state it is in ("Signed in on this profile", "Not signed in on this
  profile", "Running · <device>", "Stopped"), with **Open** / **Sign in** /
  **Start Android** as one pill. Android starts the emulator if it is stopped.
- **The remote.** Arrows move focus and move the rail under it, Enter opens, Escape
  closes the sheet. The mouse works too (hover is focus); F8 gives the mouse and
  keyboard to the page underneath.
- **Who's watching.** The avatar (or **Manage profiles**) opens the profile sheet:
  one face per profile, the current one marked *Watching*, a click switches the
  room to it. **Manage profiles** goes to the settings page, **Reload the room**
  restarts the room's Chromium.

## Profiles

A profile is a Chromium user-data directory on the host, under
`<install dir>/profiles/`, mounted into the container at `/profiles`. Chromium
runs on `--user-data-dir=/profiles/active`, a symlink: switching is re-pointing
that link and restarting Chromium, so the sign-ins follow the profile and a
recreated container keeps them. `up.sh` seeds `house` on first run and makes the
link `active -> house`.

The library page is drawn as the same television's settings screen: the profiles
on the left with a dot per service and whether it has a session, the selected one's
detail on the right (directory, cookies, size, when it was last used, which
services are signed in, what the room is showing and whether Android is up), and
the actions at the bottom of that detail. **Use** switches to it, **Rename**
renames the directory (the active link follows, so the room keeps running on the
same sign-ins), **Reset** clears the sign-ins and keeps the old directory as a
hidden `.name.reset-<stamp>` backup, **Delete** removes it. The active profile
cannot be reset or deleted; the create field sits under the list.

That the library really is the browser's state, and not a copy of it, is what
`test/e2e-profile-persistence.sh` proves: it writes into the active profile from
inside the container, has `up.sh` throw the container away and make it again, and
finds the marker, the cookie database and the room's Chromium on the same
profile afterwards.

## Android, and how the game asks for it

The emulator runs on the same host (`open77-android-emulator.service`), its screen
is served by ws-scrcpy (`open77-android-web.service`, port 8000) and only a
browser on that host can reach it -- which is exactly the room's Chromium, whose
picture the television already shows.

- The room service polls the game server it belongs to
  (`GET <public base>/v1/pending`, every two seconds). A host pressing **Android
  screen** in the `/watch` panel (or `/watch android`) makes the game record a new
  request id; the room sees a higher id, starts the emulator if it is stopped, and
  swaps its tab to the stream.
- `POST /v1/android/selftest` on the game server's loopback listener makes the same
  request from the machine itself. It is how the end-to-end test drives the whole
  path (and how an operator can put the emulator on a screen from the box), and it
  is refused from anywhere but that machine.
- The request id only goes up, so a room that restarts adopts what it finds
  instead of replaying an old request.

**Mouse and keyboard.** ws-scrcpy leaves keyboard capture off unless the stream URL
says `captureKeyboard=true`; the room appends it. Measured 2026-10-22: without it
the pointer plays and typing does nothing (Settings' search stayed empty); with it
the emulator's field takes the text and filters. `test/e2e-android-input.sh` proves
both through the X server -- the same place neko puts a player's mouse and
keyboard -- and asserts on the emulator's own UI, not on the browser's opinion.

**Which Android.** The service does not hard-code the emulator. Everything it
needs is environment, so a room can be pointed at another stack -- the native one
in `../android-native/` -- without a patch, and pointed back the same way:

| variable | emulator (default) | native |
|---|---|---|
| `ANDROID_PORT` | `8000` | `8001` |
| `ANDROID_PAGE_PATH` | -- (a scrcpy link is built) | `/open77-screen.html` |
| `ANDROID_UDID` | `emulator-5554` | `192.168.240.112:5555` |
| `ANDROID_SERVICES` | `open77-android-web.service open77-android-emulator.service` | the four native units + `open77-android-native-adb.service` |
| `ANDROID_LABEL` | `Android emulator` | `Android (native)` |

The device is asked for **by name**, not "the first thing adb lists", because
this host can have two Android devices connected at once. A device on the network
is re-connected before the room calls it down: an adb server restart (the
emulator's stack starts one) makes adb forget `host:port` devices, and that is
not the same as Android being off.

The native side's `ANDROID_PAGE_PATH` is `/open77-screen.html`, which is
`../android-native/screen.html`: noVNC's RFB client with no control bar, so the
canvas is the whole window instead of the window minus a toolbar.

## The room's own browser, over CDP

The image runs Chromium with a loopback DevTools socket and a `socat` forwarder
(`build/chromium.conf`), so the service can drive the room without publishing a
port. This Chromium (154) answers `/json/list` and lets a browser-level client
list, create, activate and close targets, but refuses `Target.attachToTarget`
("Not allowed") and never replies on a page target's own socket. So an "open" is
done the way a person would: a new tab on the URL, made the active tab (it is the
window the television shows), and the tab it replaces closed -- one tab at a time,
and never closing the last one.

## Install

Copy this directory to the game server and, as root:

```bash
bash ./install.sh          # /opt/open77-room, the systemd unit, `ws`, then restart
```

`install.sh` also installs the tests beside the service. They run where the
service runs:

```bash
python3 /opt/open77-room/test/e2e-room.py staging            # 18 checks: the service and its pages
bash /opt/open77-room/test/e2e-game-request.sh staging       # 5 checks: the game asks, the room shows
bash /opt/open77-room/test/e2e-android-input.sh staging      # 5 checks, needs the emulator
bash /opt/open77-room/test/e2e-profile-persistence.sh staging # 14 checks, recreates the container
```

All four take `staging` (default), `production`, `f2077` or `fr-us`, and each
one's room is that role's container (`open77-tvbrowser-xbs`, `-xb`, `-f2077`,
`-fr-us`). The input test
leaves the emulator on Settings' search page; the room is put back on its splash
by the first test's last step. The persistence test recreates that role's
container (`up.sh <role>`: stop, rm, run from the same image and mounts) and
checks the profile, its cookie database, the active link and Chromium come back
on it -- it writes its marker inside the container at `/profiles/active`, so what
it verifies is the browser's own view of its profile, and it deletes the marker
when it is done.
