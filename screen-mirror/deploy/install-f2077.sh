#!/usr/bin/env bash
# A SECOND screen-mirror relay, owned by one game server.
#
# The xbuniverse box already runs the shared relay (open77-mirror.service, port
# 8787, /screen-mirror/, one MIRROR_TOKEN). A second game server on the same box
# (f2077) should not share that token: the token is what makes a channel
# unwatchable by everyone who merely knows its name, so one token for two
# servers means a player handed f2077's link can join XyberRP's channel by
# guessing "WORK". This installs f2077 its own relay process, its own port, its
# own path and its own token -- and then writes f2077's opx_screenmirror config
# from that token, so its capture cinemas are put up with its own channel.
#
# What it does, additively:
#   1. copies the live relay (relay.mjs, sender.html, receiver.html, ws) to
#      /opt/open77-screen-mirror-f2077, owned by the unprivileged `mirror` user
#   2. installs open77-mirror-f2077.service on 127.0.0.1:8788, with the same
#      TURN/STUN environment file as the shared relay (/etc/open77-mirror.env)
#   3. adds location /screen-mirror-f2077/ to the nginx site -- backed up,
#      nginx -t checked, restored if it is refused, and proved through nginx
#   4. writes f2077's opx_screenmirror/server/config.lua (0600 root), keeping the
#      previous file, ending in the mirror mark open77_media looks for
#
# The shared relay, its token, its channel and XyberRP's config are not touched.
#
# USAGE  bash install-f2077.sh        (safe to re-run; the token is kept)
set -euo pipefail

NAME=f2077
SERVICE="open77-mirror-$NAME.service"
DEST="/opt/open77-screen-mirror-$NAME"
PORT=8788
SITE_PATH=/screen-mirror-f2077
CHANNEL=F2077
TOKEN_FILE="/root/.open77-mirror-$NAME-token"
SRC=/opt/open77-screen-mirror
SITE=/etc/nginx/sites-enabled/redsync-cdn
AVAIL=/etc/nginx/sites-available/redsync-cdn
SRV=/opt/open77-f2077
CFG="$SRV/resources/opx_screenmirror/server/config.lua"
PUBLIC_HOST=xbuniverse.duckdns.org
STAMP="$(date -u +%Y%m%d-%H%M%S)"

echo "== open77 screen-mirror: $NAME's own relay ($PORT, $SITE_PATH, channel $CHANNEL)"

# --- 0. where the relay code comes from ------------------------------------
if [ ! -f "$SRC/relay.mjs" ]; then
  HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  [ -f "$HERE/relay.mjs" ] || { echo "!! no relay.mjs at $SRC or $HERE"; exit 2; }
  SRC="$HERE"
fi

# --- 1. its own install dir, unprivileged owner ----------------------------
id -u mirror >/dev/null 2>&1 || useradd --system --home /opt/open77-screen-mirror --shell /usr/sbin/nologin mirror
mkdir -p "$DEST"
for f in relay.mjs sender.html receiver.html package.json; do
  [ -f "$SRC/$f" ] && cp -f "$SRC/$f" "$DEST/$f"
done
# The relay's one dependency, copied rather than fetched: the box is already
# running it, and a second npm install would only be a second place to differ.
if [ -d "$SRC/node_modules" ]; then
  cp -a "$SRC/node_modules" "$DEST/" 2>/dev/null || true
elif [ ! -d "$DEST/node_modules" ]; then
  ( cd "$DEST" && npm install --omit=dev --no-audit --no-fund >/dev/null )
fi
chown -R mirror:mirror "$DEST"
echo "-- relay code in $DEST ($(ls "$DEST" | tr '\n' ' '))"

# --- 2. its own token ------------------------------------------------------
umask 077
if [ -s "$TOKEN_FILE" ]; then
  TOKEN="$(tr -d '\r\n' < "$TOKEN_FILE")"
  echo "-- token reused from $TOKEN_FILE"
else
  TOKEN="$(openssl rand -hex 16)"
  printf '%s\n' "$TOKEN" > "$TOKEN_FILE"
  echo "-- token generated and written to $TOKEN_FILE (root-only)"
fi
[ "${#TOKEN}" -eq 32 ] || { echo "!! $TOKEN_FILE does not hold a 32-hex token"; exit 1; }

# --- 3. its own unit -------------------------------------------------------
cat > "/etc/systemd/system/$SERVICE" <<UNIT
[Unit]
Description=open77 screen-mirror relay for $NAME (view-only screen mirror)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$DEST
ExecStart=/usr/bin/node $DEST/relay.mjs
Environment=PORT=$PORT
Environment=HOST=127.0.0.1
Environment=MIRROR_TOKEN=$TOKEN
EnvironmentFile=-/etc/open77-mirror.env
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
chmod 600 "/etc/systemd/system/$SERVICE"
systemctl daemon-reload
systemctl enable "$SERVICE" >/dev/null
systemctl restart "$SERVICE"
sleep 1
ACTIVE="$(systemctl is-active "$SERVICE" || true)"
echo "-- $SERVICE: $ACTIVE"
if [ "$ACTIVE" != "active" ]; then
  echo "!! relay did not start; logs:"
  journalctl -u "$SERVICE" -n 20 --no-pager || true
  exit 1
fi

# --- 4. nginx: its own path, additively ------------------------------------
if grep -q "location $SITE_PATH/" "$SITE"; then
  echo "-- nginx already has $SITE_PATH/"
else
  BK=/opt/open77-backups
  mkdir -p "$BK"
  cp -p "$SITE" "$BK/redsync-cdn.before-$NAME-mirror-$STAMP"
  cp -p "$AVAIL" "$BK/redsync-cdn.available.before-$NAME-mirror-$STAMP" 2>/dev/null || true
  python3 - "$SITE" "$SITE_PATH" "$PORT" <<'PY'
import sys
path, prefix, port = sys.argv[1], sys.argv[2], sys.argv[3]
block = """
    # --- open77 screen-mirror: %s (view-only screen relay, its own token) ---
    location %s/ {
        proxy_pass http://127.0.0.1:%s/;
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
""" % (prefix.strip("/"), prefix, port)
s = open(path).read()
marker = "    location / { return 404; }"
if marker not in s:
    raise SystemExit("no final 404 location to insert before: not editing")
if s.count(marker) != 1:
    raise SystemExit("more than one final 404 location: not editing")
s = s.replace(marker, block + marker, 1)
open(path, "w").write(s)
print("-- nginx config updated")
PY
  if ! nginx -t 2>/tmp/nginx-t-$NAME.log; then
    cat /tmp/nginx-t-$NAME.log
    cp -p "$BK/redsync-cdn.before-$NAME-mirror-$STAMP" "$SITE"
    systemctl reload nginx || true
    echo "!! nginx -t refused it: restored"
    exit 1
  fi
  systemctl reload nginx
  echo "-- nginx reloaded"
fi

# --- 5. prove both the loopback and the public path ------------------------
ok=0
for _ in $(seq 1 20); do
  if curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then ok=1; break; fi
  sleep 1
done
[ "$ok" = "1" ] || { echo "!! loopback /health on $PORT did not answer"; exit 1; }
echo "-- loopback /health OK on $PORT"

code=000
for _ in $(seq 1 15); do
  code=$(curl -s -o /dev/null -w '%{http_code}' --resolve "$PUBLIC_HOST:443:127.0.0.1" \
    "https://$PUBLIC_HOST$SITE_PATH/health" || true)
  [ "$code" = "200" ] && break
  sleep 1
done
[ "$code" = "200" ] || { echo "!! $SITE_PATH/health answered HTTP $code through nginx"; exit 1; }
echo "-- through nginx: $SITE_PATH/health HTTP $code"
if [ -f "$AVAIL" ]; then cp -p "$SITE" "$AVAIL"; fi
echo "-- unrelated route still: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve "$PUBLIC_HOST:443:127.0.0.1" "https://$PUBLIC_HOST/healthz")"
echo "-- XyberRP's relay still: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve "$PUBLIC_HOST:443:127.0.0.1" "https://$PUBLIC_HOST/screen-mirror/health")"

# --- 6. the game server's link --------------------------------------------
if [ ! -f "$CFG" ]; then
  echo "!! no opx_screenmirror config at $CFG: relay installed, link not written"
  exit 1
fi
LINK="https://$PUBLIC_HOST$SITE_PATH/receiver?channel=$CHANNEL&token=$TOKEN"
BK="/opt/open77-backups/$(basename "$SRV")-screenmirror-config-$STAMP"
mkdir -p "$BK"
cp -a "$CFG" "$BK/config.lua"
umask 077
{
  printf -- '-- opx_screenmirror -- server/config.lua\n'
  printf -- '--\n'
  printf -- '-- Written on the server at deploy (root-only) with the screen-mirror viewer\n'
  printf -- '-- link. This copy is the placeholder a bundle carries: with no link, a capture\n'
  printf -- '-- cinema says the capture is not set up here.\n'
  printf -- 'OpxScreenMirrorConfig = {\n'
  printf -- '    -- The relay viewer page for the channel the remote PC publishes to. A link\n'
  printf -- '    -- ending in the mirror mark, or one without it (the mark is added for you).\n'
  printf -- '    url = "%s",\n' "$LINK"
  printf -- '    cinemaRecord = "cinema.150ft.capture",\n'
  printf -- '    cinema100Record = "cinema.100ft.capture",\n'
  printf -- '}\n'
} > "$CFG"
chmod 600 "$CFG"; chown root:root "$CFG"
echo "-- wrote $CFG for channel $CHANNEL (0600 root:root; previous kept at $BK/config.lua)"

echo
echo "== installed =="
echo " sender (remote PC): https://$PUBLIC_HOST$SITE_PATH/?channel=$CHANNEL&token=<in $TOKEN_FILE>&source=screen"
echo " viewer (in game):   https://$PUBLIC_HOST$SITE_PATH/receiver?channel=$CHANNEL&token=<in $TOKEN_FILE>"
echo " token file:         $TOKEN_FILE (root-only)"
echo " restart the game server to pick the link up: systemctl restart open77-f2077"
