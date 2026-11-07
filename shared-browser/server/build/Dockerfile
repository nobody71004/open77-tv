# The shared browser a television in Open77 shows: neko's Chromium, streamed to
# the screen over WebRTC. One instance per server role (`up.sh staging`, `up.sh
# production`, `up.sh f2077`, `up.sh fr-us`), all run as
# open77/tvbrowser-chromium:10 (the history is in shared-browser/README.md).
# Image 9 kept image 8's profile library (Chromium on
# --user-data-dir=/profiles/active, mounted from the host) and loopback
# DevTools socket with a forwarder, and ran Chromium in kiosk mode so a cinema
# screen shows the page and nothing else. Image 10 adds what the rooms were
# given in place on 2026-11-07 (tools/tvkit, T7 and T8), so a rebuilt room keeps
# it: the input guard first in the client page, a window manager with no keys
# or menus of its own and one desktop, and uBlock Origin Lite told to leave
# netflix.com alone. It still has: no DRM module, the managed policies, the
# scripts in the client page, and a health check at the path the server
# answers on. No port, capability or setting changes.
FROM ghcr.io/m1k1o/neko/chromium:3@sha256:2471119c7b4010067f76f3421a78a8bcf35e3c223b2d1f4db084914ec915fb30

# No DRM module baked in. The room plays protected video (Netflix and the like)
# through the host's own Widevine copy, mounted read-only at run time (up.sh),
# so Google's CDM binary is never copied into -- or redistributed with -- this
# image: the image ships Widevine, and it is removed here. Component updates are
# off in the policy below, so it is not fetched back either.
RUN rm -rf /usr/lib/chromium/WidevineCdm \
 && ! find / -xdev -iname '*widevine*' 2>/dev/null | grep -q .

COPY policies.json /etc/chromium/policies/managed/policies.json
# neko launches Chromium from this program definition; the copy replaces the
# --user-data-dir with the mounted profile library and adds the DevTools socket
# and its loopback-only forwarder.
COPY chromium.conf /etc/neko/supervisord/chromium.conf
# The window manager the kiosk window needs (the stock image has openbox but
# does not run it, so a kiosk window stayed at Chrome's default size).
COPY openbox.conf /etc/neko/supervisord/openbox.conf
# ... and with nothing of its own on the keyboard (image 10). The stock rc.xml
# binds 26 keys and opens four desktops: Super+D hid the browser and showed an
# empty desktop, Ctrl+Alt+arrows went to an empty one, Shift+Alt+arrows sent
# the browser to another, Alt+F4 closed it, Alt+Space opened openbox's menu,
# and a right or middle click on the root window opened a menu. A player
# driving the room presses those without knowing, and the television then
# shows an empty desktop, or no browser at all. This copy is the stock 3.6.1 file with the key bindings taken out
# (chainQuitKey kept), no menus on the root window and one desktop -- made by
# tools/tvkit's rc_quiet(), the change the kit made in the running rooms. The
# keys still reach Chromium; neko's own pipeline is not involved.
COPY openbox-rc.xml /etc/xdg/openbox/rc.xml
# uBlock Origin Lite does not filter netflix.com (image 10). Netflix answered
# "Something went wrong" in the rooms, and a content blocker cutting one of
# Netflix's own requests is a known cause of exactly that page; noFiltering is
# the extension's managed setting for a site it must leave alone. Whether it
# was the cause here is still to be proven by a signed-in room (2026-11-07).
# A file of its own, so the policy up.sh mounts per room stays as it was.
COPY open77-netflix.json /etc/chromium/policies/managed/open77-netflix.json
# socat is the forwarder; the stock image does not carry it.
RUN apt-get update \
 && apt-get install -y --no-install-recommends socat \
 && rm -rf /var/lib/apt/lists/*

# Four scripts in the client page, ahead of the client's own (which reads the
# login from the address bar and then takes it off, and opens its socket):
#   open77-ice.js    what the stream did, told to the television framing the page
#                    (kinds and counts, no addresses), and one retry over TCP
#   open77-paste.js  Ctrl+V on the picture pastes this PC's clipboard over there,
#                    and says what it did (a right-click is told to use Ctrl+V)
#   open77-volume.js the television's volume and mute on the picture's sound,
#                    through Web Audio (the game takes the sound before an
#                    element's volume is applied)
#   open77-fit.js    what the 16:9 picture does with a screen box that is not its
#                    shape, since the television frames the page in the box exactly
#                    and does not fit the picture to it: the picture is scaled up
#                    until it covers the box, keeping its shape, and what hangs over
#                    is cut off. Nothing is drawn beside it -- no bars, no wash, no
#                    second stream. Five fills that left something beside the picture
#                    were measured on a television on 2026-10-16 and all five were
#                    rejected as bars (README.md)
# and one more, first (image 10):
#   open77-input.js  the input guard: what the player's mouse and keyboard do on
#                    the picture, told to the television (clicks, keys held,
#                    the stream's own numbers -- frame rate, round trip, how far
#                    behind the server's screen the picture is), and the
#                    browser's own shortcuts (F1-F12, Ctrl+T/N/W/Tab/1-9/H/J/P,
#                    Alt+Space/Esc/Tab, Shift+Esc, Super) kept from the room's
#                    Chromium, which they would take away from the page shown.
#                    It hooks the client's socket and peer connection, so it
#                    loads before the client's own script and before the other
#                    four.
COPY open77-input.js /var/www/open77-input.js
COPY open77-ice.js /var/www/open77-ice.js
COPY open77-paste.js /var/www/open77-paste.js
COPY open77-volume.js /var/www/open77-volume.js
COPY open77-fit.js /var/www/open77-fit.js
# Readable by the browser and by the client's own server whatever modes the build
# context had: copied from Windows the files arrive root-only, and then neko
# answers 403 for the scripts and Chromium never reads its policies.
RUN chmod 644 /etc/chromium/policies/managed/policies.json /etc/neko/supervisord/chromium.conf \
      /etc/neko/supervisord/openbox.conf /etc/xdg/openbox/rc.xml \
      /etc/chromium/policies/managed/open77-netflix.json \
      /var/www/open77-input.js /var/www/open77-ice.js /var/www/open77-paste.js /var/www/open77-volume.js /var/www/open77-fit.js
# `?v=3` on the fit script is deliberate: the file is served with no Cache-Control at
# all, and a page that already has a copy is not guaranteed to fetch a changed one --
# which is how a fixed script can leave a player still looking at the old bars. The tag
# is bumped whenever that script's behaviour changes, and it is part of the check below
# so an image that lost it fails the build rather than shipping quietly. The
# guard's `?v=3` is its version, for the same reason.
RUN grep -q '<head>' /var/www/index.html \
 && sed -i 's#<head>#<head><script src="open77-input.js?v=3"></script><script src="open77-ice.js"></script><script src="open77-paste.js"></script><script src="open77-volume.js"></script><script src="open77-fit.js?v=3"></script>#' /var/www/index.html \
 && grep -c 'src="open77-input.js?v=3"></script><script src="open77-ice.js"></script><script src="open77-paste.js"></script><script src="open77-volume.js"></script><script src="open77-fit.js?v=3"' /var/www/index.html | grep -qx 1 \
 && grep -q 'open77-input 3' /var/www/open77-input.js

# The profile library and the room service are the two things this image is
# for, and both are easy to lose silently: without --user-data-dir=/profiles the
# sign-ins die with the container, and without the forwarder the room cannot be
# driven at all. Fail the build rather than ship either.
RUN grep -q -- '--user-data-dir=/profiles/active' /etc/neko/supervisord/chromium.conf \
 && grep -q -- '--kiosk' /etc/neko/supervisord/chromium.conf \
 && grep -q 'program:openbox' /etc/neko/supervisord/openbox.conf \
 && test -x /usr/bin/openbox \
 && grep -q -- '--remote-debugging-port=9223' /etc/neko/supervisord/chromium.conf \
 && grep -q 'program:cdp' /etc/neko/supervisord/chromium.conf \
 && grep -q '?room=__ROOM__' /etc/chromium/policies/managed/policies.json \
 && ! grep -q '"172.17.0.1"' /etc/chromium/policies/managed/policies.json \
 && command -v socat >/dev/null
# Image 10's three: a window manager with no keys and one desktop (well-formed:
# openbox reads a broken file and logs a parser error every start), and the
# Netflix setting where Chromium reads it.
RUN python3 -c "import xml.dom.minidom; xml.dom.minidom.parse('/etc/xdg/openbox/rc.xml')" \
 && grep -q 'open77 tv kit' /etc/xdg/openbox/rc.xml \
 && ! grep -q '<keybind ' /etc/xdg/openbox/rc.xml \
 && grep -q '<chainQuitKey>' /etc/xdg/openbox/rc.xml \
 && grep -q '<number>1</number>' /etc/xdg/openbox/rc.xml \
 && python3 -c "import json; p = json.load(open('/etc/chromium/policies/managed/open77-netflix.json')); assert p['3rdparty']['extensions']['ddkjiahejlhfcafbddmgiahcphecmpfh']['noFiltering'] == ['netflix.com']"

# neko's own check asks /health, which a server with a path prefix answers
# under the prefix: it reported this healthy container as unhealthy.
HEALTHCHECK --interval=10s --timeout=5s --retries=8 \
  CMD wget -q -O /dev/null "http://localhost:${NEKO_SERVER_BIND#*:}${NEKO_SERVER_PATH_PREFIX}/health" || exit 1
