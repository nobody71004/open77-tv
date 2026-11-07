#!/usr/bin/env python3
"""The TV room made to feel like a television of its own (2026-11-07): four things the player saw, each a part.
tv_kit.py sends this, with tv_repair.py, tv_fix.py, tv_clicks.py (whose helpers it uses) and open77-screen.html, to a
work folder of its own on the dedicated machine:

    python3 tv_native.py look    WORKDIR [--screen] [--pointer] [--keys] [--netflix]                    READ ONLY
    python3 tv_native.py fix     WORKDIR [--screen] [--pointer] [--keys] [--netflix] [--try-android ROLE] --ts TS
    python3 tv_native.py putback WORKDIR [--screen] [--pointer] [--keys] [--netflix] --ts TS

No part named means all of them.

--screen   The Android tile answered "Error code: 404 ... File not found": the room shows Android's screen at
           http://172.17.0.1:<ANDROID_PORT><ANDROID_PAGE_PATH> (open77-room's own settings: :8001/open77-screen.html),
           websockify (open77-android-native-web) serves that port from noVNC's folder and carries its WebSocket on to
           the VNC server on Android's display -- and the page is not in that folder.  fix puts open77-screen.html there
           (noVNC's client, the whole window its picture, Alt+Home / Alt+Left left to the room's browser), any page
           already there kept first, then checks it the way the room's browser will use it: the page and every module
           it imports answer, from here and from inside a television, and VNC's greeting comes back through
           websockify.  --try-android ROLE then opens Android in that room through the room service, takes a picture
           of its screen, and puts the room back on its home screen.
--pointer  Two cursors, one late: neko draws the pointer into the video while the game draws its own at once.  The
           televisions run neko's v2 ("legacy") client, and neko 3.1 gives that client a pipeline of its own: a copy of
           NEKO_CAPTURE_VIDEO_PIPELINE in which exactly "show-pointer=false" is turned back into "show-pointer=true"
           (config/capture.go: "Hacky way to enable pointer for legacy pipeline").  So show-pointer=false (T7) still
           drew it.  GStreamer reads show-pointer=0 as false, and neko's replace does not touch it.  fix makes every
           television whose pipeline is not already show-pointer=0 again with show-pointer=0 and nothing else changed
           -- kept whole first (docker commit), stopped, renamed aside (kept, restart off), made again from that image
           with its own settings, started, and checked (running, healthy, its page answers, neko's own line says
           show-pointer=0, and the legacy copy neko makes of it says the same); one that is not right is put back by
           itself.  The machine's own files that make these containers (/opt/open77-tvbrowser*/) get the same change,
           each kept first, so a later up.sh does not bring the second cursor back.  Every Chromium window is then made
           fullscreen again (what tv_repair.py --windows does) and the click guard checked.
--keys     openbox's own keys work on the televisions: Ctrl+Alt+arrows and Shift+Alt+arrows switch to (or send the
           browser to) one of four desktops -- an empty screen --, Super+D hides the browser, Alt+Esc takes the
           keyboard away from it, Alt+Space opens openbox's menu, Alt+F4 closes the browser.  fix gives openbox (as the
           user it runs as) an rc.xml without key bindings or the desktop's menus and with one desktop, kept first if
           one was there, and asks openbox to read it again (openbox --reconfigure); checked: one desktop, none shown
           instead of the browser.
--netflix  "Something went wrong" signing in to Netflix: what the room's browser shows is read (the page, the
           television's picture), and Netflix is asked for its sign-in page from this machine (status, size, what it
           says); fix tells uBlock Origin Lite (the rooms' one extension) not to filter netflix.com (its managed
           setting noFiltering, a policy file of its own in every television), so a blocked request of Netflix's own
           cannot be what fails a sign-in.  It takes effect when the room's browser starts (--pointer starts them).

putback puts each part back from the newest copies this script kept (only what is still as it wrote it).  Passwords,
tokens and keys are never printed; an address's query is never printed.  The last line is "TV result: {...}".
"""
import base64
import hashlib
import json
import os
import posixpath
import re
import shlex
import shutil
import socket
import struct
import sys
import time
import urllib.request

MODE = sys.argv[1] if len(sys.argv) > 1 else ""
W = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else ".")
ARGS = sys.argv[3:]
sys.path.insert(0, W)
import tv_repair as R      # noqa: E402  say, mask, run, dexec, probe, the windows' fix
import tv_fix as F         # noqa: E402  docker_args: a container made again with its own settings
import tv_clicks as K      # noqa: E402  a television's picture, the room service's state and actions

K.C = os.path.join(W, "native")          # where pictures are kept (packed into native.tgz)
say, run, dexec, probe, probe_text, path = R.say, R.run, R.dexec, R.probe, R.probe_text, R.path
BACKUPS = R.BACKUPS
GATEWAY = R.GATEWAY
ROOM_PORT = R.ROOM_PORT
ROOMS = K.ROOM_CONTAINER                  # role -> container
ROLE_OF = {c: r for r, c in ROOMS.items()}
NAME_RX = re.compile(r"^open77-tvbrowser[A-Za-z0-9_.-]*$")
# the folders the televisions are made from (the room service's own list, and staging's second name): nothing else
TV_DIRS = ["/opt/open77-tvbrowser", "/opt/open77-tvbrowser-xb", "/opt/open77-tvbrowser-xbs", "/opt/open77-tvbrowser-f2077",
           "/opt/open77-tvbrowser-fr-us"]
ASIDE_RX = re.compile(r"-(tvkit|tvnative)-")
SCREEN_UNIT = "open77-android-native-web.service"
ROOM_UNIT = "open77-room.service"
PAGE_SRC = os.path.join(W, "open77-screen.html")
PAGE_MARK = b"open77-screen.html (TV kit"
PIPE_KEY = "NEKO_CAPTURE_VIDEO_PIPELINE"
UBOL = "ddkjiahejlhfcafbddmgiahcphecmpfh"
NETFLIX_POLICY = "/etc/chromium/policies/managed/open77-netflix.json"
NETFLIX_HOSTS = ["netflix.com"]
NETFLIX_URL = os.environ.get("O77_NETFLIX_URL", "https://www.netflix.com/login")   # the rehearsal's stand-in elsewhere
OPENBOX_MARK = "open77 tv kit, tv_native.py keys"
# what T7 wrote (2026-11-07): openbox takes that file, but libxml2 logs "Double hyphen within comment" every time it
# reads it -- a "--" is not allowed inside an XML comment.  Still recognised as this kit's, never written again.
OPENBOX_MARKS = (OPENBOX_MARK, "open77 tv kit (tv_native.py --keys)")
VNC_TYPES = {0: "invalid", 1: "None", 2: "VNC password", 5: "RA2", 6: "RA2ne", 16: "Tight", 18: "TLS", 19: "VeNCrypt",
             22: "XVP", 30: "Apple", 113: "MSLogonII", 129: "Unix login"}
NOVNC_CAN = {1, 2, 16, 19, 22}
WAIT_HEALTHY = int(os.environ.get("O77_WAIT_HEALTHY", "120"))
WAIT_WINDOW = int(os.environ.get("O77_WAIT_WINDOW", "60"))
WAIT_ROOM = int(os.environ.get("O77_WAIT_ROOM", "90"))
TRY_WAIT = float(os.environ.get("O77_TRY_WAIT", "10"))
MIN_FREE_GB = float(os.environ.get("O77_MIN_FREE_GB", "3"))


def arg(name, default=""):
    return ARGS[ARGS.index(name) + 1] if name in ARGS and ARGS.index(name) + 1 < len(ARGS) else default


def short_url(u):
    """An address without its query or fragment (a sign-in page's carries the player's state)."""
    m = re.match(r"^([a-z][a-z0-9+.-]*://[^/?#]*)([^?#]*)", u or "", re.I)
    return (m.group(1) + m.group(2) + (" (+query)" if "?" in (u or "") else "")) if m else (u or "")[:60]


def sha_bytes(b):
    return hashlib.sha256(b).hexdigest()


def write_readme(d, text):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "README.txt"), "w") as f:
        f.write(text.rstrip() + "\n")


def newest_backup(prefix, must_hold):
    found = R.kept_copies(prefix, must_hold)
    return os.path.join(path(BACKUPS), found[-1]) if found else ""


def docker_json(args, timeout=30):
    code, out = run(["docker"] + args, timeout=timeout)
    if code != 0:
        return None
    try:
        return json.loads(out)
    except ValueError:
        return None


def inspect(name):
    d = docker_json(["inspect", name])
    return d[0] if isinstance(d, list) and d else None


def env_of(c):
    return dict(e.split("=", 1) for e in ((c or {}).get("Config") or {}).get("Env") or [] if "=" in e)


def all_tvs(running_only=False):
    code, out = run(["docker", "ps"] + ([] if running_only else ["-a"]) + ["--format", "{{.Names}}"], timeout=30)
    return sorted(n.strip() for n in out.splitlines() if NAME_RX.match(n.strip()) and not ASIDE_RX.search(n.strip()))


def cat_in(c, p, timeout=20):
    code, out = dexec(c, ["cat", p], timeout=timeout)
    return out if code == 0 else None


def cp_into(c, local, dest, mode="644", owner=""):
    code, out = run(["docker", "cp", local, f"{c}:{dest}"], timeout=60)
    if code != 0:
        return False, out.strip()[-200:]
    dexec(c, ["chmod", mode, dest], timeout=15)
    if owner:
        dexec(c, ["chown", owner, dest], timeout=15)
    return True, ""


def cp_out(c, src, local):
    os.makedirs(os.path.dirname(local), exist_ok=True)
    code, out = run(["docker", "cp", f"{c}:{src}", local], timeout=60)
    return code == 0


# =================================================================================================================
# --screen: Android's screen page
# =================================================================================================================

def unit_argv(unit):
    """The unit's command line, from systemctl show's ExecStart ({ path=... ; argv[]=... ; ... })."""
    props = R.unit_props([unit], ["ExecStart", "ActiveState", "SubState", "MainPID", "LoadState"]).get(unit, {})
    m = re.search(r"argv\[\]=([^;]*)", props.get("ExecStart", ""))
    return (m.group(1).split() if m else []), props


def screen_where():
    """Where the room sends a television for Android, and where websockify serves it from."""
    argv, props = unit_argv(SCREEN_UNIT)
    web, listen, target = "", "", ""
    for i, a in enumerate(argv):
        if a.startswith("--web="):
            web = a.split("=", 1)[1]
        elif a == "--web" and i + 1 < len(argv):
            web = argv[i + 1]
    plain = [a for i, a in enumerate(argv) if not a.startswith("-") and not (i > 0 and argv[i - 1] in ("--web", "--cert", "--key"))
             and re.match(r"^[\w.\[\]:-]*:\d+$", a)]
    if len(plain) >= 2:
        listen, target = plain[-2], plain[-1]
    env = R.unit_props([ROOM_UNIT], ["Environment"]).get(ROOM_UNIT, {}).get("Environment", "")
    try:
        words = shlex.split(env)          # systemd quotes a value with spaces: "ANDROID_LABEL=Android (native)"
    except ValueError:
        words = env.split()
    renv = dict(kv.split("=", 1) for kv in words if "=" in kv)
    port = int(renv.get("ANDROID_PORT") or (listen.rsplit(":", 1)[1] if listen else 8001))
    page = renv.get("ANDROID_PAGE_PATH", "")
    return {"argv": argv, "unit": props, "web": web, "listen": listen, "target": target, "port": port, "page": page,
            "room_env": {k: v for k, v in renv.items() if k.startswith("ANDROID_")}}


def page_file(where):
    if not where["web"] or not where["page"]:
        return ""
    rel = posixpath.normpath("/" + where["page"].split("?")[0]).lstrip("/")
    full = posixpath.normpath(posixpath.join(where["web"], rel))
    return full if full.startswith(where["web"].rstrip("/") + "/") else ""


def rfb_hello(host, port, ws_path=None, timeout=6.0):
    """VNC's greeting at host:port -- directly, or through websockify's WebSocket at ws_path, the way noVNC asks:
    {"version", "types"} or {"error"}.  Nothing is sent but the version VNC offered back."""
    try:
        s = socket.create_connection((host, port), timeout=timeout)
    except OSError as e:
        return {"error": f"cannot connect: {e.__class__.__name__}: {e.strerror or e}"}
    s.settimeout(timeout)
    buf = bytearray()
    try:
        if ws_path:
            key = base64.b64encode(os.urandom(16)).decode()
            s.sendall((f"GET {ws_path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: binary\r\n"
                       f"Origin: http://{host}:{port}\r\nUser-Agent: open77-tvkit-native\r\n\r\n").encode())
            while b"\r\n\r\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    return {"error": "websockify closed the connection before answering"}
                buf += chunk
            head, _, rest = bytes(buf).partition(b"\r\n\r\n")
            status = head.split(b"\r\n", 1)[0].decode("latin-1")
            if " 101 " not in status + " ":
                return {"error": f"no WebSocket: {status[:80]}"}
            buf = bytearray(rest)

            def frame():
                nonlocal buf
                while True:
                    while len(buf) < 2:
                        chunk = s.recv(4096)
                        if not chunk:
                            raise EOFError("closed")
                        buf += chunk
                    n, i = buf[1] & 0x7F, 2
                    if n == 126:
                        while len(buf) < 4:
                            buf += s.recv(4096)
                        n, i = struct.unpack(">H", bytes(buf[2:4]))[0], 4
                    elif n == 127:
                        while len(buf) < 10:
                            buf += s.recv(4096)
                        n, i = struct.unpack(">Q", bytes(buf[2:10]))[0], 10
                    masked = buf[1] & 0x80
                    if masked:
                        i += 4
                    while len(buf) < i + n:
                        chunk = s.recv(65536)
                        if not chunk:
                            raise EOFError("closed")
                        buf += chunk
                    op, data = buf[0] & 0x0F, bytes(buf[i:i + n])
                    if masked:
                        mk = bytes(buf[i - 4:i])
                        data = bytes(b ^ mk[j % 4] for j, b in enumerate(data))
                    buf = buf[i + n:]
                    if op == 8:
                        raise EOFError("closed by websockify")
                    if op in (0, 1, 2):
                        return data
            got = bytearray()

            def read(n):
                nonlocal got
                while len(got) < n:
                    got += frame()
                out, got = bytes(got[:n]), got[n:]
                return out

            def send(data):
                mk = os.urandom(4)
                body = bytes(b ^ mk[j % 4] for j, b in enumerate(data))
                s.sendall(bytes([0x82, 0x80 | len(data)]) + mk + body)
        else:
            def read(n):
                nonlocal buf
                while len(buf) < n:
                    chunk = s.recv(4096)
                    if not chunk:
                        raise EOFError("closed")
                    buf += chunk
                out, buf = bytes(buf[:n]), buf[n:]
                return out

            def send(data):
                s.sendall(data)
        version = read(12)
        if not version.startswith(b"RFB "):
            return {"error": f"not VNC: {version[:12]!r}"}
        send(version if version in (b"RFB 003.008\n", b"RFB 003.007\n") else b"RFB 003.008\n")
        n = read(1)[0]
        if n == 0:
            ln = struct.unpack(">I", read(4))[0]
            return {"version": version.decode().strip(), "error": "refused: " + read(min(ln, 200)).decode("utf-8", "replace")}
        types = list(read(n))
        return {"version": version.decode().strip(), "types": types}
    except (OSError, EOFError, socket.timeout) as e:
        return {"error": f"{e.__class__.__name__}: {e}"}
    finally:
        try:
            s.close()
        except OSError:
            pass


def rfb_text(r):
    if "error" in r and "types" not in r:
        return r["error"]
    types = r.get("types", [])
    names = ", ".join(f"{t} {VNC_TYPES.get(t, '?')}" for t in types)
    usable = [t for t in types if t in NOVNC_CAN]
    return (f"{r['version']}, security offered: {names or 'none'} -- " +
            ("noVNC can answer it" + (" without a password" if 1 in types else " (it will ask for a password)")
             if usable else "NOT one noVNC can answer"))


IMPORT_RX = re.compile(r"""(?:^|[;\s}])(?:import|export)\s[^'";]*?\sfrom\s*['"]([^'"]+)['"]|(?:^|[;\s])import\s*['"]([^'"]+)['"]""", re.M)


def import_graph(port, start, limit=400):
    """Every module the page loads, fetched as the browser would: (fetched, [(path, what)] missing)."""
    seen, todo, missing = set(), [start], []
    while todo and len(seen) < limit:
        p = todo.pop()
        if p in seen:
            continue
        seen.add(p)
        r = probe("127.0.0.1", port, p, keep=2000000)
        if r.get("code") != 200:
            missing.append((p, r.get("status") or r.get("connect") or r.get("answer") or "?"))
            continue
        for m in IMPORT_RX.finditer(r.get("body") or ""):
            spec = m.group(1) or m.group(2)
            if spec.startswith("."):
                todo.append(posixpath.normpath(posixpath.join(posixpath.dirname(p), spec)))
    return seen, missing


def page_imports(text):
    return [m.group(1) or m.group(2) for m in IMPORT_RX.finditer(text or "")]


def novnc_version(web):
    code, out = run(["dpkg-query", "-W", "-f=${Version}", "novnc"], timeout=15)
    if code == 0 and out.strip():
        return out.strip() + " (Debian's package)"
    for cand in ("package.json", "core/../package.json"):
        try:
            return json.load(open(path(posixpath.join(web, cand)))).get("version", "?")
        except (OSError, ValueError):
            pass
    try:
        txt = open(path(posixpath.join(web, "app/ui.js")), errors="replace").read(200000)
        m = re.search(r"version\s*[:=]\s*['\"]([\d.]+)", txt)
        return m.group(1) if m else "?"
    except OSError:
        return "not found"


INSIDE_STATUS = ("import sys, urllib.request\n"
                 "r = urllib.request.urlopen(urllib.request.Request(sys.argv[1], headers={'User-Agent': 'open77-tvkit-native'}), timeout=8)\n"
                 "b = r.read(2 * 1024 * 1024)\n"
                 "print('HTTP %d, %d bytes%s' % (r.status, len(b), \", this kit's page\" if b'open77-screen.html (TV kit' in b else ''))\n")


def inside_fetch(c, url):
    code, out = dexec(c, ["python3", "-c", INSIDE_STATUS, url], timeout=30)
    return code, out.strip().splitlines()[-1][:160] if out.strip() else ""


def screen_look(quiet=False):
    say("===== Android's screen: the page the room shows for the Android tile")
    w = screen_where()
    u = w["unit"]
    say(f"  {SCREEN_UNIT}: {u.get('LoadState', '?')}, {u.get('ActiveState', '?')}/{u.get('SubState', '?')}, pid {u.get('MainPID', '?')}; "
        f"runs: {' '.join(w['argv'])[:200] or '(no command line)'}")
    say(f"  the room's settings: {', '.join(f'{k}={v}' for k, v in sorted(w['room_env'].items())) or '(none)'}")
    url = f"http://{GATEWAY}:{w['port']}{w['page']}" if w["page"] else ""
    say(f"  a television is sent to: {url or '(ANDROID_PAGE_PATH not set: the room builds a ws-scrcpy link instead)'}")
    res = {"web": w["web"], "page": w["page"], "port": w["port"], "url": url}
    if not w["web"]:
        say("  !! websockify's command line names no --web folder: nothing serves pages on that port")
        res["result"] = "no web folder"
        return res
    pf = page_file(w)
    web = path(w["web"])
    core = os.path.isfile(os.path.join(web, "core", "rfb.js"))
    say(f"  websockify serves {w['web']} ({'there' if os.path.isdir(web) else 'MISSING'}): noVNC {novnc_version(w['web'])}, "
        f"core/rfb.js {'there' if core else 'MISSING'}, vnc.html {'there' if os.path.isfile(os.path.join(web, 'vnc.html')) else 'missing'}")
    data = R.read_bytes(path(pf)) if pf else None
    ours = R.read_bytes(PAGE_SRC)
    if data is None:
        say(f"  the page {pf or '(outside the folder)'}: NOT THERE -- this is the 404 the Android tile shows")
        res["state"] = "missing"
    else:
        same = ours is not None and sha_bytes(data) == sha_bytes(ours)
        say(f"  the page {pf}: {len(data)} bytes, sha256 {sha_bytes(data)[:12]}"
            + (" -- this kit's own" if same else (" -- an earlier copy of this kit's" if PAGE_MARK in data else " -- not this kit's")))
        res["state"] = "ours" if same else "other"
        imports = page_imports(data.decode("utf-8", "replace"))
        say(f"    it imports: {', '.join(imports) or 'nothing'}")
    others = []     # where the Android stack's own copy of the page may have been left (its folders only, by name)
    for base in ("/opt", "/srv", "/var/www", "/usr/share/novnc", "/usr/local/share"):
        for dp, dns, fns in os.walk(path(base)):
            if dp.count(os.sep) - path(base).count(os.sep) >= 4:
                dns[:] = []
            dns[:] = [d for d in dns if d not in ("node_modules", ".git", "profiles", "open77-backups", "android-sdk", "docker",
                                                  "containerd")]
            for fn in fns:
                if fn == "open77-screen.html":
                    full = os.path.join(dp, fn)
                    if not ROOT_OK(full, pf):
                        others.append(full[len(R.ROOT):] if R.ROOT else full)
    if others:
        say(f"  other copies of open77-screen.html on this machine: {', '.join(others[:6])}")
    r = probe("127.0.0.1", w["port"], w["page"] or "/", keep=200)
    say(f"  asked here: {probe_text(r)}")
    r2 = probe(GATEWAY, w["port"], w["page"] or "/", keep=200)
    say(f"  asked at the address the televisions use: {probe_text(r2)}")
    res["http"] = r.get("code", 0)
    host, _, port = (w["target"] or "127.0.0.1:5900").rpartition(":")
    direct = rfb_hello(host or "127.0.0.1", int(port or 5900))
    say(f"  VNC on {w['target'] or '127.0.0.1:5900'} (Android's display), asked directly: {rfb_text(direct)}")
    via = rfb_hello("127.0.0.1", w["port"], "/websockify")
    say(f"  ... and through websockify's WebSocket, the way the page asks: {rfb_text(via)}")
    res["vnc"] = direct.get("types", direct.get("error"))
    res["via_websockify"] = via.get("types", via.get("error"))
    # (not the room's /android: asked while Android is down, it starts it)
    rs = probe("127.0.0.1", ROOM_PORT, "/api/status", keep=200000)
    try:
        st = json.loads(rs.get("body") or "{}")
        say(f"  the room service: Android {'up' if st.get('androidUp') else 'NOT up'} on its port {st.get('androidPort')}, "
            f"device {st.get('androidDevice')!r} ({st.get('androidLabel')})")
        res["room_android_up"] = bool(st.get("androidUp"))
    except ValueError:
        say(f"  the room service's status: {probe_text(rs)}")
    return res


def ROOT_OK(full, pf):
    """Whether `full` (a path under the fake root in a rehearsal) is the page itself."""
    return bool(pf) and os.path.normpath(full) == os.path.normpath(path(pf))


def screen_check(w, pf):
    """The page as a television's browser will use it: (ok, findings)."""
    found, ok = [], True
    r = probe("127.0.0.1", w["port"], w["page"], keep=100000)
    has_mark = PAGE_MARK.decode() in (r.get("body") or "")
    found.append(f"the page from here: {probe_text(r)}" + ("" if has_mark else " -- NOT this kit's page"))
    ok &= r.get("code") == 200 and has_mark
    r2 = probe(GATEWAY, w["port"], w["page"], keep=200)
    found.append(f"the page at the televisions' address: {probe_text(r2)}")
    ok &= r2.get("code") == 200
    starts = [posixpath.normpath(posixpath.join(posixpath.dirname(w["page"]), sp)) for sp in page_imports(r.get("body") or "")
              if sp.startswith(".")]
    fetched, missing = set(), []
    for st in starts:
        f2, m2 = import_graph(w["port"], st)
        fetched |= f2
        missing += m2
    found.append(f"the modules it loads: {len(fetched)} fetched, {len(missing)} missing"
                 + (": " + ", ".join(f"{p} ({why})" for p, why in missing[:5]) if missing else ""))
    ok &= bool(fetched) and not missing
    via = rfb_hello("127.0.0.1", w["port"], "/websockify")
    found.append(f"VNC's greeting through websockify: {rfb_text(via)}")
    ok &= bool(set(via.get("types", [])) & NOVNC_CAN)
    tvs = all_tvs(running_only=True)
    if tvs:
        code, out = inside_fetch(tvs[0], f"http://{GATEWAY}:{w['port']}{w['page']}")
        found.append(f"the page from inside {tvs[0]}: {out or ('exit %d' % code)}")
        ok &= code == 0
    return ok, found


def screen_fix(ts):
    say("===== Android's screen: the page the room shows, put where websockify serves it")
    w = screen_where()
    pf = page_file(w)
    ours = R.read_bytes(PAGE_SRC)
    if ours is None or PAGE_MARK not in ours:
        say("  !! this kit's open77-screen.html was not sent along: nothing changed")
        return {"result": "error", "error": "no page sent"}
    if not w["web"] or not pf:
        say(f"  !! no folder to put it in (websockify --web: {w['web'] or 'none'}; the room's ANDROID_PAGE_PATH: "
            f"{w['page'] or 'none'}): nothing changed")
        return {"result": "error", "error": "no web folder or page path"}
    if not os.path.isfile(path(posixpath.join(w["web"], "core", "rfb.js"))):
        say(f"  !! {w['web']} has no core/rfb.js (noVNC's client): the page could not load it: nothing changed")
        return {"result": "error", "error": "no noVNC core in the web folder"}
    data = R.read_bytes(path(pf))
    if data is not None and sha_bytes(data) == sha_bytes(ours):
        say(f"  {pf} is already this kit's page")
        changed = False
    else:
        if data is not None:
            keep = os.path.join(path(BACKUPS), f"android-screen-{ts}")
            os.makedirs(keep, exist_ok=True)
            shutil.copy2(path(pf), os.path.join(keep, "open77-screen.html"))
            write_readme(keep, f"{pf} as it was before tv_native.py --screen ({ts}); put back by tv_native.py putback --screen")
            say(f"  the page that was there kept in {keep[len(R.ROOT):] if R.ROOT else keep}/")
        else:
            keep = os.path.join(path(BACKUPS), f"android-screen-{ts}")
            write_readme(keep, f"{pf} was not there before tv_native.py --screen ({ts}); putback moves this kit's page "
                               f"aside into this folder")
        tmp = path(pf) + ".tvnative-new"
        with open(tmp, "wb") as f:
            f.write(ours)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path(pf))
        say(f"  written: {pf} ({len(ours)} bytes, sha256 {sha_bytes(ours)[:12]})")
        changed = True
    ok, found = screen_check(w, pf)
    for ln in found:
        say("  " + ln)
    res = {"result": ("fixed" if changed else "already") if ok else "written, not right", "page": pf, "changed": changed}
    role = arg("--try-android")
    if role:
        res["try"] = try_android(role)
    return res


def try_android(role):
    """Android opened in the room the way its tile does, a picture of the television, and the room put back home."""
    say("")
    say(f"===== Android in the {role} room, end to end (the tile's own request)")
    c = ROOMS.get(role)
    if not c or c not in all_tvs(running_only=True):
        say(f"  no running television for {role}: not tried")
        return {"result": "not tried"}
    st, r = K.room_state(role)
    before = st.get("roomUrl") or ""
    home = f"http://{GATEWAY}:{ROOM_PORT}/?room={role}"
    say(f"  it shows {short_url(before) or '?'}")
    if not before.startswith(home):
        say("  not on its home screen (a player may be using it): not tried -- Android is tried from its home screen only")
        return {"result": "not tried", "why": "not on its home screen"}
    a = K.probe_post("/api/action", json.dumps({"room": role, "action": "open", "url": "android"}).encode(), timeout=120)
    say(f"  \"open android\": {probe_text(a)} -> {(a.get('body') or '')[:200]}")
    time.sleep(TRY_WAIT)
    st2, _ = K.room_state(role)
    now = st2.get("roomUrl") or ""
    png, look = K.screen_stats(c)
    if png:
        K.save(f"try/{c}-android.png", png)
    say(f"  now it shows {short_url(now) or '?'}; its screen: {look} (picture kept: try/{c}-android.png)")
    shown = "open77-screen.html" in now or ":8001" in now
    b = K.probe_post("/api/action", json.dumps({"room": role, "action": "open", "url": home}).encode())
    time.sleep(3)
    st3, _ = K.room_state(role)
    png3, look3 = K.screen_stats(c)
    if png3:
        K.save(f"try/{c}-home-again.png", png3)
    back = (st3.get("roomUrl") or "").startswith(home)
    say(f"  put back on its home screen: {probe_text(b)}; it shows {short_url(st3.get('roomUrl') or '') or '?'}; its screen: {look3}")
    return {"result": "shown" if shown else "not shown", "screen": look, "home_again": back}


def screen_putback(ts):
    say("===== Android's screen: the page put back")
    w = screen_where()
    pf = page_file(w)
    if not pf:
        say("  no page path to put back")
        return {"result": "nothing"}
    data = R.read_bytes(path(pf))
    if data is None or PAGE_MARK not in data:
        say(f"  {pf} is not this kit's page (or not there): left as it is")
        return {"result": "nothing"}
    keep = newest_backup("android-screen", "README.txt")
    old = os.path.join(keep, "open77-screen.html") if keep else ""
    aside = os.path.join(path(BACKUPS), f"android-screen-{ts}")
    os.makedirs(aside, exist_ok=True)
    shutil.move(path(pf), os.path.join(aside, "open77-screen.html.kit"))
    write_readme(aside, f"this kit's page, taken out of {pf} by tv_native.py putback --screen ({ts})")
    if old and os.path.isfile(old):
        shutil.copy2(old, path(pf))
        say(f"  {pf}: the page that was there before is back (from {keep[len(R.ROOT):] if R.ROOT else keep})")
    else:
        say(f"  {pf}: this kit's page moved aside (nothing was there before: the Android tile answers 404 again)")
    return {"result": "put back"}


# =================================================================================================================
# --pointer: one cursor
# =================================================================================================================

POINTER_OFF = "show-pointer=0"      # GStreamer's false, which neko's legacy copy leaves alone


def pipe_parts(pipe):
    sp = re.search(r"show-pointer=(\w+)", pipe or "")
    fps = re.search(r"framerate=(\d+)/1", pipe or "")
    br = re.search(r"target-bitrate=(\d+)", pipe or "")
    return (sp.group(1) if sp else None, int(fps.group(1)) if fps else None, int(br.group(1)) if br else None)


def gst_bool(v):
    """GStreamer's own reading of a boolean property's text (gst_value_deserialize_boolean); None: not given."""
    if v is None:
        return None
    return v.lower() in ("true", "yes", "t", "1")


def legacy_copy(pipe):
    """The pipeline neko 3.1 gives its v2 ("legacy") client, from the main one: config/capture.go's own replace."""
    return pipe.replace("show-pointer=false", "show-pointer=true", 1)


def draws_pointer(pipe):
    """Whether the televisions' picture (the legacy pipeline) has the pointer drawn in: ximagesrc draws it when
    show-pointer is not given, too."""
    sp, _, _ = pipe_parts(legacy_copy(pipe))
    return True if sp is None else gst_bool(sp)


def host_files():
    """The machine's own files that make the televisions (up.sh, neko.env... in TV_DIRS, two levels down at most, not
    the profiles), with how often they say show-pointer=true or show-pointer=false (both draw it for the TVs'
    client): [(path, count)]."""
    out = []
    for d in [path(x) for x in TV_DIRS]:
        if not os.path.isdir(d):
            continue
        for dp, dns, fns in os.walk(d):
            if dp.count(os.sep) - d.count(os.sep) >= 2:
                dns[:] = []
            dns[:] = [x for x in dns if x not in ("profiles", "node_modules", ".git", "data")]
            for fn in fns:
                full = os.path.join(dp, fn)
                try:
                    if os.path.islink(full) or os.path.getsize(full) > 512 * 1024:
                        continue
                    text = open(full, "rb").read()
                except OSError:
                    continue
                if b"\0" in text[:4096]:
                    continue
                n = text.count(b"show-pointer=true") + text.count(b"show-pointer=false")
                if n:
                    out.append((full, n))
    return out


def load_look():
    say("===== the machine's load (a busy machine is a late picture)")
    try:
        la = open(path("/proc/loadavg")).read().split()[:3]
    except OSError:
        la = ["?"] * 3
    say(f"  {os.cpu_count()} CPUs; load {' '.join(la)} (1, 5, 15 min)")
    code, out = run(["ps", "-eo", "pcpu,rss,comm", "--sort=-pcpu"], timeout=20)
    top = [ln.strip() for ln in out.splitlines()[1:7] if ln.strip()]
    say("  busiest: " + "; ".join(top))
    code, out = run(["docker", "stats", "--no-stream", "--format", "{{.Name}} {{.CPUPerc}} {{.MemUsage}}"], timeout=60)
    for ln in out.splitlines():
        if ln.startswith("open77-tvbrowser"):
            say("  " + ln.strip())
    code, out = run(["df", "-P", path("/var/lib/docker") if os.path.isdir(path("/var/lib/docker")) else path("/")], timeout=20)
    lines = out.strip().splitlines()
    if len(lines) >= 2:
        f = lines[-1].split()
        say(f"  disk for docker: {int(f[3]) // (1024 * 1024)} GB free of {int(f[1]) // (1024 * 1024)} GB")


def pointer_look():
    say("===== the televisions' cursor: does neko draw the pointer into the picture?")
    res = {}
    for c in all_tvs():
        ci = inspect(c)
        if not ci:
            continue
        env = env_of(ci)
        pipe = env.get(PIPE_KEY, "")
        sp, fps, br = pipe_parts(pipe)
        st = (ci.get("State") or {})
        health = ((st.get("Health") or {}).get("Status")) or "no check"
        state = "running" if st.get("Running") else st.get("Status", "?")
        if not pipe:
            say(f"  {c} ({state}): no {PIPE_KEY}: neko's own pipeline (its show_pointer setting decides)")
            res[c] = "no pipeline setting"
            continue
        drawn = draws_pointer(pipe)
        flipped = sp == "false"
        say(f"  {c} ({state}, {health}): its setting says show-pointer={sp}" +
            (" -- which neko's legacy copy (the TVs' client's picture) turns back into true" if flipped else "") +
            " -- " + ("the pointer IS drawn into the TVs' video: a second, late cursor" if drawn else "no pointer in the TVs' video")
            + f"; {fps} fps, {br / 1e6 if br else 0:.2f} Mbit/s")
        code, logs = run(["docker", "logs", "--tail", "3000", c], timeout=60)
        lines = [ln for ln in logs.splitlines() if "pipeline" in ln and "show-pointer=" in ln]
        legacy = [ln for ln in lines if "id=legacy" in ln]
        main = [ln for ln in lines if "id=legacy" not in ln]
        for name, sel in (("the legacy pipeline (what the TVs' client gets), the last made", legacy),
                          ("the main pipeline, last checked", main)):
            if sel:
                m = re.search(r"show-pointer=(\w+)", sel[-1])
                when = re.match(r"^(\S+)", sel[-1])
                say(f"    neko's own line, {name}: show-pointer={m.group(1) if m else '?'}"
                    + (f" ({when.group(1)[:19]})" if when and when.group(1)[:2] == "20" else ""))
        res[c] = "pointer in video" if drawn else "no pointer"
    files = host_files()
    for full, n in files:
        say(f"  {full[len(R.ROOT):] if R.ROOT else full}: says show-pointer=true or =false {n}x (a container made from it "
            f"again would draw the pointer again)")
    if not files:
        say(f"  the machine's own files for the televisions ({', '.join(TV_DIRS)}): none says show-pointer=true or =false")
    return {"containers": res, "files": len(files)}


def health(ci):
    return ((ci.get("State") or {}).get("Health") or {}).get("Status")


def healthy_pointer(c, port, since):
    """Running, healthy (when it has a check), its page answers, and neko's own line says show-pointer=0 (and no
    line of its own -- the legacy copy's included, if a viewer came -- draws the pointer)."""
    deadline = time.time() + WAIT_HEALTHY
    why = "not checked"
    while time.time() < deadline:
        ci = inspect(c)
        st = (ci or {}).get("State") or {}
        if not ci or not st.get("Running") or st.get("Restarting"):
            why = f"state {st.get('Status', '?')}" + (f" (exit {st.get('ExitCode')})" if st.get("ExitCode") else "")
            if ci and st.get("Status") in ("exited", "dead"):
                return False, why
            time.sleep(2)
            continue
        hs = health(ci)
        if hs == "unhealthy":
            return False, "docker says unhealthy"
        ok, what = F.page_answers(port)
        code, logs = run(["docker", "logs", "--since", since, c], timeout=30)
        lines = [ln for ln in logs.splitlines() if "pipeline" in ln and "show-pointer=" in ln]
        bad = [ln for ln in logs.splitlines() if "pipeline" in ln.lower() and re.search(r"\b(ERR|FTL|ERROR|FATAL|PANIC)\b|syntax check[^\n]*failed", ln)]
        if bad:
            return False, "neko: " + R.mask(bad[-1].strip())[-200:]
        drawn = [ln for ln in lines if re.search(r"show-pointer=(true|yes|t|1)\b", ln, re.I)]
        if drawn:
            return False, "neko's own line still draws the pointer: " + re.search(r"show-pointer=\w+", drawn[-1]).group(0)
        says = POINTER_OFF if any(POINTER_OFF in ln for ln in lines) else ""
        if ok and says and hs in (None, "healthy"):
            return True, f"running{', healthy' if hs else ''}, {what} from its page, neko says {says}"
        why = (f"its page: {what}" if not ok else "") + ("" if says else " neko has not said its pipeline yet") + \
              (f" health {hs}" if hs not in (None, "healthy") else "")
        time.sleep(3)
    return False, why.strip()


def disk_free_gb():
    target = path("/var/lib/docker") if os.path.isdir(path("/var/lib/docker")) else path("/")
    code, out = run(["df", "-P", target], timeout=20)
    try:
        return int(out.strip().splitlines()[-1].split()[3]) / (1024 * 1024)
    except (ValueError, IndexError):
        return -1


def pointer_fix(ts):
    say("===== one cursor: every television whose picture has the pointer drawn in made again without it")
    todo = []
    for c in all_tvs():
        ci = inspect(c)
        pipe = env_of(ci).get(PIPE_KEY, "")
        if pipe and draws_pointer(pipe):
            todo.append(c)
        else:
            say(f"  {c}: " + (f"already {POINTER_OFF} (neko's legacy copy keeps it)" if pipe else "no pipeline setting to change"))
    rec_dir = os.path.join(path(BACKUPS), f"tvnative-{ts}")
    rec = {"ts": ts, "containers": [], "files": []}
    rc = {}
    if todo:
        free = disk_free_gb()
        if 0 <= free < MIN_FREE_GB:
            say(f"  !! only {free:.1f} GB free for docker (a kept image needs room; {MIN_FREE_GB} wanted): nothing changed")
            return {"result": "error", "error": "disk", "containers": {}}
        os.makedirs(rec_dir, mode=0o700, exist_ok=True)
        write_readme(rec_dir, f"tv_native.py --pointer ({ts}): the televisions made again with {POINTER_OFF}; record.json "
                              f"says which, where the old ones are (renamed aside, restart off) and the machine's own files "
                              f"kept here (files/); putback: tv_native.py putback --pointer")
    for c in todo:
        rc[c] = recreate(c, ts, rec, rec_dir)
    files_done = []
    for full, n in host_files():
        rel = full[len(path("")):] if R.ROOT else full
        os.makedirs(os.path.join(rec_dir, "files"), exist_ok=True)
        keep = os.path.join(rec_dir, "files", rel.strip("/").replace("/", "__"))
        shutil.copy2(full, keep)
        text = open(full, "rb").read()
        new = text.replace(b"show-pointer=true", POINTER_OFF.encode()).replace(b"show-pointer=false", POINTER_OFF.encode())
        tmp = full + ".tvnative-new"
        with open(tmp, "wb") as f:
            f.write(new)
        shutil.copymode(full, tmp)
        os.replace(tmp, full)
        rec["files"].append({"path": rel, "kept": os.path.basename(keep), "sha_written": sha_bytes(new)})
        files_done.append(rel)
        say(f"  {rel}: show-pointer=true/false -> {POINTER_OFF} ({n}x; kept first)")
    if todo or files_done:
        with open(os.path.join(rec_dir, "record.json"), "w") as f:
            json.dump(rec, f, indent=1)
        with open(os.path.join(path(BACKUPS), "tvnative-last.json"), "w") as f:
            json.dump({"ts": ts, "dir": rec_dir[len(R.ROOT):] if R.ROOT else rec_dir}, f)
    made = [c for c, v in rc.items() if v == "made again"]
    after = {}
    if made:
        after = after_recreate(made)
    ok = all(v in ("made again",) for v in rc.values())
    return {"result": ("fixed" if todo else "nothing") if ok else "partly", "containers": rc, "files": files_done, "after": after}


def recreate(c, ts, rec, rec_dir):
    ci = inspect(c)
    with open(os.path.join(rec_dir, f"{c}.inspect.json"), "w") as f:
        json.dump(ci, f, indent=1)
    was_running = bool((ci.get("State") or {}).get("Running"))
    policy = ((ci.get("HostConfig") or {}).get("RestartPolicy") or {}).get("Name") or "no"
    keep_img = f"tvnative-keep/{c}:{ts}"
    old_name = f"{c}-tvnative-old-{ts}"
    say(f"  ----- {c}")
    t0 = time.time()
    code, out = run(["docker", "commit", c, keep_img], timeout=900)
    if code != 0:
        say(f"    !! could not keep it whole (docker commit): {out.strip()[-200:]} -- NOTHING changed for {c}")
        return "not changed"
    say(f"    kept whole as the image {keep_img} ({time.time() - t0:.0f} s)")
    icfg = F.image_cfg(keep_img)
    pipe = env_of(ci)[PIPE_KEY]
    sp = re.search(r"show-pointer=\w+", pipe)
    new_pipe = pipe.replace(sp.group(0), POINTER_OFF, 1) if sp else pipe.replace("ximagesrc ", f"ximagesrc {POINTER_OFF} ", 1)
    say(f"    its pipeline: {sp.group(0) if sp else '(show-pointer not given)'} -> {POINTER_OFF}; the legacy copy neko "
        f"makes of it then says {pipe_parts(legacy_copy(new_pipe))[0] and 'show-pointer=' + pipe_parts(legacy_copy(new_pipe))[0]}")
    args = F.docker_args(ci, icfg, keep_img, [f"{PIPE_KEY}={new_pipe}"])
    since = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 2))
    run(["docker", "stop", "-t", "20", c], timeout=120)
    code, out = run(["docker", "rename", c, old_name], timeout=30)
    if code != 0:
        say(f"    !! could not rename it aside: {out.strip()[-200:]} -- starting it again as it was")
        if was_running:
            run(["docker", "start", c], timeout=60)
        return "not changed"
    run(["docker", "update", "--restart=no", old_name], timeout=30)
    say(f"    stopped and renamed aside: {old_name} (kept, restart off)")
    entry = {"name": c, "old": old_name, "image": keep_img, "restart": policy, "was_running": was_running}
    code, out = run(["docker", "create"] + args, timeout=120)
    ok, why = False, f"docker create: {out.strip()[-200:]}"
    if code == 0 and not was_running:
        e2 = env_of(inspect(c))
        ok = not draws_pointer(e2.get(PIPE_KEY, ""))
        why = "made, left stopped as it was" if ok else f"its settings do not say {POINTER_OFF}"
    elif code == 0:
        code2, out2 = run(["docker", "start", c], timeout=90)
        ok, why = healthy_pointer(c, F.page_port(ci), since) if code2 == 0 else (False, f"docker start: {out2.strip()[-200:]}")
    if ok:
        say(f"    made again with {POINTER_OFF}" + (" and started" if was_running else "") + f": {why}")
        rec["containers"].append(entry)
        return "made again"
    say(f"    !! the new one is not right ({why}): the old one put back")
    failed = f"{c}-tvnative-failed-{ts}"
    run(["docker", "stop", "-t", "10", c], timeout=60)
    if inspect(c) is not None:
        run(["docker", "rename", c, failed], timeout=30)
        run(["docker", "update", "--restart=no", failed], timeout=30)
    run(["docker", "rename", old_name, c], timeout=30)
    run(["docker", "update", f"--restart={policy}", c], timeout=30)
    if was_running:
        run(["docker", "start", c], timeout=90)
    say(f"    put back as it was ({'running' if was_running else 'stopped'}); the new one kept, stopped, as {failed}")
    return "put back: " + why[:120]


def after_recreate(made):
    """What a new container needs that is not in its settings: its window full screen, its click guard checked, and
    the room service reaching it at its new address."""
    out = {}
    deadline = time.time() + WAIT_WINDOW
    waiting = set(made)
    while waiting and time.time() < deadline:
        for c in sorted(waiting):
            r = R.win_list(c)
            if "error" not in r and any("chrom" in w["class"].lower() for w in r.get("windows", [])):
                waiting.discard(c)
        if waiting:
            time.sleep(2)
    if waiting:
        say(f"  no Chromium window yet in {', '.join(sorted(waiting))} after {WAIT_WINDOW} s")
    say("")
    out["windows"] = R.win_fix()
    say("")
    say("===== the made-again televisions: their click guard, and the room service reaching them")
    for c in made:
        page = cat_in(c, "/var/www/index.html") or ""
        m = re.search(r'<script src="open77-input\.js(?:\?v=([0-9a-z]+))?"></script>', page)
        say(f"  {c}: the click guard " + (f"is in its page (v={m.group(1)})" if m else "is NOT in its page (the kit's clicks-fix "
                                                                                      "puts it in)"))
        out.setdefault("guard", {})[c] = bool(m)
    deadline = time.time() + WAIT_ROOM
    roles = [ROLE_OF[c] for c in made if c in ROLE_OF]
    seen = {}
    while roles and time.time() < deadline:
        for role in list(roles):
            st, _ = K.room_state(role)
            if st.get("roomUrl"):
                seen[role] = st["roomUrl"]
                roles.remove(role)
        if roles:
            time.sleep(5)
    for role, u in seen.items():
        say(f"  the room service reaches the {role} room again: it shows {short_url(u)}")
    for role in roles:
        say(f"  !! the room service does not read the {role} room's browser yet after {WAIT_ROOM} s (it keeps a "
            f"container's address a minute)")
    out["room"] = {"reached": sorted(seen), "not_yet": roles}
    return out


def pointer_putback(ts):
    say("===== one cursor: the televisions put back as they were before tv_native.py --pointer")
    lastf = os.path.join(path(BACKUPS), "tvnative-last.json")
    if not os.path.isfile(lastf):
        say("  nothing to put back: no --pointer fix recorded here")
        return {"result": "nothing"}
    last = json.load(open(lastf))
    rec_dir = path(last["dir"])
    rec = json.load(open(os.path.join(rec_dir, "record.json")))
    ok = True
    for e in rec.get("containers", []):
        c = e["name"]
        say(f"  ----- {c}: back to {e['old']}")
        if inspect(e["old"]) is None:
            say(f"    !! {e['old']} is not here any more: {c} stays as it is")
            ok = False
            continue
        undone = f"{c}-tvnative-undone-{ts}"
        run(["docker", "stop", "-t", "20", c], timeout=120)
        if inspect(c) is not None:
            run(["docker", "rename", c, undone], timeout=30)
            run(["docker", "update", "--restart=no", undone], timeout=30)
        code, out = run(["docker", "rename", e["old"], c], timeout=30)
        if code != 0:
            say(f"    !! could not rename {e['old']} back: {out.strip()[-200:]}")
            ok = False
            continue
        run(["docker", "update", f"--restart={e['restart']}", c], timeout=30)
        if e.get("was_running", True):
            code, out = run(["docker", "start", c], timeout=90)
            say(f"    {c} is the old container again, " + ("started" if code == 0 else f"!! not started: {out.strip()[-200:]}"))
            ok &= code == 0
        say(f"    the one with one cursor kept, stopped, as {undone}")
    for fe in rec.get("files", []):
        full = path(fe["path"])
        keep = os.path.join(rec_dir, "files", fe["kept"])
        cur = R.read_bytes(full)
        if cur is not None and sha_bytes(cur) != fe["sha_written"]:
            say(f"  {fe['path']}: changed since the fix -- left as it is")
            continue
        shutil.copy2(keep, full)
        say(f"  {fe['path']}: put back")
    os.replace(lastf, os.path.join(rec_dir, f"putback-{ts}.json"))
    if rec.get("containers"):
        after_recreate([e["name"] for e in rec["containers"]])
    return {"result": "put back" if ok else "partly"}


# =================================================================================================================
# --keys: openbox's own keys
# =================================================================================================================

ROOTPROPS_SRC = r'''
import ctypes, ctypes.util, json, sys
mode, display = sys.argv[1], sys.argv[2]
x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
d = x.XOpenDisplay(display.encode())
if not d:
    print(json.dumps({"error": "cannot open display " + display})); sys.exit(2)
x.XDefaultRootWindow.restype = ctypes.c_ulong
x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
root = x.XDefaultRootWindow(d)
x.XInternAtom.restype = ctypes.c_ulong
x.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
x.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long, ctypes.c_int,
    ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_void_p)]
def card(name):
    t, f, n, a, p = ctypes.c_ulong(), ctypes.c_int(), ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_void_p()
    if x.XGetWindowProperty(d, root, x.XInternAtom(d, name.encode(), 0), 0, 1, 0, 6, ctypes.byref(t), ctypes.byref(f),
                            ctypes.byref(n), ctypes.byref(a), ctypes.byref(p)) != 0 or not p.value or n.value < 1:
        return None
    return ctypes.cast(p, ctypes.POINTER(ctypes.c_ulong))[0]
def ask(name, value):
    # what a pager does (EWMH): a message to the root window the window manager acts on
    class E(ctypes.Structure):
        _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                    ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong), ("format", ctypes.c_int), ("l", ctypes.c_long * 5)]
    class U(ctypes.Union):
        _fields_ = [("xclient", E), ("pad", ctypes.c_long * 24)]
    ev = U()
    ev.xclient.type, ev.xclient.window, ev.xclient.format = 33, root, 32
    ev.xclient.message_type = x.XInternAtom(d, name.encode(), 0)
    ev.xclient.l[0] = value
    x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
    x.XSendEvent(d, root, 0, (1 << 20) | (1 << 19), ctypes.byref(ev))
    x.XFlush.argtypes = [ctypes.c_void_p]
    x.XFlush(d)
    import time; time.sleep(0.4)
if mode == "show-windows":
    ask("_NET_SHOWING_DESKTOP", 0)
elif mode.startswith("desktops:"):
    # openbox takes the number of desktops from rc.xml only when it starts on a screen that has none set yet: a
    # running one is asked, the way a pager asks (_NET_NUMBER_OF_DESKTOPS); its windows on a desktop that goes are
    # moved to the last one left
    ask("_NET_NUMBER_OF_DESKTOPS", int(mode.split(":", 1)[1]))
    ask("_NET_CURRENT_DESKTOP", 0)
print(json.dumps({"desktops": card("_NET_NUMBER_OF_DESKTOPS"), "current": card("_NET_CURRENT_DESKTOP"),
                  "showing": card("_NET_SHOWING_DESKTOP")}))
'''

RISKY = {"C-A-Left": "empty desktop", "C-A-Right": "empty desktop", "C-A-Up": "empty desktop", "C-A-Down": "empty desktop",
         "S-A-Left": "browser sent away", "S-A-Right": "browser sent away", "S-A-Up": "browser sent away",
         "S-A-Down": "browser sent away", "W-d": "browser hidden", "A-F4": "browser closed", "A-Escape": "focus taken away",
         "A-space": "openbox's menu", "W-F1": "desktop", "W-F2": "desktop", "W-F3": "desktop", "W-F4": "desktop"}


def root_props(c, mode="read"):
    code, out = dexec(c, ["python3", "-c", ROOTPROPS_SRC, mode, R.display_of(c)], timeout=30)
    try:
        return json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"error": out.strip()[-160:] or f"exit {code}"}


def openbox_of(c):
    """(the rc.xml openbox reads, its HOME, --config-file if any) from the running openbox's own command line and
    environment."""
    code, out = dexec(c, ["sh", "-c", "for p in $(pgrep -x openbox 2>/dev/null); do tr '\\0' ' ' < /proc/$p/cmdline; echo; "
                                      "tr '\\0' '\\n' < /proc/$p/environ | grep -E '^(HOME|XDG_CONFIG_HOME)='; break; done"],
                      timeout=20)
    lines = out.strip().splitlines()
    cmdline = lines[0] if lines else ""
    env = dict(ln.split("=", 1) for ln in lines[1:] if "=" in ln)
    m = re.search(r"--config-file[= ](\S+)", cmdline)
    conf_home = env.get("XDG_CONFIG_HOME") or (env.get("HOME", "/home/neko").rstrip("/") + "/.config")
    user_rc = conf_home.rstrip("/") + "/openbox/rc.xml"
    return {"running": bool(cmdline), "cmdline": cmdline, "config_file": m.group(1) if m else "", "user_rc": user_rc,
            "home": env.get("HOME", "/home/neko")}


def rc_facts(xml):
    xml = re.sub(r"(?s)<!--.*?-->", "", xml or "")
    kb = re.search(r"(?s)<keyboard>(.*?)</keyboard>", xml)
    keys = re.findall(r'<keybind key="([^"]+)"', kb.group(1)) if kb else []
    d = re.search(r"(?s)<desktops>.*?<number>\s*(\d+)\s*</number>", xml)
    root_ctx = re.search(r'(?s)<context name="Root">(.*?)</context>', xml)
    root_binds = re.findall(r'<mousebind button="(\w+)" action="(\w+)">', root_ctx.group(1)) if root_ctx else []
    return {"keys": keys, "desktops": int(d.group(1)) if d else None, "root_binds": root_binds}


def ours_rc(xml):
    return any(m in (xml or "") for m in OPENBOX_MARKS)


def rc_quiet(xml, ts):
    """openbox's rc.xml with no key bindings, no menus on the desktop, one desktop -- the rest as it was."""
    out = re.sub(r"(?s)(<keyboard>)(.*?)(</keyboard>)",
                 lambda m: m.group(1) + "\n    " + "".join(re.findall(r"(?s)<chainQuitKey>.*?</chainQuitKey>", m.group(2))) +
                 "\n    <!-- no key bindings: a television's keys are its page's -->\n  " + m.group(3), xml, count=1)
    out = re.sub(r'(?s)(<context name="Root">)(.*?)(</context>)', r"\1\n      <!-- no menus on the desktop -->\n    \3", out, count=1)
    if re.search(r"(?s)<desktops>.*?<number>", out):
        out = re.sub(r"(?s)(<desktops>.*?<number>)\s*\d+\s*(</number>)", r"\g<1>1\g<2>", out, count=1)
    else:
        out = re.sub(r"(</openbox_config>)", "<desktops><number>1</number></desktops>\n\\1", out, count=1)
    out = re.sub(r"<!-- open77 tv kit[^\n]*?-->\n", "", out)       # an earlier mark of this kit's: one mark only
    head = f"<!-- {OPENBOX_MARK}, {ts} -->\n"
    if out.startswith("<?xml"):
        first, _, rest = out.partition("\n")
        return first + "\n" + head + rest
    return head + out


def keys_look():
    say("===== the televisions' window manager (openbox): its own keys")
    res = {}
    for c in all_tvs(running_only=True):
        ob = openbox_of(c)
        if not ob["running"]:
            say(f"  {c}: openbox is not running")
            res[c] = "no openbox"
            continue
        rc = ob["config_file"] or ob["user_rc"]
        xml = cat_in(c, rc)
        which = rc
        if xml is None and not ob["config_file"]:
            xml, which = cat_in(c, "/etc/xdg/openbox/rc.xml"), "/etc/xdg/openbox/rc.xml"
        f = rc_facts(xml)
        risky = [k for k in f["keys"] if k in RISKY]
        props = root_props(c)
        ours = ours_rc(xml)
        say(f"  {c}: reads {which}" + (" (this kit's)" if ours else "") + f": {len(f['keys'])} key binding(s), "
            f"{f['desktops']} desktop(s), desktop menus {', '.join(b + ' ' + a for b, a in f['root_binds']) or 'none'}; "
            f"X says {props.get('desktops', '?')} desktop(s), on {props.get('current', '?')}, showing the desktop "
            f"{'YES (the browser hidden)' if props.get('showing') == 1 else 'no'}")
        if risky:
            by = {}
            for k in f["keys"]:
                if k in RISKY:
                    by.setdefault(RISKY[k], []).append(k)
            say("    keys that take the television away: " + "; ".join(f"{what}: {', '.join(ks)}" for what, ks in by.items()),
                limit=1000)
        res[c] = {"keys": len(f["keys"]), "desktops": props.get("desktops"), "ours": ours}
    return res


def keys_fix(ts):
    say("===== the televisions' window manager: no keys of its own, one desktop")
    res = {}
    for c in all_tvs(running_only=True):
        ob = openbox_of(c)
        if not ob["running"]:
            say(f"  {c}: openbox is not running: not changed")
            res[c] = "no openbox"
            continue
        if ob["config_file"]:
            say(f"  {c}: openbox runs with --config-file {ob['config_file']}: not changed (that file is someone's own)")
            res[c] = "config file of its own"
            continue
        cur = cat_in(c, ob["user_rc"])
        base = cur if cur is not None and not ours_rc(cur) else None
        if base is None:
            base = cat_in(c, "/etc/xdg/openbox/rc.xml")
        if not base or "<openbox_config" not in base:
            say(f"  {c}: no openbox rc.xml to start from: not changed")
            res[c] = "no rc.xml"
            continue
        if cur is not None and not ours_rc(cur):
            keep = os.path.join(path(BACKUPS), f"tvnative-keys-{ts}")
            os.makedirs(keep, exist_ok=True)
            with open(os.path.join(keep, f"{c}.rc.xml"), "w") as f:
                f.write(cur)
            write_readme(keep, f"openbox rc.xml files the televisions had in {ob['user_rc']} before tv_native.py --keys ({ts})")
        new = rc_quiet(base, ts)
        local = os.path.join(W, f"rc-{c}.xml")
        with open(local, "w") as f:
            f.write(new)
        dexec(c, ["mkdir", "-p", posixpath.dirname(ob["user_rc"])], timeout=15)
        ok, err = cp_into(c, local, ob["user_rc"], owner="")
        if not ok:
            say(f"  {c}: !! could not write {ob['user_rc']}: {err}")
            res[c] = "not written"
            continue
        conf = posixpath.dirname(posixpath.dirname(ob["user_rc"]))
        code, out = dexec(c, ["sh", "-c", f"o=$(stat -c %u:%g {ob['home']} 2>/dev/null) && chown $o {conf} "
                                          f"{posixpath.dirname(ob['user_rc'])} {ob['user_rc']}"], timeout=15)
        code, out = dexec(c, ["openbox", "--reconfigure"], env={"DISPLAY": R.display_of(c)}, timeout=20)
        time.sleep(1)
        props = root_props(c)
        if props.get("desktops") != 1:
            props = root_props(c, "desktops:1")
        if props.get("showing") == 1:
            props = root_props(c, "show-windows")
        f = rc_facts(cat_in(c, ob["user_rc"]))
        good = props.get("desktops") == 1 and props.get("showing") != 1 and not f["keys"]
        say(f"  {c}: {ob['user_rc']} written ({len(f['keys'])} key bindings, {f['desktops']} desktop), openbox read it again "
            f"(exit {code}): X says {props.get('desktops', '?')} desktop(s), showing the desktop "
            f"{'yes' if props.get('showing') == 1 else 'no'}" + ("" if good else " -- NOT as wanted"))
        res[c] = "quiet" if good else "written, not as wanted"
    return res


def keys_putback(ts):
    say("===== the televisions' window manager: its keys put back")
    res = {}
    keep = newest_backup("tvnative-keys", "README.txt")
    for c in all_tvs(running_only=True):
        ob = openbox_of(c)
        cur = cat_in(c, ob["user_rc"]) if ob["running"] else None
        if cur is None or not ours_rc(cur):
            res[c] = "nothing"
            continue
        old = os.path.join(keep, f"{c}.rc.xml") if keep else ""
        if old and os.path.isfile(old):
            ok, err = cp_into(c, old, ob["user_rc"])
            what = "the rc.xml it had before is back"
        else:
            code, out = dexec(c, ["mv", ob["user_rc"], ob["user_rc"] + f".tvnative-off-{ts}"], timeout=15)
            ok, what = code == 0, f"this kit's rc.xml moved aside ({ob['user_rc']}.tvnative-off-{ts}): openbox reads its own again"
        dexec(c, ["openbox", "--reconfigure"], env={"DISPLAY": R.display_of(c)}, timeout=20)
        time.sleep(1)
        now_rc = cat_in(c, ob["user_rc"]) if os.path.isfile(old or "/nonexistent") else cat_in(c, "/etc/xdg/openbox/rc.xml")
        n = rc_facts(now_rc).get("desktops") or 4
        props = root_props(c, f"desktops:{n}")
        say(f"  {c}: {what}; X says {props.get('desktops', '?')} desktop(s)")
        res[c] = "put back" if ok else "not put back"
    return {"result": "put back" if all(v in ("put back", "nothing") for v in res.values()) else "partly", "containers": res}


# =================================================================================================================
# --netflix
# =================================================================================================================

def netflix_probe():
    req = urllib.request.Request(NETFLIX_URL, headers={
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36",
        "Accept-Language": "en-US,en;q=0.9", "Accept": "text/html"})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read(3000000).decode("utf-8", "replace")
            status, final = r.status, r.geturl()
    except urllib.error.HTTPError as e:
        body = e.read(300000).decode("utf-8", "replace") if e.fp else ""
        status, final = e.code, e.geturl() if hasattr(e, "geturl") else NETFLIX_URL
    except Exception as e:  # noqa: BLE001
        return {"error": f"{e.__class__.__name__}: {e}"[:200]}
    title = re.search(r"(?is)<title[^>]*>(.*?)</title>", body)
    return {"status": status, "final": short_url(final), "ms": int((time.time() - t0) * 1000), "bytes": len(body),
            "title": re.sub(r"\s+", " ", title.group(1)).strip()[:80] if title else "",
            "recaptcha": len(re.findall(r"(?i)recaptcha", body)), "went_wrong": "something went wrong" in body.lower(),
            "proxy": len(re.findall(r"(?i)\b(proxy|unblocker|vpn)\b", body))}


def netflix_look():
    say("===== Netflix: what the rooms show, and what Netflix says to this machine")
    p = netflix_probe()
    if "error" in p:
        say(f"  Netflix's sign-in page from this machine: {p['error']}")
    else:
        say(f"  Netflix's sign-in page from this machine: HTTP {p['status']} in {p['ms']} ms, {p['bytes']} bytes, at "
            f"{p['final']}, title {p['title']!r}; reCAPTCHA named {p['recaptcha']}x; 'something went wrong' "
            f"{'IN IT' if p['went_wrong'] else 'not in it'}; proxy/VPN/unblocker named {p['proxy']}x")
    res = {"probe": p, "rooms": {}}
    for role, c in ROOMS.items():
        st, r = K.room_state(role)
        u = st.get("roomUrl") or ""
        if c not in all_tvs(running_only=True):
            continue
        pol = dexec(c, ["sh", "-c", "ls /etc/chromium/policies/managed/ 2>/dev/null"], timeout=15)[1].split()
        ver = dexec(c, ["sh", "-c", "chromium --version 2>/dev/null || /usr/lib/chromium/chromium --version 2>/dev/null"],
                    timeout=20)[1].strip().splitlines()
        ext = dexec(c, ["sh", "-c", f"ls /profiles/active/Default/Extensions/{UBOL}/ 2>/dev/null | head -3"], timeout=15)[1].split()
        png, look = K.screen_stats(c)
        if png:
            K.save(f"screens/{c}.png", png)
        say(f"  {role} ({c}): shows {short_url(u) or '?'}; its screen {look}; {ver[0][:40] if ver else 'Chromium ?'}; "
            f"uBlock Origin Lite {'version ' + ext[-1] if ext else 'not in the active profile'}; policy files: {', '.join(pol) or 'none'}"
            + ("" if NETFLIX_POLICY.rsplit("/", 1)[1] not in pol else " (Netflix unfiltered: this kit's)"))
        res["rooms"][role] = {"shows": short_url(u), "netflix_policy": NETFLIX_POLICY.rsplit("/", 1)[1] in pol}
    return res


def netflix_fix(ts):
    say("===== Netflix: uBlock Origin Lite told not to filter netflix.com in every television")
    policy = {"3rdparty": {"extensions": {UBOL: {"noFiltering": NETFLIX_HOSTS}}}}
    local = os.path.join(W, "open77-netflix.json")
    with open(local, "w") as f:
        json.dump(policy, f, indent=1)
    res = {}
    for c in all_tvs(running_only=True):
        cur = cat_in(c, NETFLIX_POLICY)
        if cur is not None:
            try:
                if json.loads(cur) == policy:
                    say(f"  {c}: already there")
                    res[c] = "already"
                    continue
            except ValueError:
                pass
        ok, err = cp_into(c, local, NETFLIX_POLICY)
        back = cat_in(c, NETFLIX_POLICY)
        try:
            good = ok and json.loads(back or "") == policy
        except ValueError:
            good = False
        say(f"  {c}: {NETFLIX_POLICY} " + ("written (read when its browser next starts)" if good else f"!! NOT written: {err}"))
        res[c] = "written" if good else "not written"
    return {"result": "fixed" if all(v in ("written", "already") for v in res.values()) else "partly", "containers": res}


def netflix_putback(ts):
    say("===== Netflix: the uBlock Origin Lite setting taken out")
    res = {}
    for c in all_tvs(running_only=True):
        if cat_in(c, NETFLIX_POLICY) is None:
            res[c] = "nothing"
            continue
        code, out = dexec(c, ["mv", NETFLIX_POLICY, f"/etc/chromium/policies/open77-netflix.json.off-{ts}"], timeout=15)
        say(f"  {c}: " + ("moved out of the managed policies" if code == 0 else f"!! not moved: {out.strip()[-160:]}"))
        res[c] = "put back" if code == 0 else "not put back"
    return {"result": "put back" if all(v in ("put back", "nothing") for v in res.values()) else "partly", "containers": res}


# =================================================================================================================

def parts():
    chosen = [p for p in ("screen", "pointer", "keys", "netflix") if f"--{p}" in ARGS]
    return chosen or ["screen", "pointer", "keys", "netflix"]


def pack():
    """native.tgz, always -- with the pictures when there are any: tv_kit.py fetches it after every run, and a run
    that took no picture (--pointer alone, T8) used to leave none, so the fetch was retried four times for nothing."""
    import tarfile
    os.makedirs(K.C, exist_ok=True)
    with tarfile.open(os.path.join(W, "native.tgz"), "w:gz") as t:
        t.add(K.C, arcname="native")


def main():
    if MODE not in ("look", "fix", "putback"):
        print(__doc__)
        return 2
    want = parts()
    result, rc = {"mode": MODE, "parts": want}, 0
    ts = arg("--ts") or time.strftime("%Y%m%d-%H%M%S", time.gmtime())

    def part(name, fn, *a):
        try:
            return fn(*a)
        except Exception as e:  # noqa: BLE001 -- said, and the other parts still run
            import traceback
            say(f"!! {name}: {e.__class__.__name__}: {e}")
            for ln in traceback.format_exc().splitlines()[-6:]:
                say("   " + ln)
            return {"result": "error", "error": f"{e.__class__.__name__}: {e}"}
    if MODE == "look":
        for name, fn in (("screen", screen_look), ("pointer", pointer_look), ("keys", keys_look), ("netflix", netflix_look)):
            if name in want:
                result[name] = part(name, fn)
                say("")
        result["load"] = part("load", load_look)
    elif MODE == "fix":
        if os.geteuid() != 0 and not R.ROOT:
            say("this needs root: nothing changed")
            return 2
        # the files first (a container made again by --pointer keeps them; its browser starts with them)
        if "screen" in want:
            result["screen"] = part("screen", screen_fix, ts)
            rc |= 0 if result["screen"].get("result") in ("fixed", "already") else 1
            say("")
        if "keys" in want:
            r = part("keys", keys_fix, ts)
            result["keys"] = r
            rc |= 0 if isinstance(r, dict) and all(v in ("quiet", "no openbox") for v in r.values()) else 1
            say("")
        if "netflix" in want:
            result["netflix"] = part("netflix", netflix_fix, ts)
            rc |= 0 if result["netflix"].get("result") == "fixed" else 1
            say("")
        if "pointer" in want:
            result["pointer"] = part("pointer", pointer_fix, ts)
            rc |= 0 if result["pointer"].get("result") in ("fixed", "nothing") else 1
            if "keys" in want:
                # a television made again runs a new openbox: it reads the kept rc.xml at its start; checked
                say("")
                say("===== the window manager after the televisions were made again")
                result["keys_after"] = part("keys", keys_look)
    else:
        for name, fn in (("pointer", pointer_putback), ("keys", keys_putback), ("netflix", netflix_putback),
                         ("screen", screen_putback)):
            if name in want:
                result[name] = part(name, fn, ts)
                say("")
        rc = 0 if all(v.get("result") in ("put back", "nothing") for k, v in result.items() if isinstance(v, dict)) else 1
    pack()
    say("")
    say("result: " + json.dumps(result, separators=(",", ":")), limit=100000)
    return rc


if __name__ == "__main__":
    sys.exit(main())
