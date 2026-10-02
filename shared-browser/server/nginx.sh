#!/usr/bin/env bash
# Adds /tv-browser-xb-staging/ (the shared browser, websocket) to the
# xbuniverse.duckdns.org site. Backs up both copies of the site first, refuses
# to reload a configuration nginx -t rejects. Other locations are untouched.
set -euo pipefail
F=/etc/nginx/sites-enabled/redsync-cdn; A=/etc/nginx/sites-available/redsync-cdn
TS=$(date -u +%Y%m%d-%H%M%S); BK=/opt/open77-backups
if grep -q 'location /tv-browser-xb-staging/' "$F"; then echo "== already configured"; exit 0; fi
cmp -s "$F" "$A" || { echo "the enabled and available copies differ: not editing"; exit 1; }
cp -p "$F" "$BK/redsync-cdn.before-tvbrowser-$TS"
LINE='    location /tv-browser-xb-staging/ { proxy_pass http://127.0.0.1:18080; proxy_http_version 1.1; proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection "upgrade"; proxy_set_header Host $host; proxy_set_header X-Real-IP $remote_addr; proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for; proxy_set_header X-Forwarded-Proto $scheme; proxy_read_timeout 300s; proxy_send_timeout 300s; proxy_buffering off; }'
python3 - "$F" "$LINE" <<'PY'
import sys
path, line = sys.argv[1], sys.argv[2]
text = open(path).read().split("\n")
at = [i for i, l in enumerate(text) if "location /opx-watch-xb-staging/" in l]
assert len(at) == 1, at
text.insert(at[0] + 1, line)
open(path, "w").write("\n".join(text))
PY
if ! nginx -t 2>/tmp/nginx-t.log; then
  cp -p "$BK/redsync-cdn.before-tvbrowser-$TS" "$F"; cat /tmp/nginx-t.log; echo "!! nginx -t refused it: restored"; exit 1
fi
cp -p "$F" "$A"
systemctl reload nginx
echo "== nginx reloaded with /tv-browser-xb-staging/ (backup $BK/redsync-cdn.before-tvbrowser-$TS)"
echo "== through nginx: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/tv-browser-xb-staging/)"
echo "== an unrelated route still answers: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve xbuniverse.duckdns.org:443:127.0.0.1 https://xbuniverse.duckdns.org/healthz)"
