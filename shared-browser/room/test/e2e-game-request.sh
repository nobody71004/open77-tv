#!/usr/bin/env bash
# The path a host's "Android screen" press takes, end to end, without a player at
# a keyboard: the game records the request, the room polls for it, and the room's
# screen changes. Run on the game server as root:
#
#   ./e2e-game-request.sh [staging|production]
#
# `POST /v1/android/selftest` on the game server's own lockback listener is the
# same code path as the panel's button (it bumps the same request id, which is
# all the room reads). loopback is also the only place the game accepts it from.
#
# f2077 has no game server attached to its room (the room service's GAME map has
# nothing for it), so there is nothing to ask: its Android comes from the splash.
set -uo pipefail

ROLE="${1:-staging}"
case "$ROLE" in
  staging)    GAMEPORT=11843; CONTAINER=open77-tvbrowser-xbs ;;
  production) GAMEPORT=11799; CONTAINER=open77-tvbrowser-xb ;;
  *) echo "usage: $(basename "$0") [staging|production]"; exit 2 ;;
esac
GAME="http://127.0.0.1:$GAMEPORT/opx_watchparty"
ROOM=http://127.0.0.1:8002

CHECKSC=0; FAILS=0
check() {
  local label="$1"; shift
  CHECKSC=$((CHECKSC + 1))
  if "$@" >/dev/null 2>&1; then echo "  ok   $label"; else echo "  FAIL $label"; FAILS=$((FAILS + 1)); fi
}
room_url() {
  local ip; ip=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$CONTAINER")
  curl -s --max-time 5 "http://$ip:9222/json/list" \
    | python3 -c 'import sys,json; d=json.load(sys.stdin); p=[t for t in d if t["type"]=="page"]; print(p[-1]["url"] if p else "")'
}
wait_for() { # fragment, seconds
  local i
  for i in $(seq 1 "$2"); do
    case "$(room_url)" in *"$1"*) return 0 ;; esac
    sleep 1
  done
  return 1
}

echo "== a game-asked Android request reaching $ROLE's room"

check "the game server answers on its own listener" \
  sh -c "curl -sf --max-time 5 '$GAME/v1/health' >/dev/null"
BEFORE=$(curl -s --max-time 5 "$GAME/v1/pending" | python3 -c 'import sys,json;print(json.load(sys.stdin)["id"])')
# A room that has just been installed or restarted adopts the request it finds on
# its first poll instead of replaying it -- that is what keeps an old request off
# a screen that comes back. So let it take two polls before asking for a new one.
sleep 5
ASK=$(curl -s --max-time 5 -X POST "$GAME/v1/android/selftest")
AFTER=$(curl -s --max-time 5 "$GAME/v1/pending" | python3 -c 'import sys,json;print(json.load(sys.stdin)["id"])')
check "the request is taken, from this machine only, and the id goes up" \
  sh -c "[ $AFTER -gt $BEFORE ] && echo '$ASK' | grep -q 'self-test'"
check "the room notices it within its two-second poll and puts the emulator on the screen" \
  wait_for "action=stream" 30
URL=$(room_url)
check "and the stream it opened captures the keyboard, not just the mouse" \
  test "${URL#*captureKeyboard=true}" != "$URL"

curl -s --max-time 30 -X POST "$ROOM/api/action" -H 'Content-Type: application/json' \
  -d "{\"room\":\"$ROLE\",\"action\":\"open\",\"url\":\"http://172.17.0.1:8002/?room=$ROLE\"}" >/dev/null
check "the room goes back to its splash afterwards" wait_for "8002/?room=$ROLE" 20

echo
echo "$CHECKSC checks, $FAILS failed"
[ "$FAILS" -eq 0 ]
