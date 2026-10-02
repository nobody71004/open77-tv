#!/usr/bin/env bash
# Asks a headless Chromium in the stock neko image and in ours whether the
# Widevine key system is there. Ours must answer NotSupportedError.
#
# EME needs a secure context, so the page is served from 127.0.0.1 inside the
# container (python3 is in the image), and an empty policy is mounted for the
# check so the answer is the CDM's, not the URL blocklist's.
set -u
T=$(mktemp -d)
cat > "$T/eme.html" <<'HTML'
<html><body>pending<script>
navigator.requestMediaKeySystemAccess("com.widevine.alpha",[{initDataTypes:["cenc"],videoCapabilities:[{contentType:"video/webm; codecs=\"vp9\""}]}])
 .then(function(){document.body.textContent="RESULT=WIDEVINE_AVAILABLE"},function(e){document.body.textContent="RESULT=NO_WIDEVINE_"+e.name});
</script></body></html>
HTML
echo "{}" > "$T/empty-policy.json"
for IMG in ghcr.io/m1k1o/neko/chromium:3 open77/tvbrowser-chromium:1; do
  out=$(timeout 90 docker run --rm --shm-size 512m -v "$T:/srv/eme:ro" \
    -v "$T/empty-policy.json:/etc/chromium/policies/managed/policies.json:ro" --entrypoint sh "$IMG" -c \
    "cd /srv/eme && (python3 -m http.server 8000 >/dev/null 2>&1 &) && sleep 1 && chromium --headless=new --no-sandbox --disable-gpu --virtual-time-budget=10000 --dump-dom http://127.0.0.1:8000/eme.html 2>/dev/null" \
    | grep -o "RESULT=[A-Za-z_]*" | head -1)
  files=$(docker run --rm --entrypoint sh "$IMG" -c 'find / -xdev -iname "*widevine*" 2>/dev/null | wc -l')
  echo "$IMG: ${out:-no result}; widevine files: $files"
done
rm -rf "$T"
