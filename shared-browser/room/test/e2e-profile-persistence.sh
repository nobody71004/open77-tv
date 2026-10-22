#!/usr/bin/env bash
# A profile is a directory on the host, and the browser is a container: the
# sign-ins only really persist if the directory is still there -- and the browser
# comes back on it -- after the container has been thrown away and made again.
# Run on the game server as root:
#
#   ./e2e-profile-persistence.sh [staging|production|f2077]
#
# The recreation is ../server/up.sh <role>, which is also how a room is upgraded:
# stop, rm, run from the same image with the same mounts. It is deliberately the
# real operation and not a simulation, because a bind mount that is missing, or a
# --user-data-dir pointing at something else, is exactly the bug this exists to
# catch. The marker is written from INSIDE the container, at the path the browser
# uses, so what is checked afterwards is the browser's own view of its profile.
#
# Nothing signs in on the room's behalf: a marker file stands in for the cookies,
# and the profile's own cookie count (read through the room service) is compared
# before and after, so a profile that came back empty is caught.
set -uo pipefail

ROLE="${1:-staging}"
case "$ROLE" in
  staging)
    CONTAINER=open77-tvbrowser-xbs; DIR=/opt/open77-tvbrowser ;;
  production)
    CONTAINER=open77-tvbrowser-xb;  DIR=/opt/open77-tvbrowser-xb ;;
  f2077)
    CONTAINER=open77-tvbrowser-f2077; DIR=/opt/open77-tvbrowser-f2077 ;;
  *) echo "usage: $(basename "$0") [staging|production|f2077]"; exit 2 ;;
esac
ROOM=http://127.0.0.1:8002
# up.sh lives in the room's install directory (it is what created the container),
# so the test recreates the container exactly as an upgrade would.
UP="${UP:-$DIR/up.sh}"
MARKER=open77-persistence-test

CHECKSC=0; FAILS=0
check() {
  local label="$1"; shift
  CHECKSC=$((CHECKSC + 1))
  if "$@" >/dev/null 2>&1; then echo "  ok   $label"; else echo "  FAIL $label"; FAILS=$((FAILS + 1)); fi
}
mount_source() {
  docker inspect -f "{{range .Mounts}}{{if eq .Destination \"/profiles\"}}{{.Source}}{{end}}{{end}}" "$CONTAINER"
}
active_profile() { readlink "$DIR/profiles/active" | xargs -r basename; }
room_field() { # python expression over /api/state
  curl -s --max-time 8 "$ROOM/api/state?room=$ROLE" | python3 -c "import sys,json;d=json.load(sys.stdin);print($1)"
}

echo "== profile persistence across a container recreate ($ROLE)"

check "the room's installer is where the recreate expects it ($UP)" test -x "$UP"
check "the room's container is up" \
  sh -c "docker inspect -f '{{.State.Running}}' $CONTAINER | grep -qx true"
check "the profile library is a host directory mounted at /profiles" \
  test "$(mount_source)" = "$DIR/profiles"
ACTIVE_BEFORE=$(active_profile)
COOKIES_BEFORE=$(room_field "sum(p.get('cookies',0) for p in d['profiles'])")
check "the active profile is a real directory in the library" \
  test -d "$DIR/profiles/$ACTIVE_BEFORE"

# Written the way the browser would write it: inside the container, on the path
# Chromium calls its own profile directory.
docker exec "$CONTAINER" sh -c "printf 'written before the recreate\n' > /profiles/active/Default/$MARKER" >/dev/null 2>&1
check "a file written into the active profile inside the container is on the host" \
  test -s "$DIR/profiles/$ACTIVE_BEFORE/Default/$MARKER"

echo "  .. recreating $CONTAINER (up.sh $ROLE: stop, rm, run from the same image and mounts)"
if "$UP" "$ROLE" >/tmp/open77-persist-up.log 2>&1; then
  check "the container comes back" \
    sh -c "for i in \$(seq 1 40); do docker inspect -f '{{.State.Running}}' $CONTAINER 2>/dev/null | grep -qx true && exit 0; sleep 3; done; exit 1"
else
  echo "  FAIL up.sh $ROLE"; FAILS=$((FAILS + 1)); CHECKSC=$((CHECKSC + 1))
  tail -12 /tmp/open77-persist-up.log | sed 's/^/       /'
fi

check "it is still the same active profile" \
  test "$(active_profile)" = "$ACTIVE_BEFORE"
check "the marker survived the recreate" \
  test -s "$DIR/profiles/$ACTIVE_BEFORE/Default/$MARKER"
check "the sign-in database survived the recreate" \
  test -s "$DIR/profiles/$ACTIVE_BEFORE/Default/Cookies"
check "no profile was lost: the library lists the same one" \
  sh -c "curl -s --max-time 8 '$ROOM/api/state?room=$ROLE' | python3 -c \"import sys,json;d=json.load(sys.stdin);sys.exit(0 if d['profile']=='$ACTIVE_BEFORE' and any(p['name']=='$ACTIVE_BEFORE' for p in d['profiles']) else 1)\""
COOKIES_AFTER=$(room_field "sum(p.get('cookies',0) for p in d['profiles'])")
check "and the same number of cookies as before ($COOKIES_BEFORE)" \
  test "$COOKIES_AFTER" = "$COOKIES_BEFORE"

# The browser itself, on the same profile: Chromium up, and the screen showing
# this room's own page (the start page), not a blank kiosk.
check "Chromium comes back on the profile" \
  sh -c "for i in \$(seq 1 20); do docker exec $CONTAINER supervisorctl status chromium 2>/dev/null | grep -q RUNNING && exit 0; sleep 3; done; exit 1"
check "the television's page comes back on the room's screen" \
  sh -c "for i in \$(seq 1 20); do IP=\$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' $CONTAINER); curl -s --max-time 4 \"http://\$IP:9222/json/list\" | grep -q \"8002/?room=$ROLE\" && exit 0; sleep 3; done; exit 1"
check "the room service answers for it again" \
  sh -c "curl -sf --max-time 8 '$ROOM/api/state?room=$ROLE' | grep -q '\"running\": *true'"

docker exec "$CONTAINER" sh -c "rm -f /profiles/active/Default/$MARKER" >/dev/null 2>&1

echo
echo "$CHECKSC checks, $FAILS failed"
[ "$FAILS" -eq 0 ]
