#!/usr/bin/env bash
# The shared browser for Open77 televisions on XBUNIVERSE STAGING: neko's Chromium
# without Widevine, streamed over WebRTC (build/, run as image 5). Builds the
# image and (re)creates the container -- which also wipes the browser's own state
# (sessions, history). The first run generates neko.env (viewer and admin
# passwords, API token) and the televisions' link (tv-url.secret), both
# root-only; later runs reuse them, so the link a server's opx_tvbrowser holds
# does not change. No firewall or nginx change (nginx.sh does the site).
# Production servers are not touched. Prints no secret.
set -euo pipefail
D=/opt/open77-tvbrowser; NAME=open77-tvbrowser-xbs; IMAGE=open77/tvbrowser-chromium:5
cd "$D"
umask 077
if [ ! -f neko.env ]; then
  {
    echo "NEKO_SERVER_PROXY=true"
    echo "NEKO_SERVER_PATH_PREFIX=/tv-browser-xb-staging"
    echo "NEKO_DESKTOP_SCREEN=1280x720@30"
    echo "NEKO_MEMBER_PROVIDER=multiuser"
    echo "NEKO_MEMBER_MULTIUSER_USER_PASSWORD=$(openssl rand -hex 12)"
    echo "NEKO_MEMBER_MULTIUSER_ADMIN_PASSWORD=$(openssl rand -hex 24)"
    echo "NEKO_SESSION_API_TOKEN=$(openssl rand -hex 32)"
    echo "NEKO_SESSION_IMPLICIT_HOSTING=true"
    echo "NEKO_WEBRTC_UDPMUX=59100"
    echo "NEKO_WEBRTC_TCPMUX=59100"
    echo "NEKO_WEBRTC_NAT1TO1=88.214.59.166"
  } > neko.env
  echo "== neko.env generated (passwords and API token, root-only)"
fi
chmod 600 neko.env
# The television's link carries the viewer password, so it is root-only as well.
# An existing link that neko.env no longer matches is a mismatch to look at, not
# something to overwrite: the servers' opx_tvbrowser config was written from it.
P=$(grep '^NEKO_MEMBER_MULTIUSER_USER_PASSWORD=' neko.env | cut -d= -f2-)
LINK_ENV=$(printf 'https://xbuniverse.duckdns.org/tv-browser-xb-staging/?usr=open77&pwd=%s&embed=1#open77-shared-browser\n' "$P" | sha256sum | cut -c1-64)
if [ -s tv-url.secret ]; then
  [ "$(sha256sum < tv-url.secret | cut -c1-64)" = "$LINK_ENV" ] || { echo "tv-url.secret does not match neko.env: not recreating"; exit 1; }
else
  printf 'https://xbuniverse.duckdns.org/tv-browser-xb-staging/?usr=open77&pwd=%s&embed=1#open77-shared-browser\n' "$P" > tv-url.secret
  echo "== tv-url.secret written (root-only)"
fi
chmod 600 tv-url.secret

docker build -q -t "$IMAGE" "$D/build" >/dev/null
echo "== image $IMAGE built: $(docker image inspect "$IMAGE" --format '{{.Id}}' | cut -c1-19)"
docker run --rm --entrypoint sh "$IMAGE" -c \
  'grep -q "src=\"open77-ice.js\"></script><script src=\"open77-paste.js\"></script><script src=\"open77-volume.js\"" /var/www/index.html && test -s /var/www/open77-ice.js && test -s /var/www/open77-paste.js && test -s /var/www/open77-volume.js && grep -q "\"NewTabPageLocation\"" /etc/chromium/policies/managed/policies.json && ! find / -xdev -iname "*widevine*" 2>/dev/null | grep -q .' \
  || { echo "the image is missing its scripts or its policy, or still has a DRM module: not recreating"; exit 1; }
docker run --rm --entrypoint sh "$IMAGE" -c 'for f in /etc/chromium/policies/managed/policies.json /var/www/open77-ice.js /var/www/open77-paste.js /var/www/open77-volume.js; do [ "$(stat -c %a "$f")" = 644 ] || exit 1; done' \
  || { echo "the image's policy or scripts are not readable by the browser: not recreating"; exit 1; }
echo "== image checked: client page loads open77-ice.js, open77-paste.js and open77-volume.js, all readable; policies in place; no Widevine"

if docker ps -a --format '{{.Names}}' | grep -qx "$NAME"; then docker rm -f "$NAME" >/dev/null; echo "== previous container removed"; fi
docker run -d --name "$NAME" --restart unless-stopped \
  --cpus 4 --memory 4g --memory-swap 4g --pids-limit 2048 --shm-size 2g \
  --log-opt max-size=10m --log-opt max-file=3 \
  -p 127.0.0.1:18080:8080 -p 59100:59100/udp -p 59100:59100/tcp \
  --env-file "$D/neko.env" \
  "$IMAGE" >/dev/null
echo "== container $NAME started on $IMAGE"
code=000
for i in $(seq 1 30); do
  sleep 2
  code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18080/tv-browser-xb-staging/ || true)
  [ "$code" = "200" ] && break
done
echo "== loopback client page: HTTP $code"
sleep 15
echo "== health: $(docker inspect "$NAME" --format '{{.State.Health.Status}}')"
docker logs --tail 10 "$NAME" 2>&1 | sed -E 's/\x1b\[[0-9;]*m//g' | grep -v -i -E "password|token|secret|pwd" | cut -c1-160 || true
