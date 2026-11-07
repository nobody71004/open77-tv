# tvkit: the scripts that fixed the TV rooms in place (2026-11-06 / 07)

The four rooms on rl31 (`shared-browser/README.md`, "On the server") were
diagnosed and fixed while they ran, by the scripts in `remote/`. Each one is run
as root on the machine, in a work folder of its own; `look` changes nothing,
`fix` keeps whatever it replaces first (under `/opt/open77-backups/`), and
`putback` restores exactly what it changed. They print no password, token or
key, and no address's query. The last line of every run is `TV result: {...}`.

```
remote/tv_fix.py      the move to this machine (2026-11-04) made the containers from a commit of the old ones, so
                      they kept telling players the old address for WebRTC: plan / fix --ip NEW_IP (each container
                      kept whole, made again with its own settings and the new address, put back if not healthy)
remote/tv_room.py     READ ONLY: why the home screen's tiles did not answer a click (the browser, its tabs, the
                      room service's pages, one picture of each screen)
remote/tv_clicks.py   the deeper look (the room service, neko's control and input lines, openbox, the browser's own
                      log, Widevine) and the input guard put into every running room's client page (fix --input)
remote/tv_repair.py   the room service answered nothing (its server.mjs, open77-room-server*.mjs here, brought back
                      up), Android's stack, every Chromium window made full screen again
remote/tv_native.py   four parts, each look / fix / putback:
                        --screen   Android's tile answered 404: open77-screen.html installed where websockify serves
                                   noVNC, checked as the room's browser uses it; --try-android ROLE opens it once
                        --pointer  the second cursor: every room made again with show-pointer=0 (neko 3.1's legacy
                                   pipeline turns exactly "show-pointer=false" back into true), and the machine's own
                                   files for the rooms changed the same way
                        --keys     openbox given an rc.xml with no key bindings, no root-window menus and one desktop
                                   (rc_quiet(): what build/openbox-rc.xml in the image is made with)
                        --netflix  uBlock Origin Lite told not to filter netflix.com (open77-netflix.json)
remote/tv_sources.py  READ ONLY: the rooms' own files as the machine runs them, copied exactly for this repository
                      (never neko.env, the link or the profiles; anything that looks like a secret left out)
remote/open77-input.js     the input guard (v3), the same file as shared-browser/server/build/open77-input.js
remote/open77-screen.html  Android's page, the same file as shared-browser/android-native/screen.html
```

By hand, on the machine (Python 3, Docker; nothing else is needed):

```bash
mkdir -p /root/tvwork && cp remote/* /root/tvwork/ && cd /root/tvwork
python3 tv_native.py look /root/tvwork --pointer --keys            # read only
python3 tv_native.py fix  /root/tvwork --keys --ts "$(date -u +%Y%m%d-%H%M%S)"
python3 tv_native.py putback /root/tvwork --keys --ts "$(date -u +%Y%m%d-%H%M%S)"
```

On 2026-11-06 and 07 they were run from the operator's Windows kit (`tv_kit.py`
and the bats T1-T8, which send these files over SSH and fetch the logs back),
in this order: T2 the address (tv_fix), T3/T4 the clicks (tv_room, tv_clicks),
T5/T6 the room service and Home (tv_repair, tv_clicks --home), T7 Android's page,
the keys, Netflix and the guard v3 (tv_native, tv_clicks), T8 the cursor for real
and the sources (tv_native --pointer, tv_sources). The kit and its rehearsals
(fake Docker, real Xvfb, openbox, websockify, x11vnc, Chromium and sshd: t_native
77 checks, t_clicks 62, t_repair 67, t_room 41, t_input 66) stay with the
operator's kit, which also carries the server access this repository does not.

Image 10 (`shared-browser/server/build/`) builds in what `tv_native.py` and
`tv_clicks.py --input` changed in the running rooms, so a recreated room keeps
it; `shared-browser/server/test/image-local.sh` checks that on any Docker host.
