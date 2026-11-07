#!/usr/bin/env bash
# Mouse and keyboard, through the room's Chromium, into the Android screen.
# Run on the game server as root:
#
#   ./e2e-android-input.sh [staging|production|f2077|fr-us]
#
# What it proves, and why it is not a unit test: the room's Chromium is showing
# Android, and the input goes in at the X server -- which is exactly where neko
# puts the mouse and keyboard a player uses at the television. So the path under
# test is the real one:
#
#   X (xdotool) -> Chromium -> ws-scrcpy -> adb -> the emulator        (old)
#   X (xdotool) -> Chromium -> noVNC -> wayvnc -> sway -> Waydroid     (native)
#
# The test does not care which: the device is the one the room service reports,
# and the assertions are that device's own -- Android's Settings search opens
# after the click, and the text typed on the keyboard is in its field, measured
# by uiautomator rather than by asking the browser what it thinks happened.
#
# Measured 2026-10-22, image 9, on the emulator: without captureKeyboard in the
# stream URL the field stayed empty however much was typed (the player leaves
# keyboard capture off by default); with it, "wifi" landed and the results
# filtered to Wi-Fi. The native screen has no such switch -- VNC carries the
# keyboard -- and this test passes there for the same reason, not by accident.
set -uo pipefail

ROLE="${1:-staging}"
case "$ROLE" in
  staging)    CONTAINER=open77-tvbrowser-xbs ;;
  production) CONTAINER=open77-tvbrowser-xb ;;
  f2077)      CONTAINER=open77-tvbrowser-f2077 ;;
  fr-us)      CONTAINER=open77-tvbrowser-fr-us ;;
  *) echo "usage: $(basename "$0") [staging|production|f2077|fr-us]"; exit 2 ;;
esac
ROOM=http://127.0.0.1:8002

CHECKSC=0; FAILS=0
check() {
  local label="$1"; shift
  CHECKSC=$((CHECKSC + 1))
  if "$@" >/dev/null 2>&1; then echo "  ok   $label"; else echo "  FAIL $label"; FAILS=$((FAILS + 1)); fi
}
x() { docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 $*"; }
room() { # one JSON field of the room's state, as text
  curl -s --max-time 10 "$ROOM/api/state?room=$ROLE" \
    | python3 -c "import json,sys; print(json.load(sys.stdin).get('$1') or '')"
}

DEVICE=$(room androidDevice)
LABEL=$(room androidLabel)
[ -n "$DEVICE" ] || { echo "the room reports no Android device: is Android running, and is the room pointed at it?"; exit 1; }
# Every adb call below names this device. Two can be attached at once (the
# emulator and the native one), and with two, a bare `adb shell` is an error,
# not a choice.
adbRoot() { command adb -s "$DEVICE" shell "$@" 2>/dev/null; }
ui() { adbRoot uiautomator dump /sdcard/ui.xml >/dev/null 2>&1; adbRoot cat /sdcard/ui.xml; }

# The Android screen fills the room's kiosk window, so a coordinate read from the
# device dump is a coordinate on the screen (the device's own size is scaled onto
# it when they differ).
DEVW=$(adbRoot wm size | tr -d '\r' | sed -n 's/.*Physical size: \([0-9]*\)x.*/\1/p')
DEVH=$(adbRoot wm size | tr -d '\r' | sed -n 's/.*Physical size: [0-9]*x\([0-9]*\).*/\1/p')
DEVW=${DEVW:-1280}; DEVH=${DEVH:-720}
SCREENW=$(docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 xdpyinfo | sed -n 's/^  dimensions: *\([0-9]*\)x.*/\1/p'" | tr -d '\r')
SCREENH=$(docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 xdpyinfo | sed -n 's/^  dimensions: *[0-9]*x\([0-9]*\) .*/\1/p'" | tr -d '\r')
SCREENW=${SCREENW:-1280}; SCREENH=${SCREENH:-720}
scale_x() { awk -v v="$1" -v s="$DEVW" -v t="$SCREENW" 'BEGIN { printf "%d", v * t / s }'; }
scale_y() { awk -v v="$1" -v s="$DEVH" -v t="$SCREENH" 'BEGIN { printf "%d", v * t / s }'; }

echo "== mouse and keyboard into $ROLE's Android ($LABEL, $DEVICE, $DEVW x $DEVH on $SCREENW x $SCREENH)"

check "the device reports boot_completed" test "$(adbRoot getprop sys.boot_completed | tr -d '\r')" = "1"
curl -s --max-time 180 -X POST "$ROOM/api/action" -H "Content-Type: application/json" \
  -d "{\"room\":\"$ROLE\",\"action\":\"open\",\"url\":\"android\"}" >/dev/null
sleep 12
check "the room has the screen open in its Chromium" \
  docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 xdotool search --name Chromium >/dev/null"
check "the window fills the screen (kiosk), so a device coordinate is a screen coordinate" \
  docker exec "$CONTAINER" sh -c "DISPLAY=:99.0 xdotool search --name Chromium | while read w; do DISPLAY=:99.0 xdotool getwindowgeometry \$w; done | grep -q '${SCREENW}x${SCREENH}'"

# Settings resumes where it was left (a previous run's search page has no bar to
# click); starting it cold is the state this test is written against. The bar's
# id differs between images: the phone layout uses search_action_bar, and the
# two-pane (wide) layout the native Android shows uses search_action_bar_two_pane.
adbRoot input keyevent KEYCODE_HOME >/dev/null 2>&1
BOUNDS=""
for attempt in 1 2 3 4 5 6; do
  adbRoot am force-stop com.android.settings >/dev/null 2>&1
  adbRoot am start -a android.settings.SETTINGS >/dev/null 2>&1
  sleep 4
  BOUNDS=$(ui | tr '>' '\n' \
    | grep -E 'resource-id="com.android.settings:id/(search_action_bar|search_action_bar_two_pane|search_bar|search_open)"' \
    | sed -n 's/.*bounds="\[\([0-9]*\),\([0-9]*\)\]\[\([0-9]*\),\([0-9]*\)\]".*/\1 \2 \3 \4/p' | head -1)
  [ -n "$BOUNDS" ] && break
done
if [ -z "$BOUNDS" ]; then echo "  FAIL Settings' search bar is not on the screen"; FAILS=$((FAILS + 1)); else
  set -- $BOUNDS
  CX=$(scale_x $(( ($1 + $3) / 2 ))); CY=$(scale_y $(( ($2 + $4) / 2 )))
  # Away first, then onto the target. X sends no motion event when the pointer
  # already sits where it is being put, and Chromium then hands the page a press
  # with no position, so noVNC sends the top-left one: on Settings' home that is
  # a tap on the status bar and the notification shade opens. Measured
  # 2026-10-23 on both noVNC's vnc.html and screen.html, so the move is the test
  # being explicit about the pointer, not a workaround for one page.
  #
  # The click and the typing are each asked for twice, the second time only when
  # the first did not land. Measured on production 2026-10-23: the first press
  # after that room's Chromium had sat on the splash for three hours was dropped,
  # and the same gesture a second later opened Settings -- so one retry is the
  # difference between a flake and a red suite. The retry is printed, never
  # silent, so a run that needed it still says so.
  click_screen() {
    x "xdotool mousemove $((SCREENW - 8)) $((SCREENH - 8)) sleep 0.3 mousemove $CX $CY sleep 0.3 click 1"
  }
  # A press that reaches the remote screen before the page has a pointer
  # position is sent as a tap at the top-left, which opens the notification
  # shade -- and the shade then covers whatever the next click was aimed at,
  # so repeating the gesture without undoing it cannot work. Measured on
  # staging 2026-10-23: the click became a top-left tap, the retry hit the
  # shade, and the typing went to the shade too. So the retry first puts the
  # screen back the way the check expects: collapse the shade, and make sure
  # Settings is the window in front again.
  reset_screen() {
    adbRoot cmd statusbar collapse >/dev/null 2>&1
    case "$(adbRoot dumpsys window 2>/dev/null | grep -i mCurrentFocus | head -1)" in
      *Settings*) ;;
      *) adbRoot am start -a android.settings.SETTINGS >/dev/null 2>&1; sleep 3 ;;
    esac
  }
  # Two images, two ways of saying "the search activity came up": AOSP 14 (the
  # emulator) reports it through the input method, LineageOS 20 (the native one)
  # focuses com.android.settings.intelligence …SearchActivity. Either is the
  # click having arrived; a miss leaves Settings' homepage focused.
  search_open() {
    sh -c "adb -s '$DEVICE' shell dumpsys window 2>/dev/null | grep -qi SearchActivity || adb -s '$DEVICE' shell dumpsys input_method 2>/dev/null | grep -q open_search_view_edit_text"
  }
  type_wifi() { x "xdotool type --delay 150 wifi"; }
  wifi_typed() {
    sh -c "adb -s '$DEVICE' shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1; adb -s '$DEVICE' shell cat /sdcard/ui.xml | tr '>' '\n' | grep -q 'text=\"wifi\"' && adb -s '$DEVICE' shell cat /sdcard/ui.xml | tr '>' '\n' | grep -q 'text=\"Wi-Fi\"'"
  }

  click_screen
  sleep 3
  search_open || {
    echo "  .. the first click did not reach Settings; putting the screen back and asking again"
    reset_screen
    click_screen
    sleep 3
  }
  check "a click on the screen opens Settings' search (the mouse reached Android)" search_open

  type_wifi
  sleep 3
  wifi_typed || {
    # Whatever the first attempt left in the field is cleared before the retry,
    # so a partial "wif" cannot make the second attempt read "wifwifi" and fail
    # a check the room actually got right.
    echo "  .. the first keystrokes did not land; clearing the field and typing again"
    for _ in 1 2 3 4 5 6 7 8 9 10; do adbRoot input keyevent KEYCODE_DEL >/dev/null 2>&1; done
    type_wifi
    sleep 3
  }
  check "the text typed at the keyboard is in Android's field, and it filtered the results" wifi_typed
fi

adbRoot input keyevent KEYCODE_BACK >/dev/null 2>&1
echo
echo "$CHECKSC checks, $FAILS failed"
[ "$FAILS" -eq 0 ]
