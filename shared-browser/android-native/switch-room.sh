#!/usr/bin/env bash
# Which Android the rooms show.
#
# The room service (open77-room, shared-browser/room/server.mjs) takes the
# Android screen as environment: its port, the page to open, the adb device it
# watches, and the units that "put Android up" starts. This installs that
# environment as a systemd drop-in for one stack or the other, so switching --
# and switching back -- is one command, not a patch.
#
#   bash switch-room.sh native     Waydroid on the host's kernel, noVNC on :8001
#   bash switch-room.sh emulator    the qemu emulator, ws-scrcpy on :8000
#   bash switch-room.sh status      what the rooms are showing now
#
# Only open77-room is restarted. The two Android stacks themselves keep running;
# what changes is which one the televisions are pointed at.
set -euo pipefail

DROPIN=/etc/systemd/system/open77-room.service.d/10-android-native.conf
NATIVE_SERVICES="open77-android-native-display.service open77-waydroid-session.service open77-android-native-vnc.service open77-android-native-web.service open77-android-native-adb.service"

state() {
  curl -s --max-time 5 "http://127.0.0.1:8002/api/state?room=production" | python3 -c '
import json, sys
d = json.load(sys.stdin)
print("  label:  ", d.get("androidLabel"))
print("  port:   ", d.get("androidPort"))
print("  device: ", d.get("androidDevice") or "none on adb")
print("  screen: ", next((c["url"] for c in d.get("channels", []) if c["id"] == "android"), "?"))
'
}

case "${1:-status}" in
  native)
    install -d -m 0755 "$(dirname "$DROPIN")"
    cat > "$DROPIN" <<CONF
# The rooms show the native Android (Waydroid): see
# /opt/open77-android-native/README.md. Written by switch-room.sh.
[Service]
Environment=ANDROID_PORT=8001
Environment=ANDROID_PAGE_PATH=/open77-screen.html
Environment=ANDROID_UDID=192.168.240.112:5555
# Quoted: systemd splits an unquoted value on spaces and keeps only the first
# word, logging "Invalid environment assignment" for the rest (measured
# 2026-10-23: ANDROID_SERVICES came through as the display unit alone).
Environment="ANDROID_SERVICES=$NATIVE_SERVICES"
Environment="ANDROID_LABEL=Android (native)"
CONF
    systemctl daemon-reload
    systemctl restart open77-room
    sleep 3
    echo "rooms now show the native Android:"
    state
    ;;
  emulator)
    rm -f "$DROPIN"
    rmdir "$(dirname "$DROPIN")" 2>/dev/null || true
    systemctl daemon-reload
    systemctl restart open77-room
    sleep 3
    echo "rooms now show the emulator (server.mjs defaults):"
    state
    ;;
  status)
    systemctl is-active open77-room >/dev/null || { echo "open77-room is not running"; exit 1; }
    state
    ;;
  *)
    echo "usage: $(basename "$0") [native|emulator|status]" >&2
    exit 2
    ;;
esac
