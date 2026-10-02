#!/usr/bin/env bash
# test/e2e-page.mjs in four networks, in Playwright's image. Firewall rules are
# added only INSIDE the throwaway test container's own network namespace (on the
# existing test network tvtest, NET_ADMIN for that container only); the host's
# firewall is not touched. Prints no secret.
#   plain        host networking, as a player's PC with nothing blocked
#   no-udp-in    UDP answers dropped (a PC firewall that blocks them)
#   no-tcp-out   outbound TCP to port 59100 dropped
#   neither      both: the picture cannot come; the page says so
set -euo pipefail
D=/opt/open77-tvbrowser; IMG=mcr.microsoft.com/playwright:v1.56.1-noble
mkdir -p "$D/test/work"; cp "$D/test/e2e-page.mjs" "$D/test/work/e2e-page.mjs"
PREP='[ -d node_modules/playwright ] || { npm init -y >/dev/null && npm i --silent playwright@1.56.1 >/dev/null; }'
FW='command -v iptables >/dev/null || { apt-get -qq update >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y iptables >/dev/null 2>&1; }'
run() { # <name> <docker network args> <rules>
  echo "== $1"
  docker run --rm $2 --ipc host \
    -v "$D/tv-url.secret:/secret:ro" -v "$D/test/work:/work" -w /work "$IMG" \
    bash -c "set -e; $3 $PREP; node e2e-page.mjs ${SECONDS_PER_RUN:-50}" 2>&1 | grep -v -i -E "pwd=[^R]|password|debconf" || true
}
run plain "--network host" ""
run no-udp-in "--network tvtest --cap-add NET_ADMIN" "$FW; iptables -A INPUT -i lo -j ACCEPT; iptables -A INPUT -p udp --sport 53 -j ACCEPT; iptables -A INPUT -p udp -j DROP;"
run no-tcp-out "--network tvtest --cap-add NET_ADMIN" "$FW; iptables -A OUTPUT -p tcp --dport 59100 -j DROP;"
run neither "--network tvtest --cap-add NET_ADMIN" "$FW; iptables -A INPUT -i lo -j ACCEPT; iptables -A INPUT -p udp --sport 53 -j ACCEPT; iptables -A INPUT -p udp -j DROP; iptables -A OUTPUT -p tcp --dport 59100 -j DROP;"
