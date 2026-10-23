#!/usr/bin/env bash
# A NATIVE Android next to the emulated one.
#
# The game's Android screen is a qemu VM today (open77-android-emulator.service:
# Android 14 under KVM with SwiftShader, about 5.5 GB of RAM and a third of a
# core before anyone touches it). This installs a second Android that is not a
# VM at all: Waydroid, which runs the Android system image as an LXC container
# on this host's own kernel (binder_linux, already loaded here). Nothing is
# emulated: it is the host CPU, the host memory and the host kernel.
#
# It is ADDITIVE, on purpose: its own port, its own units, its own firewall
# rule, and the emulator left exactly as it was. Nothing else on the box
# changes, so a player who presses Android in the game still gets the old screen
# until the room is pointed at this one (see the end of this file).
#
# WHY VNC AND NOT ws-scrcpy. The emulator's screen is scrcpy's H.264 stream,
# shown by ws-scrcpy. That does not work here: this VM has no GPU (only
# bochs-drm), and Waydroid's software encoder dies on configure --
# "MediaCodec$CodecException: Error 0x80001001", retried at every size, and
# Waydroid freezes the container on the same hardware transaction. So the native
# screen is served as a VNC framebuffer instead: a headless wlroots compositor
# (sway), Waydroid's window inside it, wayvnc sending changed rectangles, and
# noVNC in the room's Chromium. No decoder, no encoder, no H.264. noVNC is a
# page of canvas draws, which is exactly what this Chromium is good at.
#
# AND WHY OUR OWN PAGE. noVNC's stock pages put a control bar above the canvas:
# connection status, a "Send CtrlAltDel" button, a settings panel. On a
# television that is browser furniture in front of a player who asked for
# Android, and it takes its height off the picture. So the room opens screen.html
# from this directory, installed next to noVNC's pages: no bar, no buttons, just
# the screen, the keyboard and the pointer. (The pointer mapping itself is right
# on either page -- measured 2026-10-23 -- the bar costs picture, not accuracy.)
#
# What it does, additively:
#   1. the Waydroid repo and package (1.6.x), with the kernel modules its
#      network script needs loaded at boot
#   2. /var/lib/waydroid -- the LineageOS 20 image with GAPPS, so Play Store
#      works, downloaded once
#   3. a headless sway (1280x720, no GPU, Android fullscreen in it) and
#      Waydroid's own UI on it: open77-android-native-display / -session
#   4. wayvnc on loopback, and noVNC+websockify on 0.0.0.0:8001
#      (open77-android-native-vnc / -web), behind the same Docker-bridge-only
#      firewall rule the emulator's port 8000 has
#   5. the host's adb key accepted inside Android, so the game's own tooling
#      (and the room's "is it up" check) can see the device
#
# USAGE  bash install.sh        (safe to re-run; images and keys are kept)
set -euo pipefail

DEVICE=192.168.240.112          # Waydroid's own address on its LXC bridge
PORT=8001                       # next to the emulator's 8000
WAYLAND=display                 # the stable name the installer gives sway's socket
RUN=/run/open77-waydroid        # XDG_RUNTIME_DIR for the session
NATIVE=/opt/open77-android-native
ADB_BIN=/opt/android-sdk/platform-tools/adb
NOVNC=/usr/share/novnc
PUBLIC_HOST=xbuniverse.duckdns.org

echo "== open77 Android, native: Waydroid on :$PORT next to the emulator on :8000"

# --- 0. the kernel has to be able to host Android --------------------------
# binder_linux is the Android IPC driver; binderfs must be mounted for LXC to
# hand the container its /dev/binder. This box already has both, but a kernel
# upgrade or a fresh image would not, so the modules are declared at boot.
modprobe binder_linux
modprobe ip_tables ip6_tables iptable_filter iptable_nat iptable_mangle nf_nat xt_CHECKSUM
cat > /etc/modules-load.d/open77-waydroid.conf <<'MODULES'
# Android's IPC driver, and the iptables tables Waydroid's network script writes
# into. Its network script prefers iptables-legacy, so the legacy tables are the
# ones that have to exist (measured: without them every rule fails with "Table
# does not exist" and the session dies at "container failed to start").
binder_linux
ip_tables
ip6_tables
iptable_filter
iptable_nat
iptable_mangle
nf_nat
xt_CHECKSUM
MODULES
if ! mountpoint -q /dev/binderfs; then
  mkdir -p /dev/binderfs
  mount -t binder binder /dev/binderfs || echo "!! binderfs did not mount; Waydroid will not start"
fi
echo "-- kernel: $(lsmod | awk '$1=="binder_linux"{print "binder_linux loaded"}'), binderfs $(mountpoint -q /dev/binderfs && echo mounted || echo MISSING)"

# --- 1. the packages -------------------------------------------------------
# sway + wayvnc are the screen path; novnc + websockify put it on the web.
if ! command -v waydroid >/dev/null 2>&1; then
  install -d -m 0755 /etc/apt/keyrings
  curl -fsSL https://repo.waydro.id/waydroid.gpg -o /etc/apt/keyrings/waydroid.gpg
  . /etc/os-release
  echo "deb [signed-by=/etc/apt/keyrings/waydroid.gpg] https://repo.waydro.id/ ${VERSION_CODENAME} main" \
    > /etc/apt/sources.list.d/waydroid.list
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq waydroid sway wayvnc novnc websockify
echo "-- waydroid $(waydroid --version), sway $(sway --version | head -1), wayvnc $(wayvnc --version 2>&1 | head -1)"

# --- 2. the Android image (once) -------------------------------------------
# GAPPS, not VANILLA: the tile promises Play Store. LineageOS 20 = Android 13.
# This is the one step that downloads gigabytes, and it is skipped once
# /var/lib/waydroid/images holds a system image.
if [ ! -f /var/lib/waydroid/images/system.img ]; then
  echo "-- downloading the Android image (LineageOS 20, GAPPS): a few GB"
  waydroid init -s GAPPS -f
fi
echo "-- image: $(du -sh /var/lib/waydroid/images | cut -f1) in /var/lib/waydroid/images"

# --- 3. a display for a machine with no screen -----------------------------
# wlroots' headless backend is a real compositor with no outputs attached: it
# renders into memory with pixman (no GPU here), and Waydroid's UI is a client
# of it. 1280x720 because that is what the TVs show; the Android window is put
# fullscreen so a device coordinate is a screen coordinate.
install -d -m 0755 "$NATIVE"
cat > "$NATIVE/sway.conf" <<SWAY
# The native Android's display. Started by open77-android-native-display.service
# with WLR_BACKENDS=headless and WLR_RENDERER=pixman: there is no GPU in this VM.
output HEADLESS-1 resolution 1280x720
for_window [app_id="Waydroid"] fullscreen enable
# No pointer acceleration games and no bars: this screen is one Android window.
default_border none
default_floating_border none
SWAY

cat > /etc/systemd/system/open77-android-native-display.service <<UNIT
[Unit]
Description=Open77 Android native: headless sway (the display Waydroid draws into)
Documentation=file:$NATIVE/README.md
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
Environment=XDG_RUNTIME_DIR=$RUN
Environment=WLR_BACKENDS=headless
Environment=WLR_LIBINPUT_NO_DEVICES=1
Environment=WLR_RENDERER=pixman
# The PulseAudio socket below is a plain file: Waydroid's container config
# always bind-mounts \$XDG_RUNTIME_DIR/pulse/native, and a missing source aborts
# the whole mount setup ("Failed to setup mount entries") before Android starts.
RuntimeDirectory=open77-waydroid
RuntimeDirectoryMode=0700
ExecStartPre=/bin/mkdir -p $RUN/pulse
ExecStartPre=/bin/sh -c 'test -e $RUN/pulse/native || : > $RUN/pulse/native'
ExecStart=/usr/bin/sway -c $NATIVE/sway.conf
# sway names its socket itself (wayland-1 here) and ignores WAYLAND_DISPLAY, so
# the socket it made gets a stable name that every other unit can use: "display".
# Hardcoding wayland-1 would break the day something else holds that name.
ExecStartPost=/bin/sh -c 'for i in \$(seq 1 30); do s=\$(ls -t $RUN/wayland-[0-9]* 2>/dev/null | head -1); if [ -S "\$s" ]; then ln -sfn "\$(basename "\$s")" $RUN/display; exit 0; fi; sleep 1; done; exit 1'
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
UNIT

# --- 4. the Android session on it ------------------------------------------
# waydroid-container (the LXC) is the package's own unit; this one is the
# session and the window. show-full-ui does both -- it is what makes Android's
# display active, so it replaces "waydroid session start" here.
#
# dbus-run-session is not decoration: there is no login session on this box to
# hand Waydroid a session bus, and without one it dies on "Unable to autolaunch
# a dbus-daemon without a \$DISPLAY for X11".
cat > /etc/systemd/system/open77-waydroid-session.service <<UNIT
[Unit]
Description=Open77 Android native: the Waydroid session (Android 13, GAPPS)
Documentation=file:$NATIVE/README.md
After=open77-android-native-display.service waydroid-container.service network-online.target
Requires=open77-android-native-display.service
Wants=waydroid-container.service network-online.target

[Service]
Type=simple
Environment=XDG_RUNTIME_DIR=$RUN
Environment=WAYLAND_DISPLAY=display
# The compositor's socket exists a moment before it accepts clients.
ExecStartPre=/bin/sh -c 'for i in \$(seq 1 30); do [ -S $RUN/$WAYLAND ] && exit 0; sleep 1; done; exit 0'
ExecStart=/usr/bin/dbus-run-session -- /usr/bin/waydroid show-full-ui
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT

# --- 4b. adb, so the game and the room can see the device -----------------
# The VNC screen does not need adb, but everything else that asks "is Android
# up" does: the room's own check, /watch android, and any tooling. The
# connection belongs to the adb *server*, not to a client, so it is
# re-established here rather than assumed to survive.
cat > "$NATIVE/adb-connect.sh" <<'ADB'
#!/bin/sh
# Connect this host's adb to the Waydroid device and wait until Android has
# finished booting. Python-free, one line of shell: it runs as a service.
export PATH=/opt/android-sdk/platform-tools:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
adb start-server >/dev/null 2>&1 || true
i=0
while [ "$i" -lt 60 ]; do
  adb connect 192.168.240.112:5555 >/dev/null 2>&1 || true
  if [ "$(adb -s 192.168.240.112:5555 shell getprop sys.boot_completed 2>/dev/null | tr -d '
')" = "1" ]; then
    exit 0
  fi
  i=$((i + 1))
  sleep 5
done
exit 0
ADB
chmod 0755 "$NATIVE/adb-connect.sh"
cat > /etc/systemd/system/open77-android-native-adb.service <<UNIT
[Unit]
Description=Open77 Android native: connect this host's adb to the Waydroid device
Documentation=file:$NATIVE/README.md
After=open77-waydroid-session.service network-online.target
Wants=open77-waydroid-session.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$NATIVE/adb-connect.sh

[Install]
WantedBy=multi-user.target
UNIT

# adb forgets a network device when its server is restarted (the emulator's
# stack starts one too), and the room then reports Android as down. So the
# connect runs on a timer: cheap when it is already connected, self-healing
# when it is not.
cat > /etc/systemd/system/open77-android-native-adb.timer <<UNIT
[Unit]
Description=Open77 Android native: keep adb connected to the Waydroid device
Documentation=file:$NATIVE/README.md

[Timer]
OnBootSec=20s
OnUnitActiveSec=30s
Unit=open77-android-native-adb.service

[Install]
WantedBy=timers.target
UNIT

# --- 5. its screen on the web ---------------------------------------------
# wayvnc sends the compositor's framebuffer as changed rectangles, websockify
# carries it over the same origin, noVNC draws it in the room's Chromium. Both
# are loopback: the only way in is the Docker bridge rule below.
cat > "$NATIVE/wayvnc.conf" <<'VNC'
# wayvnc serves the compositor's framebuffer; noVNC draws it in the room.
address=127.0.0.1
port=5900
# No password: the listener is loopback-only and the firewall admits only the
# Docker bridge, exactly like ws-scrcpy on the emulator's stack.
enable_auth=false
VNC
cat > /etc/systemd/system/open77-android-native-vnc.service <<UNIT
[Unit]
Description=Open77 Android native: wayvnc (the framebuffer Waydroid draws)
Documentation=file:$NATIVE/README.md
After=open77-waydroid-session.service
Wants=open77-waydroid-session.service

[Service]
Type=simple
Environment=XDG_RUNTIME_DIR=$RUN
Environment=WAYLAND_DISPLAY=display
ExecStartPre=/bin/sh -c 'for i in \$(seq 1 60); do [ -S $RUN/$WAYLAND ] && exit 0; sleep 1; done; exit 0'
# Loopback only, and no password: like ws-scrcpy on the emulator's stack, what
# can reach it can drive Android, so what can reach it is the firewall's job.
ExecStart=/usr/bin/wayvnc -C $NATIVE/wayvnc.conf
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT

# The page the room opens. Installed from this directory and re-installed on
# every run, so the file on the box and the file in the repository are the same
# one (see the header for why noVNC's own pages are not used).
install -d -m 0755 "$NOVNC"
install -m 0644 "$(dirname "$0")/screen.html" "$NOVNC/open77-screen.html"
echo "-- page: $NOVNC/open77-screen.html (no toolbar, so a tap is where it looks)"
# The half of Android's internet that belongs to this host (the firewall unit
# runs it): Docker's nft FORWARD chain drops waydroid0's traffic otherwise.
install -m 0755 "$(dirname "$0")/waydroid-internet.sh" "$NATIVE/waydroid-internet.sh"
echo "-- Android's own internet: $NATIVE/waydroid-internet.sh (nft forward accept)"

cat > /etc/systemd/system/open77-android-native-web.service <<UNIT
[Unit]
Description=Open77 Android native on the web (noVNC on port $PORT)
Documentation=file:$NATIVE/README.md
After=open77-android-native-vnc.service network-online.target
Wants=open77-android-native-vnc.service

[Service]
Type=simple
ExecStart=/usr/bin/websockify --web=$NOVNC 0.0.0.0:$PORT 127.0.0.1:5900
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT

# --- 6. only the Docker bridge may reach it --------------------------------
# Port $PORT is not on the host's INPUT allowlist (its policy is drop) and this
# adds exactly one exception, matching the emulator's on 8000. Inserted through
# iptables, not nft: the ip filter table here is iptables-nft's, and a rule
# inserted straight into it with nft is invisible to the next iptables call.
cat > /etc/systemd/system/open77-android-native-firewall.service <<UNIT
[Unit]
Description=Open77 Android native: allow the neko room to reach the screen on the Docker bridge
Documentation=file:$NATIVE/README.md
After=docker.service network-online.target
Requires=docker.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'iptables -C INPUT -i docker0 -p tcp --dport $PORT -j ACCEPT 2>/dev/null || iptables -I INPUT -i docker0 -p tcp --dport $PORT -j ACCEPT'
ExecStartPost=/bin/sh -c 'iptables -C INPUT -i waydroid0 -j ACCEPT 2>/dev/null || iptables -I INPUT -i waydroid0 -j ACCEPT'
# Android's OWN traffic: Docker's nft FORWARD chain ends with `policy drop`, which
# dropped every packet from waydroid0 to the uplink -- Android showed "connect to
# a network" with its lease, route and DNS all present (2026-10-23).
ExecStartPost=/bin/sh $NATIVE/waydroid-internet.sh
ExecStop=/bin/sh -c 'true'

[Install]
WantedBy=multi-user.target
UNIT

# The emulator's ws-scrcpy stack is not part of this screen; if an earlier
# version of this installer left its units behind, they are removed here so only
# one thing answers on $PORT.
for stale in open77-android-native-web-ws-scrcpy.service; do
  systemctl disable --now "$stale" >/dev/null 2>&1 || true
  rm -f "/etc/systemd/system/$stale"
done

systemctl daemon-reload
systemctl enable open77-android-native-firewall.service open77-android-native-web.service open77-android-native-adb.service
systemctl enable --now open77-android-native-adb.timer
systemctl enable --now open77-android-native-display.service open77-waydroid-session.service open77-android-native-vnc.service open77-android-native-adb.service
systemctl restart open77-android-native-firewall.service open77-android-native-web.service

# --- 7. let the host drive Android -----------------------------------------
# adb over the LXC bridge is authorised by key: without this the device answers
# "unauthorized" and every tool that drives Android fails. Written into
# Android's own /data/misc/adb/adb_keys, kept across reboots of Android.
echo "-- waiting for Android to boot"
for _ in $(seq 1 90); do
  if [ "$(XDG_RUNTIME_DIR=$RUN WAYLAND_DISPLAY=$WAYLAND waydroid status 2>/dev/null | awk -F'\t' '/^Session:/{print $2}')" = "RUNNING" ]; then break; fi
  sleep 2
done
XDG_RUNTIME_DIR=$RUN WAYLAND_DISPLAY=$WAYLAND waydroid status | sed 's/^/   /'
if [ -f /root/.android/adbkey.pub ]; then
  cat /root/.android/adbkey.pub | XDG_RUNTIME_DIR=$RUN WAYLAND_DISPLAY=$WAYLAND \
    waydroid shell -- sh -c 'mkdir -p /data/misc/adb; cat > /data/misc/adb/adb_keys; chmod 640 /data/misc/adb/adb_keys' \
    && echo "-- host adb key accepted inside Android"
else
  echo "!! no /root/.android/adbkey.pub yet: run any adb command once, then re-run this script"
fi

# --- 8. prove it, and say how to switch ------------------------------------
ok=0
for _ in $(seq 1 40); do
  if "$ADB_BIN" connect "$DEVICE:5555" >/dev/null 2>&1 && \
     "$ADB_BIN" -s "$DEVICE:5555" shell getprop sys.boot_completed 2>/dev/null | grep -q 1; then ok=1; break; fi
  sleep 3
done
[ "$ok" = 1 ] || { echo "!! Android did not report boot_completed"; exit 1; }
echo "-- Android $( "$ADB_BIN" -s "$DEVICE:5555" shell getprop ro.build.version.release | tr -d '\r' ) on $DEVICE:5555, $( "$ADB_BIN" -s "$DEVICE:5555" shell wm size | tr -d '\r' )"
echo "-- the screen: HTTP $(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$PORT/open77-screen.html) on 127.0.0.1:$PORT/open77-screen.html"
ss -ltn | grep -q ":$PORT" && echo "-- listening on 0.0.0.0:$PORT (reachable only from docker0)"

cat <<DONE

== installed, side by side with the emulator ==
 this screen:  http://172.17.0.1:$PORT/open77-screen.html
 the emulator: http://172.17.0.1:8000/ (untouched, still what the room shows)

 To make the rooms show this one instead, the room service takes it as
 environment, not as code (shared-browser/room/server.mjs):
   ANDROID_PORT=$PORT
   ANDROID_PAGE_PATH=/open77-screen.html
   ANDROID_UDID=$DEVICE:5555
   ANDROID_SERVICES="open77-android-native-display.service open77-waydroid-session.service open77-android-native-vnc.service open77-android-native-web.service open77-android-native-adb.service"
   ANDROID_LABEL=Android (native)
 and restart open77-room.

 To put the emulator back: drop those five, restart open77-room.
DONE
