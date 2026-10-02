# The shared browser a television in Open77 shows: neko's Chromium, streamed to
# the screen over WebRTC. XBUNIVERSE STAGING only. Staging runs it as
# open77/tvbrowser-chromium:5 (the history is in shared-browser/README.md): no DRM
# module, the managed policies, three scripts in the client page, and a health
# check at the path the server answers on. No port, capability or setting changes.
FROM ghcr.io/m1k1o/neko/chromium:3@sha256:2471119c7b4010067f76f3421a78a8bcf35e3c223b2d1f4db084914ec915fb30

# No DRM module. This browser's picture is captured and streamed to players, so
# protected video (Netflix and the like) must not be able to play in it at all:
# the image ships Widevine, and it is removed here. Component updates are off in
# the policy below, so it is not fetched back.
RUN rm -rf /usr/lib/chromium/WidevineCdm \
 && ! find / -xdev -iname '*widevine*' 2>/dev/null | grep -q .

COPY policies.json /etc/chromium/policies/managed/policies.json

# Three scripts in the client page, ahead of the client's own (which reads the
# login from the address bar and then takes it off, and opens its socket):
#   open77-ice.js    what the stream did, told to the television framing the page
#                    (kinds and counts, no addresses), and one retry over TCP
#   open77-paste.js  Ctrl+V on the picture pastes this PC's clipboard over there,
#                    and says what it did (a right-click is told to use Ctrl+V)
#   open77-volume.js the television's volume and mute on the picture's sound
COPY open77-ice.js /var/www/open77-ice.js
COPY open77-paste.js /var/www/open77-paste.js
COPY open77-volume.js /var/www/open77-volume.js
# Readable by the browser and by the client's own server whatever modes the build
# context had: copied from Windows the files arrive root-only, and then neko
# answers 403 for the scripts and Chromium never reads its policies.
RUN chmod 644 /etc/chromium/policies/managed/policies.json \
      /var/www/open77-ice.js /var/www/open77-paste.js /var/www/open77-volume.js
RUN grep -q '<head>' /var/www/index.html \
 && sed -i 's#<head>#<head><script src="open77-ice.js"></script><script src="open77-paste.js"></script><script src="open77-volume.js"></script>#' /var/www/index.html \
 && grep -c 'src="open77-ice.js"></script><script src="open77-paste.js"></script><script src="open77-volume.js"' /var/www/index.html | grep -qx 1

# neko's own check asks /health, which a server with a path prefix answers
# under the prefix: it reported this healthy container as unhealthy.
HEALTHCHECK --interval=10s --timeout=5s --retries=8 \
  CMD wget -q -O /dev/null "http://localhost:${NEKO_SERVER_BIND#*:}${NEKO_SERVER_PATH_PREFIX}/health" || exit 1
