#!/usr/bin/env bash
# Pushes the open77_media updates (mirror wiring, admin lock, capture cinemas) and
# the new opx_screenmirror resource to ONE game-server instance, then restarts it
# and verifies it comes back clean. Run on the xbuniverse box as root:
#
#   bash push-media.sh open77-xbuniverse-staging      # staging first
#   bash push-media.sh open77-xbuniverse              # then production
#
# Expects /tmp/dep.tgz (the payload) already uploaded. Backs up the media resource
# and server.jsonc first; the rollback copy is printed at the end.
set -uo pipefail

INSTANCE="${1:?usage: push-media.sh <instance-dir-name>}"
TS=$(date +%Y%m%d-%H%M%S)
D="/opt/$INSTANCE"
MEDIA="$D/resources/system/open77_media"
RESDIR="$D/resources"
CFG="$D/server.jsonc"
# The screen-mirror viewer link the capture cinema shows (the channel the remote
# PC publishes to). Server-side only; the token is the channel's shared secret.
URL="https://xbuniverse.duckdns.org/screen-mirror/receiver?channel=WORK&token=628ee81bd6dbff2d82cd2a2fdb16946b"

[ -d "$MEDIA" ] || { echo "!! no $MEDIA"; exit 1; }
[ -f /tmp/dep.tgz ] || { echo "!! /tmp/dep.tgz missing"; exit 1; }

echo "=== deploy capture cinema + opx_screenmirror to $INSTANCE ==="

# 1. backup
BK="/opt/open77-backups/$INSTANCE-media-$TS"
mkdir -p "$BK"
cp -a "$MEDIA" "$BK/open77_media"
cp -a "$CFG" "$BK/server.jsonc"
echo "-- backup: $BK"

# 2. unpack payload
STAGE="/tmp/dep-$INSTANCE"
rm -rf "$STAGE"; mkdir -p "$STAGE"
tar -xzf /tmp/dep.tgz -C "$STAGE"

# 3. open77_media: copy the changed files
cp "$STAGE/open77_media/client/main.lua"    "$MEDIA/client/"
cp "$STAGE/open77_media/server/main.lua"    "$MEDIA/server/"
cp "$STAGE/open77_media/shared/records.lua" "$MEDIA/shared/"
cp "$STAGE/open77_media/web/tv.css"         "$MEDIA/web/"
cp "$STAGE/open77_media/web/tv.html"        "$MEDIA/web/"
cp "$STAGE/open77_media/web/tv.js"          "$MEDIA/web/"
echo "-- open77_media: 6 files updated"

# 4. opx_screenmirror resource + its capture link
rm -rf "$RESDIR/opx_screenmirror"
cp -r "$STAGE/opx_screenmirror" "$RESDIR/"
python3 - "$RESDIR/opx_screenmirror/server/config.lua" "$URL" <<'PY'
import sys
path, url = sys.argv[1], sys.argv[2]
s = open(path, encoding="utf-8").read()
assert "url = nil," in s, "config placeholder changed"
open(path, "w", encoding="utf-8").write(s.replace("url = nil,", 'url = "%s",' % url, 1))
print("-- capture link set in opx_screenmirror/server/config.lua")
PY
chown -R root:root "$RESDIR/opx_screenmirror"
chmod -R u=rwX,go=rX "$RESDIR/opx_screenmirror"

# 5. register opx_screenmirror in resources.load
python3 - "$CFG" <<'PY'
import sys
path = sys.argv[1]
s = open(path, encoding="utf-8").read()
if '"opx_screenmirror"' in s:
    print("-- opx_screenmirror already registered")
else:
    assert '"opx_tvbrowser",' in s, "cannot find opx_tvbrowser in resources.load"
    s = s.replace('"opx_tvbrowser",', '"opx_tvbrowser",\n      "opx_screenmirror",', 1)
    open(path, "w", encoding="utf-8").write(s)
    print("-- registered opx_screenmirror in resources.load")
PY

# 6. verify the capture record made it into records.lua on disk
grep -q 'cinema.100ft.capture' "$MEDIA/shared/records.lua" && echo "-- records.lua has cinema.100ft.capture"
grep -q 'MIRROR_MARK\|#open77-mirror' "$MEDIA/web/tv.js" && echo "-- tv.js has the mirror mark"

echo "=== restarting $INSTANCE ==="
systemctl restart "$INSTANCE"
sleep 12
echo "-- unit: $(systemctl is-active $INSTANCE)"
echo "-- last boot lines:"
grep -aE "opx_screenmirror|open77_media|capture|error|Error|ERR|WRN" "$D/server.log" 2>/dev/null | tail -12 | sed 's/^/   /'
echo
echo "=== rollback if needed:  cp -a $BK/open77_media/. $MEDIA/ && cp -a $BK/server.jsonc $CFG && systemctl restart $INSTANCE ==="
