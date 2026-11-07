#!/usr/bin/env bash
# The image end to end on THIS machine, without touching a room: builds build/
# as open77/tvbrowser-chromium:local-test, runs it as a throwaway container (its
# own passwords, loopback ports, a temporary profile library, the policy for a
# room called "local", no Widevine mount), and checks what the image is for:
#
#   1. up.sh's image checks: the five scripts in the client page (the guard
#      first), the policies (Netflix left unfiltered), the quiet window
#      manager, the modes, the profile library, kiosk, the DevTools forwarder
#   2. the window manager: one desktop, and Super+D, Ctrl+Alt+Right,
#      Shift+Alt+Right and Alt+Space change nothing (openbox reads its rc.xml
#      without a parser error)
#   3. the picture: neko's own pipeline lines say show-pointer=0, never true
#   4. a real Chromium as the player (image-local.mjs): the picture over WebRTC,
#      the guard first in the page, "hello ok" typed into the room's own page,
#      and F5, Ctrl+T/W/N, Alt+F4, F11, Ctrl+Tab and Ctrl+1 kept from it (the
#      page never reloads, no tab opens or closes)
#
# Needs docker, and node with playwright (npm i playwright; npx playwright
# install chromium). Removes the container and its files at the end. Prints no
# secret. Exits with the number of failed checks.
#
#   USAGE  test/image-local.sh          BUILD_ARGS="--build-arg http_proxy=..." for a build behind a proxy
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRV="$(cd "$HERE/.." && pwd)"
IMAGE=open77/tvbrowser-chromium:local-test
NAME=open77-tvbrowser-local-test
HTTP=18099; RTC=59199; PREFIX=/tv-local
T=$(mktemp -d); trap 'docker rm -f "$NAME" >/dev/null 2>&1; rm -rf "$T"' EXIT
fail=0
ok()  { echo "  ok    $1"; }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

echo "== build ($SRV/build)"
# shellcheck disable=SC2086
docker build -q ${BUILD_ARGS:-} -t "$IMAGE" "$SRV/build" >/dev/null || { echo "  FAIL  the build (its own checks are in the Dockerfile)"; exit 1; }
ok "built: $(docker image inspect "$IMAGE" --format '{{.Id}}' | cut -c1-19) (the Dockerfile's own checks passed)"

echo "== 1. what up.sh checks in the image"
docker run --rm --entrypoint sh "$IMAGE" -c \
  'grep -q "src=\"open77-input.js?v=3\"></script><script src=\"open77-ice.js\"></script><script src=\"open77-paste.js\"></script><script src=\"open77-volume.js\"></script><script src=\"open77-fit.js?v=3\"" /var/www/index.html && test -s /var/www/open77-input.js && test -s /var/www/open77-ice.js && test -s /var/www/open77-paste.js && test -s /var/www/open77-volume.js && test -s /var/www/open77-fit.js && grep -q "\"NewTabPageLocation\"" /etc/chromium/policies/managed/policies.json && grep -q "\"noFiltering\"" /etc/chromium/policies/managed/open77-netflix.json && grep -q "open77 tv kit" /etc/xdg/openbox/rc.xml && ! grep -q "<keybind " /etc/xdg/openbox/rc.xml && ! find / -xdev -iname "*widevine*" 2>/dev/null | grep -q .' \
  && ok "the client page loads the guard first, then the image's four; policies, the quiet rc.xml; no Widevine" \
  || bad "scripts, policies or the quiet window manager missing, or a DRM module baked in"
docker run --rm --entrypoint sh "$IMAGE" -c 'for f in /etc/chromium/policies/managed/policies.json /etc/chromium/policies/managed/open77-netflix.json /etc/xdg/openbox/rc.xml /etc/neko/supervisord/chromium.conf /var/www/open77-input.js /var/www/open77-ice.js /var/www/open77-paste.js /var/www/open77-volume.js /var/www/open77-fit.js; do [ "$(stat -c %a "$f")" = 644 ] || exit 1; done' \
  && ok "every policy, script and config readable (644)" || bad "a policy, script or config is not 644"
docker run --rm --entrypoint sh "$IMAGE" -c 'grep -q -- "--user-data-dir=/profiles/active" /etc/neko/supervisord/chromium.conf && grep -q -- "--kiosk" /etc/neko/supervisord/chromium.conf && grep -q "program:cdp" /etc/neko/supervisord/chromium.conf && command -v socat >/dev/null' \
  && ok "profile library, kiosk, DevTools forwarder" || bad "no profile library, kiosk or forwarder"

echo "== the throwaway room"
PASS=$(openssl rand -hex 12)
mkdir -p "$T/profiles/house"; ln -sfn house "$T/profiles/active"; chown -R 1000:1000 "$T/profiles"; chown -h 1000:1000 "$T/profiles/active"
sed "s/__ROOM__/local/g" "$SRV/build/policies.json" > "$T/policies.json"; chmod 644 "$T/policies.json"
# up.sh's own picture: its PIPELINE line, at its default 60 fps
PIPELINE=$(FPS=60; BITRATE=4500000; eval "$(grep '^PIPELINE=' "$SRV/up.sh")"; printf '%s' "$PIPELINE")
case "$PIPELINE" in *"show-pointer=0"*) ;; *) bad "up.sh's pipeline does not say show-pointer=0";; esac
{
  echo "NEKO_SERVER_PROXY=true"; echo "NEKO_SERVER_PATH_PREFIX=$PREFIX"; echo "NEKO_DESKTOP_SCREEN=1280x720@60"
  echo "NEKO_CAPTURE_VIDEO_PIPELINE=$PIPELINE"; echo "NEKO_MEMBER_PROVIDER=multiuser"
  echo "NEKO_MEMBER_MULTIUSER_USER_PASSWORD=$PASS"; echo "NEKO_MEMBER_MULTIUSER_ADMIN_PASSWORD=$(openssl rand -hex 24)"
  echo "NEKO_SESSION_API_TOKEN=$(openssl rand -hex 32)"; echo "NEKO_SESSION_IMPLICIT_HOSTING=true"
  echo "NEKO_WEBRTC_UDPMUX=$RTC"; echo "NEKO_WEBRTC_TCPMUX=$RTC"; echo "NEKO_WEBRTC_NAT1TO1=127.0.0.1"
} > "$T/neko.env"; chmod 600 "$T/neko.env"
docker rm -f "$NAME" >/dev/null 2>&1
docker run -d --name "$NAME" --shm-size 2g --memory 4g \
  -p 127.0.0.1:$HTTP:8080 -p 127.0.0.1:$RTC:$RTC/udp -p 127.0.0.1:$RTC:$RTC/tcp \
  -v "$T/profiles:/profiles" -v "$T/policies.json:/etc/chromium/policies/managed/policies.json:ro" \
  --env-file "$T/neko.env" "$IMAGE" >/dev/null || { bad "the container did not start"; exit "$fail"; }
code=000
for _ in $(seq 1 45); do
  sleep 2
  code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP$PREFIX/" || true)
  [ "$code" = 200 ] && break
done
[ "$code" = 200 ] && ok "client page: HTTP 200" || { bad "client page: HTTP $code"; docker logs --tail 20 "$NAME" 2>&1 | grep -v -i -E "password|token|pwd" | cut -c1-160; exit "$fail"; }
for _ in $(seq 1 30); do docker exec -u neko "$NAME" sh -c 'DISPLAY=:99.0 xdotool search --onlyvisible --class chromium' >/dev/null 2>&1 && break; sleep 1; done

echo "== 2. the window manager"
X='export DISPLAY=:99.0; p() { xprop -root "$1" | grep -o "[0-9]*$"; }'
st=$(docker exec -u neko "$NAME" sh -c "$X"'; echo "$(p _NET_NUMBER_OF_DESKTOPS) $(p _NET_CURRENT_DESKTOP) $(p _NET_SHOWING_DESKTOP)"')
[ "$st" = "1 0 0" ] && ok "one desktop, on it, the browser shown" || bad "desktops/current/showing: $st (want 1 0 0)"
after=$(docker exec -u neko "$NAME" sh -c "$X"'; for k in super+d ctrl+alt+Right shift+alt+Right alt+space; do xdotool key $k; sleep 0.5; done; echo "$(p _NET_NUMBER_OF_DESKTOPS) $(p _NET_CURRENT_DESKTOP) $(p _NET_SHOWING_DESKTOP) $(xdotool search --onlyvisible --class chromium | wc -l)"')
[ "$after" = "1 0 0 1" ] && ok "Super+D, Ctrl+Alt+Right, Shift+Alt+Right, Alt+Space: nothing changed, the browser still there" \
  || bad "after the window manager's keys: $after (want 1 0 0 1)"
docker exec "$NAME" sh -c '! grep -qi "parser error" /var/log/neko/openbox.log' && ok "openbox read its rc.xml without a parser error" \
  || bad "openbox logged a parser error"

echo "== 3. the picture's cursor"
lines=$(docker logs "$NAME" 2>&1 | sed -E 's/\x1b\[[0-9;]*m//g' | grep -o -E "show-pointer=[a-zA-Z0-9]+" | sort | uniq -c | tr -s ' ' | tr '\n' ';')
case "$lines" in
  *show-pointer=0*) case "$lines" in *show-pointer=true*|*show-pointer=1\;*) bad "a pipeline draws the pointer: $lines";; *) ok "neko's pipelines: $lines";; esac ;;
  *) bad "no show-pointer=0 in neko's log: $lines" ;;
esac

echo "== 4. a player's Chromium on the client page (image-local.mjs)"
CIP=$(docker inspect "$NAME" --format '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
GW=$(docker inspect "$NAME" --format '{{range .NetworkSettings.Networks}}{{.Gateway}}{{end}}')
O77_CLIENT="http://127.0.0.1:$HTTP$PREFIX/?usr=open77&pwd=$PASS&embed=1" node "$HERE/image-local.mjs" "$CIP" "$GW"
fail=$((fail + $?))

echo "== $([ "$fail" = 0 ] && echo "all checks passed" || echo "$fail check(s) failed")"
exit "$fail"
