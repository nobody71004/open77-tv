# The shared browser a television in Open77 shows: neko's Chromium, streamed to
# the screen over WebRTC. XBUNIVERSE STAGING only.
FROM ghcr.io/m1k1o/neko/chromium:3@sha256:2471119c7b4010067f76f3421a78a8bcf35e3c223b2d1f4db084914ec915fb30

# No DRM module. This browser's picture is captured and streamed to players, so
# protected video (Netflix and the like) must not be able to play in it at all:
# the image ships Widevine, and it is removed here. Component updates are off in
# the policy below, so it is not fetched back.
RUN rm -rf /usr/lib/chromium/WidevineCdm \
 && ! find / -xdev -iname '*widevine*' 2>/dev/null | grep -q .

COPY policies.json /etc/chromium/policies/managed/policies.json
