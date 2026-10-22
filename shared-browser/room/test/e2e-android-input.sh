#!/usr/bin/env bash
# Mouse and keyboard, through the room's Chromium, into the Android emulator.
# Run on the game server as root:
#
#   ./e2e-android-input.sh [staging|production|f2077]
#
# What it proves, and why it is not a unit test: the room's Chromium is showing
# ws-scrcpy's player for emulator-5554, and the input goes in at the X server --
# which is exactly where neko puts the mouse and keyboard a player uses at the
# television. So the path under test is the real one:
#
#   X (xdotool)  ->  Chromium  ->  ws-scrcpy  ->  adb  ->  the emulator
#
# A click that misses is a click that was not delivered, so the assertions are
# the emulator's own: Android's Settings search opens after the click, and the
# text typed on the keyboard is in its field -- measured by uiautomator, not by
# asking the browser what it thinks happened.
#
# Measured 2026-10-22, image 9: without captureKeyboard in the stream URL the
# field stayed empty however much was typed (the player leaves keyboard capture
# off by default); with it, "wifi" landed and the results filtered to Wi-Fi.
set -uo pipefail

ROLE="${1:-staging}"
case "$ROLE" in
  staging)    CONTAINER=open77-tvbrowser-xbs ;;
  production) CONTAINER=open77-tvbrowser-xb ;;
  f2077)      CONTAINER=open77-tvbrowser-f2077 ;;
  *) echo "usage: $(basename "$0") [staging|production|f2077]"; exit 2 ;;
esac

CHECKSC=0; FAILS=0
check() {
  local label="$1"; shift
  CHECKSC=$((CHECKSC + 1))
  if "$@" >/dev/null 2>&1; then echo "  ok   $label"; else echo "  FAIL $label"; FAILS=$((FAILS + 1)); fi
}
x() { docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 $*"; }
ui() { adb shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1; adb shell cat /sdcard/ui.xml; }
# The Android screen fills the room's kiosk window; if the emulator is not
# 1280x720 the device's own size is scaled onto it, so a coordinate read from
# the device dump is a coordinate on the screen.
DEVW=$(adb shell wm size | tr -d '\r' | sed -n 's/.*Physical size: \([0-9]*\)x.*/\1/p')
DEVH=$(adb shell wm size | tr -d '\r' | sed -n 's/.*Physical size: [0-9]*x\([0-9]*\).*/\1/p')
DEVW=${DEVW:-1280}; DEVH=${DEVH:-720}
SCREENW=$(docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 xdpyinfo | sed -n 's/^  dimensions: *\([0-9]*\)x.*/\1/p'" | tr -d '\r')
SCREENH=$(docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 xdpyinfo | sed -n 's/^  dimensions: *[0-9]*x\([0-9]*\) .*/\1/p'" | tr -d '\r')
SCREENW=${SCREENW:-1280}; SCREENH=${SCREENH:-720}
scale_x() { awk -v v="$1" -v s="$DEVW" -v t="$SCREENW" 'BEGIN { printf "%d", v * t / s }'; }
scale_y() { awk -v v="$1" -v s="$DEVH" -v t="$SCREENH" 'BEGIN { printf "%d", v * t / s }'; }

echo "== mouse and keyboard into $ROLE's emulator ($DEVW x $DEVH on $SCREENW x $SCREENH)"

check "the emulator is booted" adb shell "test -n \"\$(getprop sys.boot_completed)\""
curl -s --max-time 180 -X POST http://127.0.0.1:8002/api/action -H "Content-Type: application/json" \
  -d "{\"room\":\"$ROLE\",\"action\":\"open\",\"url\":\"android\"}" >/dev/null
sleep 12
check "the room has the player open on the screen" \
  docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 xdotool search --name Chromium >/dev/null"
check "the window fills the screen (kiosk), so a device coordinate is a screen coordinate" \
  docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 xdotool search --name Chromium | while read w; do DISPLAY=:99.0 xdotool getwindowgeometry \$w; done | grep -q '${SCREENW}x${SCREENH}'"

# Settings resumes where it was left (a previous run's search page has no bar to
# click); starting it cold is the state this test is written against.
adb shell input keyevent KEYCODE_HOME >/dev/null 2>&1
# A cold start is quick, but not always: a dump taken as Settings comes up finds
# the launcher. Ask again for a few seconds before believing it is not there.
BOUNDS=""
for attempt in 1 2 3 4 5 6; do
  adb shell am force-stop com.android.settings >/dev/null 2>&1
  adb shell am start -a android.settings.SETTINGS >/dev/null 2>&1
  sleep 4
  BOUNDS=$(ui | tr '>' '\n' | grep 'resource-id="com.android.settings:id/search_action_bar"' | sed -n 's/.*bounds="\[\([0-9]*\),\([0-9]*\)\]\[\([0-9]*\),\([0-9]*\)\]".*/\1 \2 \3 \4/p' | head -1)
  [ -n "$BOUNDS" ] && break
done
if [ -z "$BOUNDS" ]; then echo "  FAIL Settings' search bar is not on the screen"; FAILS=$((FAILS + 1)); else
  set -- $BOUNDS
  CX=$(scale_x $(( ($1 + $3) / 2 ))); CY=$(scale_y $(( ($2 + $4) / 2 )))
  x "xdotool mousemove $CX $CY sleep 0.2 click 1"
  sleep 3
  check "a click on the screen opens Settings' search (the mouse reached the emulator)" \
    sh -c "adb shell dumpsys input_method | grep -q open_search_view_edit_text"
  x "xdotool type --delay 150 wifi"
  sleep 3
  check "the text typed at the keyboard is in the emulator's field, and it filtered the results" \
    sh -c "adb shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1; adb shell cat /sdcard/ui.xml | tr '>' '\n' | grep -q 'text=\"wifi\"' && adb shell cat /sdcard/ui.xml | tr '>' '\n' | grep -q 'text=\"Wi-Fi\"'"
fi

adb shell input keyevent KEYCODE_BACK >/dev/null 2>&1
echo
echo "$CHECKSC checks, $FAILS failed"
[ "$FAILS" -eq 0 ]
