#!/usr/bin/env bash
# The shared browser for Open77 televisions: neko's Chromium with the host's own
# Widevine, streamed over WebRTC (build/, run as image 7). One instance per server
# ROLE -- the same image and the same build; they differ only in where they live,
# what they are called, and which three numbers they take:
#
#   role         install dir                     container                   prefix                   http   webrtc
#   staging      /opt/open77-tvbrowser           open77-tvbrowser-xbs        /tv-browser-xb-staging   18080  59100
#   production   /opt/open77-tvbrowser-xb        open77-tvbrowser-xb         /tv-browser-xb           18081  59101
#   f2077        /opt/open77-tvbrowser-f2077     open77-tvbrowser-f2077      /tv-browser-f2077        18082  59102
#
# f2077 is its own room, not a copy of production's: its own neko.env, its own
# viewer password, its own browser profile, its own WebRTC port. Signing into
# Netflix in f2077's browser does not touch the XyberRP room's session, and
# recreating f2077's container does not sign anybody out of theirs.
#
# Builds the image and (re)creates THAT role's container -- which also wipes that
# browser's own state (sessions, history). The first run generates neko.env
# (viewer and admin passwords, API token) and the television's link
# (tv-url.secret), both root-only; later runs reuse them, so the link a server's
# opx_tvbrowser holds does not change. No firewall and no nginx change (nginx.sh
# does the site). The other role's container, ports, files and link are not
# touched. Prints no secret.
#
# Widevine is NOT baked into the image (build/ still removes it, so the image
# stays redistributable without Google's CDM binary). It arrives at runtime as a
# read-only mount of the host's own Chrome copy
# (/opt/google/chrome/WidevineCdm); measured 2026-10-16, the room Chromium
# answers WIDEVINE_AVAILABLE with the mount and NotSupportedError without it.
#
# USAGE  ./up.sh [staging|production|f2077]       (default: staging)
set -euo pipefail

ROLE="${1:-staging}"
case "$ROLE" in
  staging)    D=/opt/open77-tvbrowser;        NAME=open77-tvbrowser-xbs;   PREFIX=/tv-browser-xb-staging; HTTP=18080; RTC=59100 ;;
  production) D=/opt/open77-tvbrowser-xb;     NAME=open77-tvbrowser-xb;    PREFIX=/tv-browser-xb;         HTTP=18081; RTC=59101 ;;
  f2077)      D=/opt/open77-tvbrowser-f2077; NAME=open77-tvbrowser-f2077; PREFIX=/tv-browser-f2077;      HTTP=18082; RTC=59102 ;;
  *) echo "usage: $(basename "$0") [staging|production|f2077]"; exit 2 ;;
esac
IMAGE=open77/tvbrowser-chromium:9
PUBLIC_HOST=xbuniverse.duckdns.org
PUBLIC_IP=88.214.59.166

[ -d "$D" ] || { echo "no install dir at $D: copy this server/ directory there first"; exit 2; }
cd "$D"
umask 077
if [ ! -f neko.env ]; then
  {
    echo "NEKO_SERVER_PROXY=true"
    echo "NEKO_SERVER_PATH_PREFIX=$PREFIX"
    echo "NEKO_DESKTOP_SCREEN=1280x720@30"
    echo "NEKO_MEMBER_PROVIDER=multiuser"
    echo "NEKO_MEMBER_MULTIUSER_USER_PASSWORD=$(openssl rand -hex 12)"
    echo "NEKO_MEMBER_MULTIUSER_ADMIN_PASSWORD=$(openssl rand -hex 24)"
    echo "NEKO_SESSION_API_TOKEN=$(openssl rand -hex 32)"
    echo "NEKO_SESSION_IMPLICIT_HOSTING=true"
    echo "NEKO_WEBRTC_UDMUX=$RTC"
    echo "NEKO_WEBRTC_TCPMUX=$RTC"
    echo "NEKO_WEBRTC_NAT1TO1=$PUBLIC_IP"
  } > neko.env
  echo "== neko.env generated for $ROLE (passwords and API token, root-only)"
fi
chmod 600 neko.env
# The television's link carries the viewer password, so it is root-only as well.
# An existing link that neko.env no longer matches is a mismatch to look at, not
# something to overwrite: the servers' opx_tvbrowser config was written from it.
P=$(grep '^NEKO_MEMBER_MULTIUSER_USER_PASSWORD=' neko.env | cut -d= -f2-)
LINK_ENV=$(printf 'https://%s%s/?usr=open77&pwd=%s&embed=1#open77-shared-browser\n' "$PUBLIC_HOST" "$PREFIX" "$P" | sha256sum | cut -c1-64)
if [ -s tv-url.secret ]; then
  [ "$(sha256sum < tv-url.secret | cut -c1-64)" = "$LINK_ENV" ] || { echo "tv-url.secret does not match neko.env: not recreating"; exit 1; }
else
  printf 'https://%s%s/?usr=open77&pwd=%s&embed=1#open77-shared-browser\n' "$PUBLIC_HOST" "$PREFIX" "$P" > tv-url.secret
  echo "== tv-url.secret written for $ROLE (root-only)"
fi
chmod 600 tv-url.secret

# The profile library: one directory per browser profile, on the HOST, mounted
# into the container at /profiles. Chromium runs on /profiles/active, a symlink,
# so switching is a re-point and a Chromium restart, and a recreated container
# comes back with the same cookies and sign-ins. 'house' is seeded on first run
# because Chromium will not start with a --user-data-dir that does not exist.
mkdir -p "$D/profiles"
chmod 700 "$D/profiles"
if [ ! -e "$D/profiles/active" ]; then
  mkdir -p "$D/profiles/house"
  ln -sfn house "$D/profiles/active"
  echo "== profile library created at $D/profiles (active -> house)"
fi
chown -R 1000:1000 "$D/profiles"
chown -h 1000:1000 "$D/profiles/active"

# The start page is the room's own splash, and it says which room it is: the
# image ships the template, this writes this role's copy and mounts it, so the
# two rooms do not show each other's state.
sed "s/__ROOM__/$ROLE/g" "$D/build/policies.json" > "$D/policies.json"
chmod 644 "$D/policies.json"

docker build -q -t "$IMAGE" "$D/build" >/dev/null
echo "== image $IMAGE built: $(docker image inspect "$IMAGE" --format '{{.Id}}' | cut -c1-19)"
# The host CDM this room's Widevine comes from: Google's official module from
# the host's own Chrome install, mounted read-only, never copied into the image.
CDM_SRC=/opt/google/chrome/WidevineCdm
[ -f "$CDM_SRC/manifest.json" ] || { echo "no host CDM at $CDM_SRC: not recreating"; exit 1; }
echo "== host CDM: $(grep -o '"version": *"[^"]*"' "$CDM_SRC/manifest.json" | head -1)"
docker run --rm --entrypoint sh "$IMAGE" -c \
  'grep -q "src=\"open77-ice.js\"></script><script src=\"open77-paste.js\"></script><script src=\"open77-volume.js\"></script><script src=\"open77-fit.js?v=3\"" /var/www/index.html && test -s /var/www/open77-ice.js && test -s /var/www/open77-paste.js && test -s /var/www/open77-volume.js && test -s /var/www/open77-fit.js && grep -q "\"NewTabPageLocation\"" /etc/chromium/policies/managed/policies.json && ! find / -xdev -iname "*widevine*" 2>/dev/null | grep -q .' \
  || { echo "the image is missing its scripts or its policy, or has a DRM module baked in: not recreating"; exit 1; }
docker run --rm --entrypoint sh "$IMAGE" -c 'for f in /etc/chromium/policies/managed/policies.json /etc/neko/supervisord/chromium.conf /var/www/open77-ice.js /var/www/open77-paste.js /var/www/open77-volume.js /var/www/open77-fit.js; do [ "$(stat -c %a "$f")" = 644 ] || exit 1; done' \
  || { echo "the image's policy or scripts are not readable by the browser: not recreating"; exit 1; }
docker run --rm --entrypoint sh "$IMAGE" -c 'grep -q -- "--user-data-dir=/profiles/active" /etc/neko/supervisord/chromium.conf && grep -q -- "--kiosk" /etc/neko/supervisord/chromium.conf && grep -q "program:cdp" /etc/neko/supervisord/chromium.conf && command -v socat' \
  || { echo "the image has no profile library, no kiosk screen or no DevTools forwarder: not recreating"; exit 1; }
echo "== image checked: client page loads open77-ice.js, open77-paste.js, open77-volume.js and open77-fit.js, all readable; policies in place; no Widevine baked in (it is mounted at run)"

if docker ps -a --format '{{.Names}}' | grep -qx "$NAME"; then
  # A stop first, not rm -f: Chromium is the only writer in /profiles and a
  # SIGKILL can leave the profile's Cookies WAL unwritten -- which is exactly
  # the sign-in this image exists to keep.
  docker stop -t 25 "$NAME" >/dev/null 2>&1 || true
  docker rm -f "$NAME" >/dev/null
  echo "== previous $ROLE container removed"
fi
docker run -d --name "$NAME" --restart unless-stopped \
  --cpus 4 --memory 4g --memory-swap 4g --pids-limit 2048 --shm-size 2g \
  --log-opt max-size=10m --log-opt max-file=3 \
  -p 127.0.0.1:$HTTP:8080 -p $RTC:$RTC/udp -p $RTC:$RTC/tcp \
  -v "$CDM_SRC:/usr/lib/chromium/WidevineCdm:ro" \
  -v "$D/profiles:/profiles" \
  -v "$D/policies.json:/etc/chromium/policies/managed/policies.json:ro" \
  --env-file "$D/neko.env" \
  "$IMAGE" >/dev/null
echo "== container $NAME started on $IMAGE ($ROLE: prefix $PREFIX, client on 127.0.0.1:$HTTP, WebRTC on $RTC)"
echo "== profiles: $(ls "$D/profiles" | grep -v '^active$' | tr '\n' ' ') (active -> $(readlink "$D/profiles/active"))"
code=000
for i in $(seq 1 30); do
  sleep 2
  code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP$PREFIX/" || true)
  [ "$code" = "200" ] && break
done
echo "== loopback client page: HTTP $code"
sleep 15
echo "== health: $(docker inspect "$NAME" --format '{{.State.Health.Status}}')"
docker logs --tail 10 "$NAME" 2>&1 | sed -E 's/\x1b\[[0-9;]*m//g' | grep -v -i -E "password|token|secret|pwd" | cut -c1-160 || true
echo "== next: ./nginx.sh $ROLE  (publishes $PREFIX through the site), then ./install-tv-config.sh $ROLE  (writes the game server's opx_tvbrowser config from this link)"
