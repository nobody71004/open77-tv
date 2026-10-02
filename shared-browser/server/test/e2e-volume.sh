#!/usr/bin/env bash
# test/e2e-volume.mjs in Playwright's image (host networking): the television's
# volume and mute on the shared browser's sound, read back from the stream's own
# media element. Changes nothing on the host. Prints no secret.
set -euo pipefail
D=/opt/open77-tvbrowser; IMG=mcr.microsoft.com/playwright:v1.56.1-noble
mkdir -p "$D/test/work" "$D/test/out"; cp "$D/test/e2e-volume.mjs" "$D/test/work/e2e-volume.mjs"
docker run --rm --network host --ipc host \
  -v "$D/tv-url.secret:/secret:ro" -v "$D/test/work:/work" -v "$D/test/out:/out" -w /work "$IMG" \
  bash -c '[ -d node_modules/playwright ] || { npm init -y >/dev/null && npm i --silent playwright@1.56.1 >/dev/null; }; node e2e-volume.mjs' \
  2>&1 | grep -v -i -E "pwd=|password" || true
