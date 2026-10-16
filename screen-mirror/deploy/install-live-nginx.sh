#!/usr/bin/env bash
# Installs the open77 screen-mirror relay on the xbuniverse box (which already
# runs nginx + a Let's Encrypt cert for xbuniverse.duckdns.org). It mounts the
# relay at https://xbuniverse.duckdns.org/screen-mirror/ , additively: it does
# NOT touch any running game server, the shared-browser container, or nginx's
# existing locations. Run as root on the box:
#
#   bash install-live-nginx.sh
#
# Everything is additive and reversible: the nginx config is backed up, the
# unit is a new file, and the code lives in its own /opt directory.

set -euo pipefail

DEST=/opt/open77-screen-mirror
SITE=/etc/nginx/sites-enabled/redsync-cdn
TOKEN="${MIRROR_TOKEN:-$(openssl rand -hex 16)}"

echo "== open77 screen-mirror: live install (nginx) =="

# --- token (root-only) ------------------------------------------------------
umask 077
echo "$TOKEN" > /root/.open77-mirror-token
echo "-- token written to /root/.open77-mirror-token"

# --- dedicated unprivileged user -------------------------------------------
id -u mirror >/dev/null 2>&1 || useradd --system --home "$DEST" --shell /usr/sbin/nologin mirror
chown -R mirror:mirror "$DEST"
echo "-- files owned by mirror"

# --- systemd unit ----------------------------------------------------------
cat > /etc/systemd/system/open77-mirror.service <<UNIT
[Unit]
Description=open77 screen-mirror relay (view-only screen mirror)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$DEST
ExecStart=/usr/bin/node $DEST/relay.mjs
Environment=PORT=8787
Environment=HOST=127.0.0.1
Environment=MIRROR_TOKEN=$TOKEN
Restart=always
RestartSec=2
User=mirror
Group=mirror
NoNewPrivileges=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=full

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable --now open77-mirror.service
sleep 1
ACTIVE=$(systemctl is-active open77-mirror.service || true)
echo "-- relay service: $ACTIVE"
if [ "$ACTIVE" != "active" ]; then
  echo "!! relay did not start; logs:"; journalctl -u open77-mirror.service -n 20 --no-pager || true
  exit 1
fi

# --- nginx: add /screen-mirror/ before the catch-all 404 -------------------
if grep -q 'location /screen-mirror/' "$SITE"; then
  echo "-- nginx location already present"
else
  cp "$SITE" "$SITE.bak-mirror-$(date +%Y%m%d-%H%M%S)"
  python3 - "$SITE" <<'PY'
import sys
path = sys.argv[1]
block = """
    # --- open77 screen-mirror (view-only screen relay) ---
    location /screen-mirror/ {
        proxy_pass http://127.0.0.1:8787/;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_read_timeout 3600s;
        proxy_send_timeout 3600s;
        proxy_buffering off;
    }
"""
s = open(path).read()
marker = "    location / { return 404; }"
if marker in s:
    s = s.replace(marker, block + marker, 1)
else:
    s = s.rstrip() + "\n" + block + "\n"
open(path, "w").write(s)
print("-- nginx config updated")
PY
  nginx -t
  systemctl reload nginx
  echo "-- nginx reloaded"
fi

# --- quick local health check ---------------------------------------------
sleep 1
if curl -fsS http://127.0.0.1:8787/health >/dev/null 2>&1; then
  echo "-- local /health OK"
else
  echo "!! local /health failed"; exit 1
fi

echo
echo "== installed =="
echo " sender (remote PC):  https://xbuniverse.duckdns.org/screen-mirror/?channel=WORK&token=$TOKEN&source=screen"
echo " viewer (in game):    https://xbuniverse.duckdns.org/screen-mirror/receiver?channel=WORK&token=$TOKEN"
echo " health:              https://xbuniverse.duckdns.org/screen-mirror/health"
echo " token also in:       /root/.open77-mirror-token"
