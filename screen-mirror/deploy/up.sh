#!/usr/bin/env bash
# Installs the open77 screen-mirror relay on a Debian/Ubuntu VPS and starts it
# behind Caddy (automatic HTTPS). Run as root from the copied screen-mirror/ dir:
#
#   sudo MIRROR_TOKEN=... ./deploy/up.sh mirror.example.com
#
# What it does: installs Node.js if missing, copies the relay to
# /opt/open77-screen-mirror, installs its one dependency (ws), runs it as an
# unprivileged user via systemd, and (if you give a domain) fronts it with Caddy
# for HTTPS + WSS. Screen capture needs HTTPS, so a domain is effectively
# required for the real sender.
#
# Re-running is safe: it re-copies, reinstalls and restarts.

set -euo pipefail

DOMAIN="${1:-${MIRROR_DOMAIN:-}}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../screen-mirror
DEST=/opt/open77-screen-mirror
TOKEN="${MIRROR_TOKEN:-$(openssl rand -hex 16)}"

echo "== open77 screen-mirror relay install =="
echo "   source: $HERE"
echo "   dest:   $DEST"
if [ -n "$DOMAIN" ]; then echo "   domain: $DOMAIN"; else
  echo "   domain: (none) -- relay on plain HTTP:8787; screen capture needs HTTPS, so pass a domain."
fi

# --- 1. Node.js -------------------------------------------------------------
if ! command -v node >/dev/null 2>&1; then
  echo "-- installing Node.js 20 (NodeSource)"
  curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
  apt-get install -y nodejs
fi
echo "-- node $(node -v)"

# --- 2. Copy + install the one dependency (ws) ------------------------------
echo "-- installing to $DEST"
mkdir -p "$DEST"
cp "$HERE"/relay.mjs "$HERE"/sender.html "$HERE"/receiver.html "$HERE"/package.json "$DEST"/
cd "$DEST"
npm install --omit=dev --no-audit --no-fund

# --- 3. Unprivileged user ---------------------------------------------------
id -u mirror >/dev/null 2>&1 || useradd --system --home "$DEST" --shell /usr/sbin/nologin mirror
chown -R mirror:mirror "$DEST"

# --- 4. systemd unit --------------------------------------------------------
echo "-- installing systemd unit"
sed -e "s|__DEST__|$DEST|g" -e "s|__TOKEN__|$TOKEN|g" "$HERE/deploy/relay.service" > /etc/systemd/system/open77-mirror.service
systemctl daemon-reload
systemctl enable --now open77-mirror.service
sleep 1
systemctl --no-pager --lines=0 status open77-mirror.service >/dev/null && echo "-- relay running"

# --- 5. Caddy (automatic HTTPS) --------------------------------------------
if [ -n "$DOMAIN" ]; then
  if ! command -v caddy >/dev/null 2>&1; then
    echo "-- installing Caddy"
    apt-get install -y debian-keyring debian-archive-keyring apt-transport-https curl gnupg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
      | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
    curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
      | tee /etc/apt/sources.list.d/caddy-stable.list >/dev/null
    apt-get update && apt-get install -y caddy
  fi
  echo "-- installing Caddyfile for $DOMAIN"
  sed -e "s|__DOMAIN__|$DOMAIN|g" "$HERE/deploy/Caddyfile" > /etc/caddy/Caddyfile
  systemctl reload caddy 2>/dev/null || systemctl restart caddy
fi

echo
echo "== done =="
if [ -n "$DOMAIN" ]; then
  echo " On the remote PC (your work machine), open in a browser and pick the screen:"
  echo "   https://$DOMAIN/?channel=WORK&token=$TOKEN&source=screen"
  echo " The in-game viewer (the page the game frames) is:"
  echo "   https://$DOMAIN/receiver?channel=WORK&token=$TOKEN"
  echo " Health:"
  echo "   https://$DOMAIN/health"
else
  echo " Relay: http://<vps-ip>:8787   token: $TOKEN"
  echo " Pass a domain to up.sh for HTTPS (needed for real screen capture)."
fi
echo " Keep the token safe; anyone with the link + token can watch the channel."
