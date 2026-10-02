#!/usr/bin/env bash
# Runs test/e2e.mjs in Playwright's own image with host networking, so the
# browser reaches the shared browser exactly as a player's would: through
# nginx on 443 and WebRTC on the public IP's 59100. Prints no secret.
set -euo pipefail
D=/opt/open77-tvbrowser; IMG=mcr.microsoft.com/playwright:v1.56.1-noble
mkdir -p "$D/test/out" "$D/test/work"
cp "$D/test/e2e.mjs" "$D/test/work/e2e.mjs"
docker image inspect "$IMG" >/dev/null 2>&1 || docker pull -q "$IMG" >/dev/null
docker run --rm --network host --ipc host \
  -v "$D/tv-url.secret:/secret:ro" -v "$D/test/work:/work" -v "$D/test/out:/out" \
  -w /work "$IMG" bash -c '[ -d node_modules/playwright ] || { npm init -y >/dev/null && npm i --silent playwright@1.56.1 >/dev/null; }; node e2e.mjs'
