#!/usr/bin/env bash
# Writes a game server's opx_tvbrowser/server/config.lua from THAT role's own
# tv-url.secret, so the television's browser cinemas are put up with that role's
# browser and /browser is admin-only. This is the file the repo carries only as a
# placeholder (url = nil), because the link carries the viewer password: the
# written file is root-only (600) and never ships to clients as a file.
# Backs up the config it replaces. Prints no secret.
#
#   role         link                           game server
#   staging      /opt/open77-tvbrowser          /opt/open77-xbuniverse-staging
#   production   /opt/open77-tvbrowser-xb       /opt/open77-xbuniverse
#   f2077        /opt/open77-tvbrowser-f2077    /opt/open77-f2077
#
# USAGE  ./install-tv-config.sh [staging|production|f2077]       (default: staging)
set -euo pipefail

ROLE="${1:-staging}"
case "$ROLE" in
  staging)    D=/opt/open77-tvbrowser;        SRV=/opt/open77-xbuniverse-staging ;;
  production) D=/opt/open77-tvbrowser-xb;     SRV=/opt/open77-xbuniverse ;;
  f2077)      D=/opt/open77-tvbrowser-f2077;  SRV=/opt/open77-f2077 ;;
  *) echo "usage: $(basename "$0") [staging|production|f2077]"; exit 2 ;;
esac

SEC="$D/tv-url.secret"
CFG="$SRV/resources/opx_tvbrowser/server/config.lua"
[ -s "$SEC" ] || { echo "no link at $SEC: run ./up.sh $ROLE first"; exit 2; }
[ -f "$CFG" ] || { echo "no opx_tvbrowser config at $CFG: nothing to write"; exit 2; }

TS=$(date -u +%Y%m%d-%H%M%S); BK="/opt/open77-backups/$(basename "$SRV")-tvbrowser-config-$TS"
mkdir -p "$BK"; cp -a "$CFG" "$BK/config.lua"

umask 077
{
  printf -- '-- opx_tvbrowser -- server/config.lua (written at deploy from %s)\n' "$SEC"
  printf -- 'OpxTvBrowserConfig = {\n'
  printf -- '    url = "%s",\n' "$(tr -d '\r\n' < "$SEC")"
  printf -- '    cinemaRecord = "cinema.150ft.browser",\n'
  printf -- '    cinema100Record = "cinema.100ft.browser",\n'
  printf -- '    adminAce = "command.media.lock",\n'
  printf -- '}\n'
} > "$CFG"
chmod 600 "$CFG"; chown root:root "$CFG"

echo "== wrote $CFG from $SEC for $ROLE (0600 root:root; previous kept at $BK/config.lua)"
echo "== the runtime reloads opx_tvbrowser by itself: grep opx_tvbrowser $SRV/server.log | tail -2"
