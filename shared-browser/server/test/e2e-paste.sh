#!/usr/bin/env bash
# test/e2e-paste.mjs in Playwright's image (host networking), then the shared
# browser's own clipboard as its X display holds it. Changes nothing on the host.
# Prints no secret.
set -euo pipefail
ROLE=${1:-staging}
case "$ROLE" in
  staging)    D=/opt/open77-tvbrowser;    NAME=open77-tvbrowser-xbs; RTC=59100 ;;
  production) D=/opt/open77-tvbrowser-xb; NAME=open77-tvbrowser-xb;  RTC=59101 ;;
  *) echo "usage: $0 [staging|production]"; exit 2 ;;
esac
IMG=mcr.microsoft.com/playwright:v1.56.1-noble
mkdir -p "$D/test/work" "$D/test/out"; cp "$D/test/e2e-paste.mjs" "$D/test/work/e2e-paste.mjs"
docker run --rm --network host --ipc host \
  -v "$D/tv-url.secret:/secret:ro" -v "$D/test/work:/work" -v "$D/test/out:/out" -w /work "$IMG" \
  bash -c '[ -d node_modules/playwright ] || { npm init -y >/dev/null && npm i --silent playwright@1.56.1 >/dev/null; }; node e2e-paste.mjs' \
  2>&1 | grep -v -i -E "pwd=|password" || true
echo "== the shared browser's clipboard now: $(docker exec -u neko -e DISPLAY=:99.0 "$NAME" timeout 5 xclip -o -selection clipboard 2>&1 | head -c 200)"
