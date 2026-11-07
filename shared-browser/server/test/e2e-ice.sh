#!/usr/bin/env bash
# Runs test/e2e-ice.mjs twice in Playwright's own image with host networking:
# once as a player's browser would connect, once with UDP candidates dropped.
# Changes nothing on the host. Prints no secret.
set -euo pipefail
D=/opt/open77-tvbrowser; IMG=mcr.microsoft.com/playwright:v1.56.1-noble
mkdir -p "$D/test/work"
cp "$D/test/e2e-ice.mjs" "$D/test/work/e2e-ice.mjs"
for mode in all tcp; do
  docker run --rm --network host --ipc host \
    -v "$D/tv-url.secret:/secret:ro" -v "$D/test/work:/work" \
    -w /work "$IMG" bash -c '[ -d node_modules/playwright ] || { npm init -y >/dev/null && npm i --silent playwright@1.56.1 >/dev/null; }; node e2e-ice.mjs '"$mode" \
    | grep -v -i -E "pwd=|password"
done
