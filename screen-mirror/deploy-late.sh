#!/usr/bin/env bash
# Timed post-maintenance deploy: waits for the provider maintenance window to
# close (10:00 CEST = 03:00 CDT 2026-10-16, +5 min buffer), then makes exactly
# ONE SSH attempt; if it connects, deploys the new relay bundle and verifies
# the watchdog on /health and TURN relay allocation.
#
# Run unattended in the background; all output goes to deploy-late.log.
set -u

SM="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="$SM/deploy-late.log"
exec >>"$LOG" 2>&1

KEY="$HOME/.ssh/xbuniverse_vps"
HOSTIP="88.214.59.166"
DEST_REM="/opt/open77-screen-mirror"

echo "=== runner started $(date -Is) ==="

TARGET="$(date -d '2026-10-16 03:05:00' +%s)"
NOW="$(date +%s)"
if [ "$NOW" -lt "$TARGET" ]; then
  SLEEP=$((TARGET - NOW))
  echo "maintenance window still open; sleeping ${SLEEP}s until $(date -d "@$TARGET" -Is)"
  sleep "$SLEEP"
fi
echo "=== window over; single SSH attempt $(date -Is) ==="

SSH="ssh -i $KEY -o BatchMode=yes -o ConnectTimeout=20 -o StrictHostKeyChecking=accept-new root@$HOSTIP"
if ! $SSH 'echo SSH_OK; hostname; uptime -s; mkdir -p /tmp/sm-deploy'; then
  echo "=== SSH probe FAILED - likely still fail2ban banned."
  echo "    Fix: provider console -> fail2ban-client set sshd unbanip <this-ip>, then re-run."
  exit 2
fi
echo "=== SSH connected; deploying $(date -Is) ==="

cd "$SM" || exit 1
scp -i "$KEY" -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
    relay.mjs sender.html receiver.html token.mjs deploy-vps.sh \
    "root@$HOSTIP:/tmp/sm-deploy/" || { echo "=== scp FAILED ==="; exit 3; }

$SSH "SRC=/tmp/sm-deploy DEST=$DEST_REM bash -s" < deploy-vps.sh
DEPLOY_RC=$?
echo "=== deploy script exit=$DEPLOY_RC $(date -Is) ==="

echo "=== verify /health (expect watchdog field) ==="
sleep 2
HEALTH="$(curl -sS --max-time 15 https://xbuniverse.duckdns.org/screen-mirror/health)"
echo "$HEALTH"
case "$HEALTH" in
  *'"watchdog"'*) echo "VERDICT: watchdog PRESENT on /health" ;;
  *) echo "VERDICT: watchdog MISSING - deploy did not take effect" ;;
esac

echo "=== wait for the sender agent to rejoin, then re-check ==="
sleep 25
curl -sS --max-time 15 https://xbuniverse.duckdns.org/screen-mirror/health; echo

echo "=== forced-relay TURN ICE check ==="
if [ -d "$SM/node_modules/playwright" ]; then
  node "$SM/turncheck.mjs"
  echo "turncheck exit=$?"
else
  echo "!! playwright not installed locally; run: cd screen-mirror && npm install && node turncheck.mjs"
fi

echo "=== done $(date -Is) ==="
