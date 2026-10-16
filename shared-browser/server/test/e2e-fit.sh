#!/usr/bin/env bash
# test/e2e-fit.mjs against the real client page in the real container, in
# Playwright's image on the host network -- the way a player's PC reaches the
# browser. One box per run, and the two are the two answers: the 150 ft cinema
# panel's own surface (1280x728, 1.76 -- within a third of the picture, so it is
# covered and cropped, which is the 8 px stripe the panel reported), and a 32:9
# client's viewport (5120x1440, 3.56 -- covering that would cost half the picture,
# so the picture is left whole and the gap is its own edge colours).
# Prints no secret, and exits non-zero if a box that needs filling is left black.
set -uo pipefail
D=/opt/open77-tvbrowser; IMG=mcr.microsoft.com/playwright:v1.56.1-noble
SECONDS_PER_RUN="${SECONDS_PER_RUN:-30}"
mkdir -p "$D/test/work"; cp "$D/test/e2e-fit.mjs" "$D/test/work/e2e-fit.mjs"
PREP='[ -d node_modules/playwright ] || { npm init -y >/dev/null && npm i --silent playwright@1.56.1 >/dev/null; }'
run() { # <box width> <box height>
  echo "== ${1}x${2}"
  local out code
  out=$(docker run --rm --network host --ipc host \
    -v "$D/tv-url.secret:/secret:ro" -v "$D/test/work:/work" -w /work "$IMG" \
    bash -c "set -e; $PREP; node e2e-fit.mjs $SECONDS_PER_RUN $1 $2" 2>&1) && code=0 || code=$?
  printf '%s\n' "$out" | grep -v -i -E "pwd=[^R]|password|debconf"
  return $code
}
fail=0
run 1280 728 || fail=1
run 5120 1440 || fail=1
exit $fail
