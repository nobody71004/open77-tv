#!/usr/bin/env bash
# The shared browser for Open77 televisions on XBUNIVERSE STAGING: neko's Chromium
# without Widevine, streamed over WebRTC. Builds the image and (re)creates the
# container -- which also wipes the browser's own state (sessions, history).
# Production servers are not touched. Prints no secret.
set -euo pipefail
D=/opt/open77-tvbrowser; NAME=open77-tvbrowser-xbs; IMAGE=open77/tvbrowser-chromium:1
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
docker build -q -t "$IMAGE" "$D/build" >/dev/null
echo "== image $IMAGE built: $(docker image inspect "$IMAGE" --format '{{.Id}}' | cut -c1-19)"
if docker ps -a --format '{{.Names}}' | grep -qx "$NAME"; then docker rm -f "$NAME" >/dev/null; echo "== previous container removed"; fi
docker run -d --name "$NAME" --restart unless-stopped \
  --cpus 4 --memory 4g --memory-swap 4g --pids-limit 2048 --shm-size 2g \
  --log-opt max-size=10m --log-opt max-file=3 \
  -p 127.0.0.1:18080:8080 -p 59100:59100/udp -p 59100:59100/tcp \
  --env-file "$D/neko.env" \
  "$IMAGE" >/dev/null
echo "== container $NAME started"
# The television's link carries the viewer password, so it is root-only as well.
P=$(grep '^NEKO_MEMBER_MULTIUSER_USER_PASSWORD=' neko.env | cut -d= -f2-)
printf 'https://xbuniverse.duckdns.org/tv-browser-xb-staging/?usr=open77&pwd=%s&embed=1#open77-shared-browser\n' "$P" > tv-url.secret
chmod 600 tv-url.secret
for i in $(seq 1 30); do
  sleep 2
  code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18080/tv-browser-xb-staging/ || true)
  [ "$code" = "200" ] && break
done
echo "== loopback client page: HTTP $code"
docker logs --tail 15 "$NAME" 2>&1 | grep -v -i -E "password|token|secret|pwd" | cut -c1-200 || true
