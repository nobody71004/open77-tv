#!/usr/bin/env bash
# A screen-mirror relay for one room.
#
# The box runs the shared relay (open77-mirror.service, port 8787,
# /screen-mirror/, one MIRROR_TOKEN) for XyberRP, and a second one for f2077
# (install-f2077.sh, port 8788). A third game server cannot borrow either: the
# token is what makes a channel unwatchable by everyone who merely knows its name,
# so one token for two servers means a player handed one room's link can join the
# other room's channel by guessing "WORK". Each room that shows the Mirror app
# therefore gets its own process, its own port, its own path and its own token --
# and its opx_screenmirror config is written from that token, which is the file
# the room service reads to build the app.
#
# What it does, additively (the other relays and their configs are untouched):
#   1. copies the live relay (relay.mjs, sender.html, receiver.html, package.json)
#      to /opt/open77-screen-mirror-<name>, owned by the unprivileged `mirror` user
#   2. installs open77-mirror-<name>.service on 127.0.0.1:<port>, with the same
#      TURN/STUN environment file as the shared relay (/etc/open77-mirror.env)
#   3. adds location <path>/ to the nginx site -- backed up, `nginx -t` checked,
#      restored if it is refused, and proved through nginx both ways
#   4. writes <install>/resources/opx_screenmirror/server/config.lua (0600 root),
#      keeping the previous file, naming this room's channel and token
#
# USAGE
#   bash install-relay.sh <name> <port> <site-path> <channel> <server-install-dir>
#
#   bash install-relay.sh fr-us 8789 /screen-mirror-fr-us FRUS /opt/open77-freeroamusa
#
# Safe to re-run: the token and the nginx block are kept once written. The game
# server needs a restart afterwards to pick its new link up (and, the first time,
# to start the opx_screenmirror resource at all).
set -euo pipefail

NAME="${1:?usage: install-relay.sh <name> <port> <site-path> <channel> <server-install-dir>}"
PORT="${2:?missing port}"
SITE_PATH="${3:?missing site path, e.g. /screen-mirror-fr-us}"
CHANNEL="${4:?missing channel name}"
SRV="${5:?missing the install directory of the game server}"

SERVICE="open77-mirror-$NAME.service"
DEST="/opt/open77-screen-mirror-$NAME"
TOKEN_FILE="/root/.open77-mirror-$NAME-token"
SRC=/opt/open77-screen-mirror
SITE=/etc/nginx/sites-enabled/redsync-cdn
AVAIL=/etc/nginx/sites-available/redsync-cdn
CFG="$SRV/resources/opx_screenmirror/server/config.lua"
PUBLIC_HOST=xbuniverse.duckdns.org
STAMP="$(date -u +%Y%m%d-%H%M%S)"

[[ "$SITE_PATH" == /* ]] || { echo "!! site path must start with /"; exit 2; }
SITE_PATH="${SITE_PATH%/}"

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

# --- 3. its own unit ------------------------------------------------------
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

# --- 5. prove the loopback and the public path, and that the others still work
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
echo "-- nginx itself still: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve "$PUBLIC_HOST:443:127.0.0.1" "https://$PUBLIC_HOST/healthz")"
echo "-- XyberRP's relay still: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve "$PUBLIC_HOST:443:127.0.0.1" "https://$PUBLIC_HOST/screen-mirror/health")"
echo "-- f2077's relay still: HTTP $(curl -s -o /dev/null -w '%{http_code}' --resolve "$PUBLIC_HOST:443:127.0.0.1" "https://$PUBLIC_HOST/screen-mirror-f2077/health")"

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
  printf -- '-- link. This copy is the placeholder a bundle carries: with no link, the\n'
  printf -- '-- room Mirror app says the mirror is not set up here.\n'
  printf -- 'OpxScreenMirrorConfig = {\n'
  printf -- '    -- The relay viewer page for the channel the remote PC publishes to. A link\n'
  printf -- '    -- ending in the mirror mark, or one without it (the mark is added for you).\n'
  printf -- '    -- The room service on this host reads this file for its Mirror app.\n'
  printf -- '    url = "%s",\n' "$LINK"
  printf -- '}\n'
} > "$CFG"
chmod 600 "$CFG"; chown root:root "$CFG"
echo "-- wrote $CFG for channel $CHANNEL (0600 root:root; previous kept at $BK/config.lua)"

echo
echo "== installed =="
echo " sender (remote PC): https://$PUBLIC_HOST$SITE_PATH/?channel=$CHANNEL&token=<in $TOKEN_FILE>&source=screen"
echo " viewer (in game):   https://$PUBLIC_HOST$SITE_PATH/receiver?channel=$CHANNEL&token=<in $TOKEN_FILE>"
echo " token file:         $TOKEN_FILE (root-only)"
echo " restart the game server to pick the link up (its own service, e.g."
echo "   systemctl restart open77-freeroamusa for fr-us)"
