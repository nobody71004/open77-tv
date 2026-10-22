#!/usr/bin/env bash
# Install the room service (splash, profile library, Android control) on the game
# server. Run as root from the directory holding this file.
#
#   ./install.sh
#
# Its only dependency is ws (the room's DevTools socket); this installs it if
# /opt/open77-room/node_modules is missing.
set -euo pipefail

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

D=/opt/open77-room
SRC="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$D/web"
# Install only what is not already the same file: this script is also run from
# the install directory after a copy, and `install` refuses src == dst.
for f in server.mjs; do [ "$SRC/$f" -ef "$D/$f" ] || install -m 0644 "$SRC/$f" "$D/$f"; done
for f in splash.html profiles.html; do [ "$SRC/web/$f" -ef "$D/web/$f" ] || install -m 0644 "$SRC/web/$f" "$D/web/$f"; done
# The end-to-end tests ship with the service so the thing that is running can be
# checked where it runs (test/e2e-room.py, test/e2e-android-input.sh, and the
# profile persistence test that recreates a container).
mkdir -p "$D/test"
for f in e2e-room.py e2e-android-input.sh e2e-game-request.sh e2e-profile-persistence.sh; do
  [ "$SRC/test/$f" -ef "$D/test/$f" ] || install -m 0755 "$SRC/test/$f" "$D/test/$f"
done
install -m 0644 "$SRC/open77-room.service" /etc/systemd/system/open77-room.service

if [ ! -d "$D/node_modules/ws" ]; then
  ( cd "$D" && [ -f package.json ] || npm init -y >/dev/null 2>&1
    npm install --no-audit --no-fund ws >/dev/null 2>&1 )
fi

systemctl daemon-reload
systemctl enable open77-room.service
systemctl restart open77-room.service
sleep 2
systemctl is-active open77-room.service
curl -s -o /dev/null -w "loopback /api/status http=%{http_code}\n" http://127.0.0.1:8002/api/status
echo "installed; 'systemctl status open77-room' and http://172.17.0.1:8002/ from a room"
