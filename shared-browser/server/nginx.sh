#!/usr/bin/env bash
# Publishes one role's shared-browser path through the xbuniverse.duckdns.org
# site, and keeps the enabled and available copies of that site in step (the
# screen-mirror install edited only the enabled copy, so they part company; this
# leaves them equal again). Backs both up first, refuses to reload a
# configuration nginx -t rejects, puts the file back if it does, and then proves
# the new route answers through nginx.
#
#   role         path                       proxied to
#   staging      /tv-browser-xb-staging/    127.0.0.1:18080
#   production   /tv-browser-xb/            127.0.0.1:18081
#   f2077        /tv-browser-f2077/         127.0.0.1:18082
#   fr-us        /tv-browser-fr-us/         127.0.0.1:18083
#
# USAGE  ./nginx.sh [staging|production|f2077|fr-us]       (default: staging)
set -euo pipefail

ROLE="${1:-staging}"
case "$ROLE" in
  staging)    PREFIX=/tv-browser-xb-staging; PORT=18080; ANCHOR='location /opx-watch-xb-staging/' ;;
  production) PREFIX=/tv-browser-xb;         PORT=18081; ANCHOR='location /tv-browser-xb-staging/' ;;
  f2077)      PREFIX=/tv-browser-f2077;      PORT=18082; ANCHOR='location /tv-browser-xb/' ;;
  fr-us)      PREFIX=/tv-browser-fr-us;      PORT=18083; ANCHOR='location /tv-browser-f2077/' ;;
  *) echo "usage: $(basename "$0") [staging|production|f2077|fr-us]"; exit 2 ;;
esac

F=/etc/nginx/sites-enabled/redsync-cdn; A=/etc/nginx/sites-available/redsync-cdn
TS=$(date -u +%Y%m%d-%H%M%S); BK=/opt/open77-backups
[ -f "$F" ] || { echo "no site at $F: not editing"; exit 1; }
if grep -q "location $PREFIX/" "$F"; then echo "== $PREFIX is already in $F"; exit 0; fi
grep -q "$ANCHOR" "$F" || { echo "the anchor ($ANCHOR) is not in $F: not editing"; exit 1; }
cp -p "$F" "$BK/redsync-cdn.before-tvbrowser-$TS"
cp -p "$A" "$BK/redsync-cdn.available.before-tvbrowser-$TS" 2>/dev/null || true

LINE='    location '"$PREFIX"'/ { proxy_pass http://127.0.0.1:'"$PORT"'; proxy_http_version 1.1; proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection "upgrade"; proxy_set_header Host $host; proxy_set_header X-Real-IP $remote_addr; proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for; proxy_set_header X-Forwarded-Proto $scheme; proxy_read_timeout 300s; proxy_send_timeout 300s; proxy_buffering off; }'
python3 - "$F" "$LINE" "$ANCHOR" <<'PY'
import sys
path, line, anchor = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read().split("\n")
at = [i for i, l in enumerate(text) if anchor in l]
assert len(at) == 1, at
text.insert(at[0] + 1, line)
open(path, "w").write("\n".join(text))
PY

restore() { cp -p "$BK/redsync-cdn.before-tvbrowser-$TS" "$F"; systemctl reload nginx || true; }
if ! nginx -t 2>/tmp/nginx-t.log; then cat /tmp/nginx-t.log; echo "!! nginx -t refused it: restored"; restore; exit 1; fi
systemctl reload nginx

# A graceful reload hands new connections to the new workers only once the old
# ones stop accepting, so the first request after it can still be answered by a
# worker holding the old configuration. Retry before believing a 404.
code=000
for i in $(seq 1 15); do
  code=$(curl -s -o /dev/null -w '%{http_code}' --resolve xbuniverse.duckdns.org:443:127.0.0.1 "https://xbuniverse.duckdns.org$PREFIX/" || true)
  [ "$code" = "200" ] && break
  sleep 1
done
if [ "$code" != "200" ]; then echo "!! $PREFIX answered HTTP $code through nginx after $i tries: restored"; restore; exit 1; fi
cp -p "$F" "$A"
echo "== nginx reloaded with $PREFIX/ -> 127.0.0.1:$PORT (backup $BK/redsync-cdn.before-tvbrowser-$TS)"
echo "== through nginx: HTTP $code"
echo "== an unrelated route still answers: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/healthz)"
echo "== the other role's route still answers: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/tv-browser-xb-staging/)"
