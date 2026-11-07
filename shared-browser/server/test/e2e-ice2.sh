#!/usr/bin/env bash
# What a player's browser does when UDP answers cannot reach it (as on a PC whose
# firewall blocks them): test/e2e-ice.mjs in Playwright's image on the existing
# test network `tvtest`, with inbound UDP dropped INSIDE that throwaway
# container's own network namespace (NET_ADMIN on the test container only; the
# host's firewall is not touched). Prints no secret.
set -euo pipefail
D=/opt/open77-tvbrowser; IMG=mcr.microsoft.com/playwright:v1.56.1-noble
docker network inspect tvtest >/dev/null 2>&1 || { echo "no tvtest network"; exit 1; }
mkdir -p "$D/test/work"
cp "$D/test/e2e-ice.mjs" "$D/test/work/e2e-ice.mjs"
docker run --rm --network tvtest --ipc host --cap-add NET_ADMIN \
  -v "$D/tv-url.secret:/secret:ro" -v "$D/test/work:/work" \
  -w /work "$IMG" bash -c '
    set -e
    command -v iptables >/dev/null || { apt-get -qq update >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y iptables >/dev/null; }
    iptables -A INPUT -i lo -j ACCEPT
    iptables -A INPUT -p udp --sport 53 -j ACCEPT
    iptables -A INPUT -p udp -j DROP
    echo "inbound UDP dropped in this container: $(iptables -S INPUT | grep -c DROP) rule"
    [ -d node_modules/playwright ] || { npm init -y >/dev/null && npm i --silent playwright@1.56.1 >/dev/null; }
    node e2e-ice.mjs all
    node e2e-ice.mjs all frame
    echo "udp packets dropped: $(iptables -L INPUT -v -n -x | awk "/DROP/ {print \$1}")"' \
  | grep -v -i -E "pwd=|password"
