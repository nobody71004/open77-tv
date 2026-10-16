#!/bin/bash
# open77 screen-mirror: one-shot VPS deploy (runs remotely over ONE ssh session).
#   1. install updated relay.mjs / sender.html / receiver.html / token.mjs
#   2. fix coturn TURN auth: register webrtcuser with the relay's fixed credential
#   3. restart relay + coturn, print verification info
# Files arrive beside this script in $SRC (set by the caller).
set -u
SRC="${SRC:-/tmp/sm-deploy}"
DEST="${DEST:-/opt/screen-mirror}"
# The live box (install-live-nginx.sh layout) keeps the relay in
# /opt/open77-screen-mirror; fall back to it so a bare run works either way.
if [ ! -d "$DEST" ] && [ -d /opt/open77-screen-mirror ]; then DEST=/opt/open77-screen-mirror; fi
STAMP="$(date +%Y%m%d-%H%M%S)"

echo ">> deploy from $SRC to $DEST"
if [ ! -d "$DEST" ]; then echo "!! $DEST missing"; exit 1; fi

echo ">> backup current files"
for f in relay.mjs sender.html receiver.html token.mjs; do
  [ -f "$DEST/$f" ] && cp "$DEST/$f" "$DEST/$f.bak-$STAMP"
done

echo ">> install new files"
for f in relay.mjs sender.html receiver.html token.mjs; do
  if [ -f "$SRC/$f" ]; then cp "$SRC/$f" "$DEST/$f" && echo "   installed $f"; else echo "   !! missing $f in bundle"; fi
done

echo ">> syntax-check relay"
node --check "$DEST/relay.mjs" && echo "   relay OK" || { echo "!! relay syntax failed - rolling back"; cp "$DEST/relay.mjs.bak-$STAMP" "$DEST/relay.mjs"; exit 1; }

echo ">> locate relay service"
UNIT="$(systemctl list-units --type=service --no-legend | awk '{print $1}' | grep -iE 'screen|mirror|relay' | head -1)"
echo "   unit: ${UNIT:-<none found>}"

echo ">> coturn: register webrtcuser with the relay's fixed credential"
# The relay hands out TURN_USER=webrtcuser + TURN_PASS (32-hex fixed). coturn
# must have a matching long-term user. Values are pulled from the relay's env
# file(s) so the secret never appears in this transcript.
ENVFILE="$(systemctl show -p EnvironmentFiles --value ${UNIT:-nonexistent} 2>/dev/null | tr ' ' '\n' | head -1)"
[ -z "$ENVFILE" ] && ENVFILE="$(ls "$DEST/.env" /opt/screen-mirror/.env /opt/open77-screen-mirror/.env /etc/screen-mirror.env 2>/dev/null | head -1)"
TURN_USER_VAL=""
TURN_PASS_VAL=""
if [ -n "$ENVFILE" ] && [ -f "$ENVFILE" ]; then
  TURN_USER_VAL="$(grep -E '^TURN_USER=' "$ENVFILE" | head -1 | cut -d= -f2- | tr -d '\"' )"
  TURN_PASS_VAL="$(grep -E '^TURN_PASS=' "$ENVFILE" | head -1 | cut -d= -f2- | tr -d '\"' )"
  echo "   env: $ENVFILE  (user=${TURN_USER_VAL:-webrtcuser}  pass=<${#TURN_PASS_VAL} chars>)"
fi
# Fallback: the unit's inline Environment= lines (install-live-nginx.sh layout).
if [ -z "$TURN_PASS_VAL" ] && [ -n "$UNIT" ]; then
  UENV="$(systemctl show -p Environment --value "$UNIT" 2>/dev/null)"
  TURN_USER_VAL="$(echo "$UENV" | tr ' ' '\n' | grep -E '^TURN_USER=' | head -1 | cut -d= -f2-)"
  TURN_PASS_VAL="$(echo "$UENV" | tr ' ' '\n' | grep -E '^TURN_PASS=' | head -1 | cut -d= -f2-)"
  [ -n "$TURN_PASS_VAL" ] && echo "   env: unit $UNIT Environment  (user=${TURN_USER_VAL:-webrtcuser}  pass=<${#TURN_PASS_VAL} chars>)"
fi
[ -n "$TURN_USER_VAL" ] || TURN_USER_VAL="webrtcuser"
if [ -z "$TURN_PASS_VAL" ]; then
  echo "   !! TURN_PASS not found in env file or unit; cannot register coturn user - leaving coturn untouched"
fi

if [ -n "$TURN_PASS_VAL" ]; then
  if grep -qE '^\s*use-auth-secret' /etc/turnserver.conf 2>/dev/null; then
    echo "   coturn uses use-auth-secret (REST) mode; static user not applicable - switching to lt-cred-mech"
    CONF_BACKUP="/etc/turnserver.conf.bak-$STAMP"
    sudo cp /etc/turnserver.conf "$CONF_BACKUP"
    sudo sed -i 's/^\s*use-auth-secret/# use-auth-secret (disabled: static user auth)/' /etc/turnserver.conf
    sudo sed -i 's/^\s*static-auth-secret.*/# static-auth-secret (disabled)/' /etc/turnserver.conf
  fi
  sudo turnadmin -a -u "$TURN_USER_VAL" -p "$TURN_PASS_VAL" -r "$(hostname -f 2>/dev/null || echo xbuniverse.duckdns.org)" 2>&1 | tail -2
  echo "   turnadmin done (user=$TURN_USER_VAL)"
  sudo systemctl restart coturn 2>&1 | tail -1
  sleep 1
  systemctl is-active coturn && echo "   coturn active" || echo "   !! coturn NOT active"
fi

echo ">> restart relay (and enable both services at boot)"
if [ -n "$UNIT" ]; then
  sudo systemctl enable "$UNIT" 2>&1 | tail -1
  sudo systemctl restart "$UNIT" 2>&1 | tail -1
  sleep 1
  systemctl is-active "$UNIT" && echo "   relay active" || echo "   !! relay NOT active"
  systemctl is-enabled "$UNIT" 2>/dev/null && echo "   relay enabled at boot" || echo "   !! relay NOT enabled at boot"
fi
sudo systemctl enable coturn 2>&1 | tail -1
systemctl is-enabled coturn 2>/dev/null && echo "   coturn enabled at boot" || echo "   !! coturn NOT enabled at boot"

echo ">> verify"
curl -sS --max-time 5 http://127.0.0.1:8787/health | head -c 300
echo ""
echo "--- coturn listeners (3478 + relay range) ---"
sudo ss -ulpn | grep -E '3478|relay|turn' | head -4
echo "--- done ---"
