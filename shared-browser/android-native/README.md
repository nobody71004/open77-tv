# Android that is not emulated

A second Android for the rooms, on the same game server, next to the emulator.
This one is not a virtual machine: it is **Waydroid**, the Android system image
running as a container on this host's own kernel (`binder_linux`), so the CPU and
the memory are the host's, not a qemu guest's.

It is additive. The emulator (`open77-android-emulator.service`, Android 14 under
qemu, ws-scrcpy on port 8000) is untouched and still runs; this stack listens on
**8001** and the room is switched between them with one command.

```
 the room's Chromium  ──http/ws──►  noVNC + websockify  (172.17.0.1:8001)
                                    (screen.html: a canvas, no toolbar)
                                        │  RFB over WebSocket
                                        ▼
                                    wayvnc  (127.0.0.1:5900)
                                        │  wlr-screencopy
                                        ▼
                     sway, headless, pixman  (1280x720, no GPU)
                                        │  a Wayland client
                                        ▼
                          Waydroid's UI  ──►  Android 13 (LineageOS 20, GAPPS)
                                              on this host's kernel, no VM
```

## Why VNC and not scrcpy

The emulator's screen is scrcpy: the device encodes H.264, ws-scrcpy carries it,
the room's Chromium decodes it with Broadway. That path does not work here, and
the reason is the box, not the software: **this VM has no GPU** (only
`bochs-drm`), so the emulator renders with SwiftShader and Waydroid has nothing
to hand a hardware encoder.

Measured on 2026-10-22, scrcpy against the Waydroid device:

```
[server] ERROR: Encoding error: android.media.MediaCodec$CodecException: Error 0x80001001
[server] INFO: Retrying with -m1024...      (same error)
[server] INFO: Retrying with -m800...       (same error)
ERROR: Recording failed
```

and Waydroid freezes the container on the same attempt (`waydroidhardware:
Received transaction: 3` → `lxc-freeze`). So the native screen is a **framebuffer**,
not a video stream: sway draws into memory with pixman, wayvnc sends the changed
rectangles, and noVNC paints them on a canvas in the room's Chromium. No encoder,
no decoder, nothing that needs a GPU. It is also why this stack has no
`captureKeyboard` switch to get wrong: noVNC sends the pointer and the keyboard
because that is all VNC does.

## The page the room opens: `screen.html`

noVNC ships `vnc.html` and `vnc_lite.html`, and both are pages for a desktop
browser: a control bar above the canvas with the connection status, a "Send
CtrlAltDel" button and a settings panel, and "Connecting" text over the canvas
while it dials. On a television that is browser furniture in front of a player who
asked for Android, and it takes its height off the picture.

So the room opens `screen.html` from this directory, installed next to noVNC's own
pages as `/usr/share/novnc/open77-screen.html`. It keeps noVNC's RFB client
(`core/rfb.js`), its pointer and keyboard handling, scaling to the window and its
reconnect; it drops the bar, the buttons and the "Connecting" text. One canvas,
the whole window, and the keyboard focused after every (re)connect -- a TV has no
window manager to give the page focus after a restart.

Coordinates are not the reason. Measured 2026-10-23 on both pages: a tap computed
from Settings' own search-bar bounds opens Settings' search on either one, because
noVNC maps the pointer through the canvas's own rect. The bar costs picture, not
accuracy.

## What is installed where

| piece | what it is |
|---|---|
| `/var/lib/waydroid` | the Android image (LineageOS 20 = Android 13, **GAPPS**, so Play Store works), 3 GB |
| `/opt/open77-android-native/sway.conf` | the display: 1280x720, pixman, Android fullscreen |
| `/opt/open77-android-native/wayvnc.conf` | loopback:5900, no password (the firewall is the lock) |
| `/opt/open77-android-native/adb-connect.sh` | connects this host's adb to the device, waits for boot |
| `/usr/share/novnc/open77-screen.html` | the page the rooms open: noVNC's RFB client, no toolbar |
| `screen.html` | that page's source, installed on every `install.sh` run |
| `install.sh` | does all of the above, idempotently |
| `switch-room.sh` | points the rooms here, or back at the emulator |

| unit | what it does |
|---|---|
| `open77-android-native-display` | sway, headless (`WLR_BACKENDS=headless`, `WLR_RENDERER=pixman`) |
| `open77-waydroid-session` | `waydroid show-full-ui` under `dbus-run-session` — the session *and* the window |
| `open77-android-native-vnc` | wayvnc on 127.0.0.1:5900 |
| `open77-android-native-web` | websockify + noVNC on 0.0.0.0:8001 |
| `open77-android-native-adb` + `.timer` | keeps adb connected to the device (every 30 s) |
| `open77-android-native-firewall` | admits `docker0` to 8001, nothing else |

Port 8001 is no more open to the world than 8000 is: the host's INPUT policy is
`drop`, and the one rule lets the neko rooms on `docker0` in.

## Switching the rooms

```bash
bash /opt/open77-android-native/switch-room.sh native      # Waydroid, noVNC on :8001
bash /opt/open77-android-native/switch-room.sh emulator    # the qemu emulator, ws-scrcpy on :8000
bash /opt/open77-android-native/switch-room.sh status       # what the rooms show now
```

It writes (or removes) a systemd drop-in for `open77-room` and restarts only that
service. The two Android stacks keep running either way; what changes is which
one the televisions are pointed at. The room service reads all of it as
environment (`ANDROID_PORT`, `ANDROID_PAGE_PATH`, `ANDROID_UDID`,
`ANDROID_SERVICES`, `ANDROID_LABEL`), so nothing is patched to switch.

## What was measured

The screen itself can be read back with `python3 ../../tools/vnc-shot.py
127.0.0.1:5900 out.png` on the host: a minimal RFB client (no auth, raw
encoding), there because this box has no image tooling installed.

2026-10-22, the stack:

- `waydroid status`: Session RUNNING, Container RUNNING, Android 13 (SDK 33),
  `192.168.240.112:5555`, 172 packages with GMS.
- The framebuffer is 1280x720 of Android at fullscreen — opened through VNC and
  read back pixel for pixel (the Launcher, the clock, the dock).
- The room's own path: `POST /api/action {"room":"staging","action":"open","url":"android"}`
  → websockify logged `172.17.0.2 … Path: '/websockify' … connecting to 127.0.0.1:5900`,
  i.e. the room's Chromium attached to the framebuffer.
- The emulator still answers exactly as before when the rooms are switched back
  (`8000`, `emulator-5554`, the same scrcpy link).

2026-10-23, the page and the rooms, with the rooms on this screen:

- `test/e2e-android-input.sh staging`: 5 checks, 0 failed — a click computed from
  Settings' own `search_action_bar` bounds opens Settings' search, and "wifi"
  typed at the X keyboard is in Android's field and filtered the results (read
  back with `uiautomator`, not asked of the browser).
- The pointer is drawn by Android where the pointer is: with the room showing this
  page, `xdotool mousemove 640 400` puts Android's cursor at (640, 400) in the
  framebuffer, and `mousemove 196 68` puts its I-beam in Settings' search field.
- `test/e2e-room.py staging`: 15 checks, 0 failed, including that the Android tile
  is `/open77-screen.html` and that opening it puts that tab on the screen.
- `test/e2e-game-request.sh staging`: 5 checks, 0 failed — the game's request, the
  room's poll, the native page on the screen, and back to the splash.
- **A click with no pointer motion before it is delivered at (0,0).** X sends no
  motion event when the pointer already sits on the target, and Chromium then
  hands the page a press with no position, so noVNC sends the top-left one: on
  Settings' home that is a tap on the status bar, and the notification shade
  opens. It is the same on noVNC's `vnc.html` and on `screen.html`, so the tests
  move the pointer away and back before clicking; it is not something the page
  can see or correct.

## What it does not do

- **ARM-only Play apps.** This image's `ro.product.cpu.abilist` is `x86_64,x86`.
  The emulator's Play Store image also advertises `arm64-v8a` and translates, so
  an arm64-only game installs there and will not install here without a
  translation layer (`libndk`/`libhoudini`, not installed).
- **Sound.** Neither stack carries audio into the room today; this one does not
  mount a PulseAudio socket (a placeholder exists because Waydroid's container
  config bind-mounts that path unconditionally).
- **A GPU.** Rendering is pixman on the host CPU. It is native Android, not a
  graphics-accelerated Android.

## Rolling back

```bash
bash /opt/open77-android-native/switch-room.sh emulator
```

Nothing is uninstalled by that. To remove the stack entirely, stop and disable
the five units above, `apt purge waydroid sway wayvnc novnc websockify`, and
delete `/var/lib/waydroid` (3 GB) and `/opt/open77-android-native`.
