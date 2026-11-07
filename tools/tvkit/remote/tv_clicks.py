#!/usr/bin/env python3
"""Why a click on the TV room's home screen does nothing -- the second, deeper look (2026-11-07) -- and the two fixes
it can make.  tv_kit.py sends this (with open77-input.js) to a work folder of its own on the dedicated machine:

    python3 tv_clicks.py look    WORKDIR                     READ ONLY
    python3 tv_clicks.py fix     WORKDIR --input [--room-if-stuck] [--ts TS]
    python3 tv_clicks.py putback WORKDIR

    python3 tv_clicks.py fix     WORKDIR [--input] [--home ROLE] [--ts TS]

look changes nothing.  It reads:
  * the room service (open77-room, the home screen's server on port 8002): its unit and state, the process tree under
    it (a child that has been waiting for minutes is what hangs a Node service), its kernel stack, its sockets, whether
    it answers -- from this machine and from inside every shared-browser container, with the time to connect and to
    the first byte --, its home pages and their scripts as the containers' browsers get them, its own files (copied,
    secrets hidden) and its logs;
  * every shared-browser container: how it reaches the host, whether neko's client page carries the click guard,
    what the client's bundle does with the mouse, where the X pointer is and which buttons X holds, one picture of
    the screen (read from the X display with Xlib through ctypes: no file written in the container), its windows,
    Chromium's command line, openbox's configuration, and neko's lines about presses, releases and who holds the
    controls (with their times);
  * Android: Waydroid's state, its logs, binder, adb.
It ends with what that means, and writes WORKDIR/clicks.tgz (what it read) and clicks/findings.json.

fix --input puts open77-input.js into every running shared-browser container's client page (/var/www, and one
<script> tag in its index.html after the image's own four): each container's index.html (and any earlier
open77-input.js) kept first in /opt/open77-backups/tvinput-TS/, then checked through neko itself (it serves the
script and the page carries it once); a container that does not check out is put back by itself.  --room-if-stuck
restarts the room service only when it does not answer on 127.0.0.1 twice in a row (its process tree said first),
and checks that it answers after.  putback takes the guard's tag out of every running shared browser's index.html
(each page kept first) and sets the script aside.  --home ROLE (2026-11-07) puts that room's browser back on its
own home screen through the room service (an "open" of its home page, what the home screen's own tiles ask for),
only when it shows something else, and checks the page it then shows and one picture of its screen.  Passwords, tokens and keys are never printed or copied: a value whose name says secret is <hidden>.
"""
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import time
import urllib.parse

MODE = sys.argv[1] if len(sys.argv) > 1 else ""
W = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else ".")
ARGS = sys.argv[3:]
C = os.path.join(W, "clicks")
ROOT = os.environ.get("O77ROOT", "")                      # "" on the machine: the rehearsal's root of fake files
ROOM_UNIT = "open77-room.service"
ROOM_DIR = "/opt/open77-room"
ROOM_PORT = int(os.environ.get("O77_ROOM_PORT", "8002"))  # the rehearsal's stand-in listens elsewhere
ANDROID_PORTS = [int(p) for p in os.environ.get("O77_ANDROID_PORTS", "8000,8001").split(",")]
GATEWAY = os.environ.get("O77_GATEWAY", "172.17.0.1")
BACKUPS = "/opt/open77-backups"
INPUT_JS = "open77-input.js"
INPUT_TAG_RX = re.compile(r'<script src="open77-input\.js(?:\?v=[0-9a-z]+)?"></script>')
FIT_TAG_RX = re.compile(r'<script src="open77-fit\.js(?:\?v=\d+)?"></script>')
LINE_MAX = 320

SECRET_NAME = re.compile(r"(?i)pass|secret|token|cred|apikey|api_key|private|cookie|pwd")
URL_SECRET = re.compile(r"(?i)\b((?:pwd|password|passwd|pass|token|access_token|api_?key|secret|auth|sig|signature|"
                        r"ticket|session|sid|key)=)[^&\s\"'#<>`]+")
JSON_SECRET = re.compile(r'(?i)("[^"]*(?:pass|secret|token|cred|apikey|api_key|private|pwd)[^"]*"\s*:\s*")[^"]*(")')
YAML_SECRET = re.compile(r"(?im)^(\s*[\w.-]*(?:pass|secret|token|cred|apikey|api_key|private)[\w.-]*\s*:\s*)(?![\"'`]).+$")
KV_SECRET = re.compile(r"(?i)(?<![\w.-])([\w.-]*(?:pass|secret|token|cred|apikey|api_key|private|pwd)[\w.-]*=)(?!=|process\.env)[^\s&\"',;`]+")
KEY_SECRET = re.compile(r"(?i)(?<![\w.-])([\w.-]*(?:_|-)?key(?:[_-][\w.-]*)?\s*=(?!=)\s*)(?!process\.env|<hidden>)[^\s&\"',;`]+")
ENV_SECRET = re.compile(r"(?im)^(\s*(?:export\s+|Environment=\"?)?[\w.-]*(?:pass|secret|token|cred|apikey|api_key|private|pwd)"
                        r"[\w.-]*\s*=\s*).+$")
JS_ASSIGN_SECRET = re.compile(r"(?i)((?:const|let|var)\s+[\w$]*(?:pass|secret|token|pwd|apikey|api_key|private|cred)[\w$]*\s*="
                              r"\s*)(['\"`])(?:(?!\2)[^\\\n]|\\.)*\2")
JS_KEY_SECRET = re.compile(r"(?i)((?<![\w$])[\w$]*(?:pass|secret|token|pwd|apikey|api_key|private|cred)[\w$]*\s*:\s*)(['\"`])"
                           r"(?:(?!\2)[^\\\n]|\\.)*\2")
JS_DEFAULT_SECRET = re.compile(r"(?i)(process\.env\.\w*(?:pass|secret|token|pwd|key|cred)\w*\s*(?:\|\||\?\?)\s*)(['\"`])"
                               r"(?:(?!\2)[^\\\n]|\\.)*\2")
AUTH_SECRET = re.compile(r"(?i)(\b(?:authorization|x-api-key|x-auth-token)\b['\"]?\s*[:=,]\s*['\"]?(?:bearer\s+|basic\s+|token\s+)?)"
                         r"[A-Za-z0-9._~+/=-]{8,}")
NEVER_COPY = re.compile(r"(?i)secret|\.key$|\.pem$|\.p12$|id_rsa|id_ed25519|credential|password|\.env$|token")
TEXT_EXT = (".html", ".htm", ".js", ".mjs", ".cjs", ".css", ".json", ".conf", ".sh", ".py", ".txt", ".yaml", ".yml",
            ".md", ".ini", ".service", ".toml", ".xml")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def mask(text):
    text = URL_SECRET.sub(r"\1<hidden>", text)
    text = JSON_SECRET.sub(r"\1<hidden>\2", text)
    text = JS_ASSIGN_SECRET.sub(r"\1\2<hidden>\2", text)
    text = JS_KEY_SECRET.sub(r"\1\2<hidden>\2", text)
    text = JS_DEFAULT_SECRET.sub(r"\1\2<hidden>\2", text)
    text = AUTH_SECRET.sub(r"\1<hidden>", text)
    text = YAML_SECRET.sub(r"\1<hidden>", text)
    text = ENV_SECRET.sub(r"\1<hidden>", text)
    text = KV_SECRET.sub(r"\1<hidden>", text)
    text = KEY_SECRET.sub(r"\1<hidden>", text)
    return text


def say(text=""):
    for line in str(text).splitlines() or [""]:
        print("TV " + mask(line)[:LINE_MAX], flush=True)


def run(cmd, timeout=20, binary=False, env=None):
    """(exit code, output): never raises; a command that is not there is exit 127."""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env)
    except FileNotFoundError:
        return 127, b"" if binary else ""
    except subprocess.TimeoutExpired:
        return 124, b"" if binary else f"(no answer in {timeout} s)"
    except OSError as e:
        return 126, b"" if binary else str(e)
    if binary:
        return p.returncode, p.stdout
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


def dexec(c, argv, timeout=20, binary=False, env=None):
    cmd = ["docker", "exec"]
    for k, v in (env or {}).items():
        cmd += ["-e", f"{k}={v}"]
    return run(cmd + [c] + list(argv), timeout=timeout, binary=binary)


def dsh(c, script, timeout=20, binary=False, env=None):
    return dexec(c, ["sh", "-c", script], timeout=timeout, binary=binary, env=env)


def path(p):
    return ROOT + p


def save(rel, data):
    dest = os.path.join(C, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if isinstance(data, str):
        data = mask(data).encode("utf-8")
    with open(dest, "wb") as f:
        f.write(data)
    return dest


def collapse(lines, keep=40):
    """Repeated lines once each, with how many times and when first and last (the journal's spam made readable)."""
    seen, order = {}, []
    for ln in lines:
        m = re.match(r"^(\S+T\S+|\w{3} \d+ [\d:]+|\[\d\d:\d\d:\d\d\])\s+(.*)$", ln)
        when, body = (m.group(1), m.group(2)) if m else ("", ln)
        body = re.sub(r"\[\d+\]", "[N]", re.sub(r"\[\d\d:\d\d:\d\d\]", "", body)).strip()
        body = re.sub(r"\s+", " ", re.sub(r"^\d{1,2}:\d{2}[AP]M\s+", "", body)).strip()
        if body not in seen:
            seen[body] = [0, when, when]
            order.append(body)
        seen[body][0] += 1
        seen[body][2] = when
    out = []
    for body in order[-keep:]:
        n, first, last = seen[body]
        out.append((f"{n}x " if n > 1 else "") + body + (f"  ({first} .. {last})" if n > 1 and first else
                                                          (f"  ({first})" if first else "")))
    return out


# -- probes: one GET, how long to connect and to the first byte (here and, as source, inside a container) --------

PROBE_SRC = r'''
import json, re, socket, sys, time
def probe(host, port, path, timeout=8.0, keep=0):
    r = {"target": "%s:%d%s" % (host, port, path)}
    t0 = time.time()
    s = socket.socket()
    s.settimeout(4)
    try:
        s.connect((host, port))
    except socket.timeout:
        r["connect"] = "timeout after 4 s"
        return r
    except OSError as e:
        r["connect"] = "%s: %s" % (e.__class__.__name__, e.strerror or e)
        return r
    r["connect_ms"] = int((time.time() - t0) * 1000)
    data = b""
    try:
        s.settimeout(timeout)
        s.sendall(("GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: open77-tvkit-clicks\r\nConnection: close\r\n\r\n"
                   % (path, host, port)).encode())
        t1 = time.time()
        first = s.recv(65536)
        if not first:
            r["answer"] = "closed with no answer"
            return r
        r["first_byte_ms"] = int((time.time() - t1) * 1000)
        data = first
        end = time.time() + timeout
        while time.time() < end and len(data) < keep + 65536:
            s.settimeout(max(0.05, end - time.time()))
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        if not data:
            r["answer"] = "connected, then no answer in %d s" % timeout
            return r
    except OSError as e:
        r["answer"] = "%s: %s" % (e.__class__.__name__, e)
        return r
    finally:
        s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    r["status"] = head.split(b"\r\n", 1)[0].decode("latin-1")[:80]
    r["bytes"] = len(body)
    m = re.search(rb"(?is)<title>(.*?)</title>", body)
    if m:
        r["title"] = m.group(1).decode("utf-8", "replace").strip()[:80]
    if keep:
        r["body"] = body[:keep].decode("utf-8", "replace")
    return r
if __name__ == "__main__":
    out, keep, args = [], 0, sys.argv[1:]
    if args and args[0].startswith("--keep="):
        keep, args = int(args[0][7:]), args[1:]
    for spec in args:
        host, port, path = spec.split(" ", 2)
        out.append(probe(host, int(port), path, keep=keep))
    print(json.dumps(out))
'''
_probe_ns = {"__name__": "probe"}
exec(PROBE_SRC, _probe_ns)
probe = _probe_ns["probe"]


def probe_text(r):
    if "connect" in r:
        return f"{r['target']}: cannot connect ({r['connect']})"
    if "answer" in r:
        return f"{r['target']}: connected in {r['connect_ms']} ms, {r['answer']}"
    return (f"{r['target']}: connected in {r['connect_ms']} ms, first byte after {r['first_byte_ms']} ms, "
            f"{r.get('status', '?')}, {r.get('bytes', 0)} bytes" + (f", title {r['title']!r}" if r.get("title") else ""))


def answered(r):
    return bool(r.get("status"))


# -- X, read with Xlib through ctypes inside a container (no tool needed there, nothing written there) -------------

XCAP_SRC = r'''
import ctypes, ctypes.util, json, struct, sys, zlib
mode, display = sys.argv[1], sys.argv[2]
x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
x.XOpenDisplay.restype = ctypes.c_void_p
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
dpy = x.XOpenDisplay(display.encode())
if not dpy:
    print(json.dumps({"error": "cannot open display " + display}))
    sys.exit(2)
x.XDefaultScreen.argtypes = [ctypes.c_void_p]
x.XRootWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
x.XRootWindow.restype = ctypes.c_ulong
x.XDisplayWidth.argtypes = [ctypes.c_void_p, ctypes.c_int]
x.XDisplayHeight.argtypes = [ctypes.c_void_p, ctypes.c_int]
scr = x.XDefaultScreen(dpy)
root = x.XRootWindow(dpy, scr)
w, h = x.XDisplayWidth(dpy, scr), x.XDisplayHeight(dpy, scr)
if mode == "info":
    r, c = ctypes.c_ulong(), ctypes.c_ulong()
    rx, ry, wx, wy, m = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_uint()
    x.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint)]
    x.XQueryPointer(dpy, root, ctypes.byref(r), ctypes.byref(c), ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(wx),
                    ctypes.byref(wy), ctypes.byref(m))
    print(json.dumps({"width": w, "height": h, "x": rx.value, "y": ry.value, "mask": m.value, "child": c.value}))
    sys.exit(0)
class XImage(ctypes.Structure):
    _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int), ("format", ctypes.c_int),
                ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int), ("bitmap_unit", ctypes.c_int),
                ("bitmap_bit_order", ctypes.c_int), ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int), ("red_mask", ctypes.c_ulong),
                ("green_mask", ctypes.c_ulong), ("blue_mask", ctypes.c_ulong)]
x.XGetImage.restype = ctypes.POINTER(XImage)
x.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                        ctypes.c_ulong, ctypes.c_int]
img = x.XGetImage(dpy, root, 0, 0, w, h, 0xFFFFFFFF, 2)
if not img:
    sys.exit(3)
im = img.contents
if im.bits_per_pixel != 32:
    sys.exit(4)
raw = ctypes.string_at(im.data, im.bytes_per_line * im.height)
rows = []
for y in range(im.height):
    row = raw[y * im.bytes_per_line: y * im.bytes_per_line + im.width * 4]
    rgb = bytearray(im.width * 3)
    if im.byte_order == 0:
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
    else:
        rgb[0::3], rgb[1::3], rgb[2::3] = row[1::4], row[2::4], row[3::4]
    rows.append(b"\x00" + bytes(rgb))
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
sys.stdout.buffer.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", im.width, im.height, 8, 2, 0, 0, 0)) +
                        chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
'''

INSIDE_FETCH = ("import sys, urllib.request\n"
                "r = urllib.request.urlopen(urllib.request.Request(sys.argv[1], headers={'User-Agent': 'open77-tvkit-clicks'}), timeout=8)\n"
                "sys.stdout.buffer.write(r.read(2 * 1024 * 1024))\n")

PREFS_SRC = ("import json, sys\n"
             "try:\n"
             "    p = json.load(open(sys.argv[1], encoding='utf-8'))\n"
             "except Exception as e:\n"
             "    print(json.dumps({'error': e.__class__.__name__})); sys.exit(0)\n"
             "b = p.get('browser', {}) if isinstance(p, dict) else {}\n"
             "print(json.dumps({'window_placement': b.get('window_placement'), 'app_window_placement_keys': sorted(b.get('app_window_placement', {}) or {})[:8]}))\n")


# -- the containers ----------------------------------------------------------------------------------------------

def containers():
    code, out = run(["docker", "ps", "-a", "--format", "{{.Names}}", "--filter", "name=open77-tvbrowser"], timeout=30)
    names = sorted({n.strip() for n in out.splitlines() if n.strip().startswith("open77-tvbrowser")})
    live = []
    for c in names:
        code, out = run(["docker", "inspect", "--format", "{{.State.Running}}", c], timeout=15)
        live.append((c, code == 0 and out.strip() == "true"))
    return live


def inspect_json(c, fmt):
    code, out = run(["docker", "inspect", "--format", "{{json " + fmt + "}}", c], timeout=15)
    try:
        return json.loads(out) if code == 0 else None
    except ValueError:
        return None


def neko_env(c):
    """Only the settings this needs, never a password: where neko serves the page."""
    env = {}
    for item in inspect_json(c, ".Config.Env") or []:
        k, _, v = str(item).partition("=")
        if k in ("NEKO_SERVER_BIND", "NEKO_SERVER_PATH_PREFIX", "NEKO_SESSION_IMPLICIT_HOSTING", "DISPLAY", "NEKO_DESKTOP_SCREEN"):
            env[k] = v
    return env


def room_of(c):
    code, out = dexec(c, ["cat", "/etc/chromium/policies/managed/policies.json"], timeout=15)
    try:
        home = json.loads(out).get("HomepageLocation", "") if code == 0 else ""
    except ValueError:
        home = ""
    q = urllib.parse.parse_qs(urllib.parse.urlsplit(home).query)
    return (q.get("room") or [""])[0], home


# -- look --------------------------------------------------------------------------------------------------------

FIND = {"room": {}, "containers": {}, "android": {}, "notes": []}


def look_room():
    say("===== the room service (open77-room: the home screen's server, port %d)" % ROOM_PORT)
    code, out = run(["systemctl", "show", ROOM_UNIT, "-p", "MainPID,ActiveState,SubState,NRestarts,ActiveEnterTimestamp,"
                     "ExecMainStartTimestamp,StandardOutput,StandardError,TasksCurrent,MemoryCurrent,FragmentPath,"
                     "EnvironmentFiles,DropInPaths"], timeout=15)
    props = dict(ln.split("=", 1) for ln in out.splitlines() if "=" in ln)
    main = int(props.get("MainPID", "0") or 0)
    FIND["room"].update({"active": props.get("ActiveState", "?"), "sub": props.get("SubState", "?"), "main_pid": main,
                         "restarts": props.get("NRestarts", "?"), "since": props.get("ActiveEnterTimestamp", "?")})
    say(f"  {ROOM_UNIT}: {props.get('ActiveState', '?')}/{props.get('SubState', '?')}, main pid {main}, restarts "
        f"{props.get('NRestarts', '?')}, since {props.get('ActiveEnterTimestamp', '?')}, tasks {props.get('TasksCurrent', '?')}, "
        f"memory {props.get('MemoryCurrent', '?')}, output to {props.get('StandardOutput', '?')}")
    code, out = run(["systemctl", "cat", ROOM_UNIT], timeout=15)
    save("room/unit.txt", out)
    for ln in out.splitlines():
        if ln.strip() and not ln.lstrip().startswith("#"):
            say("    " + ln.strip())
    for f in (props.get("EnvironmentFiles") or "").split():
        f = f.split("(")[0].strip()
        if f.startswith("/") and os.path.isfile(path(f)):
            text = open(path(f), encoding="utf-8", errors="replace").read()
            names = [ln.split("=", 1)[0].strip() for ln in text.splitlines() if "=" in ln and not ln.lstrip().startswith("#")]
            say(f"    EnvironmentFile {f} sets (names only): " + ", ".join(names)[:260])

    # The process tree under it: a child that has been waiting for minutes is what hangs a Node service.
    code, out = run(["ps", "-eo", "pid,ppid,etimes,stat,wchan:32,pcpu,rss,args"], timeout=15)
    rows, kids = {}, {}
    for ln in out.splitlines()[1:]:
        parts = ln.split(None, 7)
        if len(parts) < 8 or not parts[0].isdigit():
            continue
        rows[int(parts[0])] = parts
        kids.setdefault(int(parts[1]), []).append(int(parts[0]))
    tree = []

    def walk(pid, depth):
        tree.append((depth, pid))
        for k in sorted(kids.get(pid, [])):
            walk(k, depth + 1)
    if main in rows:
        walk(main, 0)
    say(f"  its processes ({len(tree)}): pid, seconds alive, state, waiting in, cpu, rss, command")
    long_children = []
    for depth, pid in tree:
        p = rows[pid]
        say(f"    {'  ' * depth}{p[0]} {p[2]}s {p[3]} {p[4]} {p[5]}% {p[6]}k {p[7][:200]}")
        if depth > 0 and int(p[2]) >= 20:
            long_children.append({"pid": pid, "seconds": int(p[2]), "command": mask(p[7][:160])})
    FIND["room"]["long_children"] = long_children
    for depth, pid in tree[:6]:
        stack = path(f"/proc/{pid}/stack")
        try:
            lines = [ln.split("]", 1)[-1].strip() for ln in open(stack).read().splitlines()][:6]
        except OSError:
            lines = []
        try:
            wchan = open(path(f"/proc/{pid}/wchan")).read().strip()
        except OSError:
            wchan = "?"
        if lines or wchan not in ("?", "0", ""):
            say(f"    pid {pid} waits in {wchan}: " + " < ".join(lines))

    # Its sockets.
    code, out = run(["ss", "-tanpH"], timeout=15)
    mine = [ln.split() for ln in out.splitlines() if f"pid={main}," in ln] if main else []
    states = {}
    peers = {}
    for f in mine:
        if len(f) < 5:
            continue
        states[f[0]] = states.get(f[0], 0) + 1
        if f[0] != "LISTEN":
            peer_host = f[4].rsplit(":", 1)[0]
            local_port = f[3].rsplit(":", 1)[-1]
            key = f"{f[0]} {peer_host} -> :{local_port}" if local_port == str(ROOM_PORT) else f"{f[0]} to {f[4]}"
            peers[key] = peers.get(key, 0) + 1
    say("  its sockets: " + (", ".join(f"{k} {v}" for k, v in sorted(states.items())) or "none"))
    for k, v in sorted(peers.items(), key=lambda kv: -kv[1])[:16]:
        say(f"    {v}x {k}")
    FIND["room"]["sockets"] = states
    code, out = run(["ss", "-ltnpH"], timeout=15)
    want = {str(ROOM_PORT), "5900", "8787", "8788", "8789", "18080", "18081", "18082", "18083"} | {str(p) for p in ANDROID_PORTS}
    say("  listening on the ports the TVs use:")
    for ln in out.splitlines():
        f = ln.split()
        if len(f) >= 4 and f[3].rsplit(":", 1)[-1] in want:
            say("    " + " ".join([f[3]] + f[5:])[:200])

    # Does it answer?  From here, twice, on loopback and on the bridge's address the containers use.
    say("  does it answer (from this machine):")
    probes = []
    for host in ("127.0.0.1", GATEWAY):
        r = probe(host, ROOM_PORT, "/?room=production")
        probes.append(r)
        say("    " + probe_text(r))
    FIND["room"]["answers_loopback"] = answered(probes[0])
    FIND["room"]["answers_gateway"] = answered(probes[1])
    FIND["room"]["probe_loopback"] = probe_text(probes[0])
    for port in ANDROID_PORTS:
        say("    " + probe_text(probe("127.0.0.1", port, "/")))
    return probes


def look_room_pages(rooms):
    """The home pages as the containers' browsers get them, and their scripts (read only: GETs of pages and files)."""
    if not FIND["room"].get("answers_loopback"):
        say("  its pages: not fetched (it does not answer)")
        return
    fetched = 0
    for room in sorted(set(rooms) | {"production"}):
        for page in (f"/?room={room}", f"/profiles?room={room}"):
            r = probe("127.0.0.1", ROOM_PORT, page, keep=2 * 1024 * 1024)
            say("  page " + probe_text(r))
            body = r.get("body") or ""
            if not body:
                continue
            name = re.sub(r"[^\w.-]+", "_", page.strip("/")) or "home"
            save(f"room/pages/{name}.html", body)
            tiles = re.findall(r"(?is)<(a|button)\b([^>]*)>(.*?)</\1>", body)
            for tag, attrs, inner in tiles[:24]:
                text = re.sub(r"(?s)<[^>]+>", " ", inner)
                text = re.sub(r"\s+", " ", text).strip()[:40]
                keep = " ".join(m for m in re.findall(r"""(?i)\b(?:href|onclick|data-[\w-]+|target|id|class)=(?:"[^"]*"|'[^']*')""", attrs))
                say(f"    {tag} {text!r}: {keep[:220]}")
            for src in re.findall(r"""(?i)<script[^>]+src=["']([^"']+)["']""", body)[:8]:
                if fetched >= 12 or re.match(r"(?i)^(https?:)?//", src):
                    continue
                u = urllib.parse.urljoin(page, src)
                a = probe("127.0.0.1", ROOM_PORT, u, keep=1024 * 1024)
                fetched += 1
                if a.get("body"):
                    save("room/pages/" + re.sub(r"[^\w.-]+", "_", u.strip("/")), a["body"])
                    for hit in sorted(set(re.findall(r"""(?:fetch|axios\.\w+|XMLHttpRequest|location\.href\s*=|window\.open)\s*\(?\s*[`'"]([^`'"]{1,120})""",
                                                     a["body"])))[:20]:
                        say(f"      {src} reaches: {hit}")
            for hit in sorted(set(re.findall(r"""(?:fetch|location\.href\s*=|window\.open)\s*\(?\s*[`'"]([^`'"]{1,120})""", body)))[:20]:
                say(f"      the page reaches: {hit}")


def look_room_files():
    say("  its files (%s, without node_modules):" % ROOM_DIR)
    base = path(ROOM_DIR)
    if not os.path.isdir(base):
        say("    (no such folder)")
        return
    copied, total = 0, 0
    for dp, dns, fns in os.walk(base):
        dns[:] = sorted(d for d in dns if d not in ("node_modules", ".git", "profiles", "cache"))
        rel = os.path.relpath(dp, base)
        if rel.count(os.sep) >= 3:
            dns[:] = []
        for fn in sorted(fns):
            full = os.path.join(dp, fn)
            try:
                st = os.stat(full)
            except OSError:
                continue
            relf = os.path.normpath(os.path.join(rel, fn))
            say(f"    {time.strftime('%Y-%m-%d %H:%M', time.gmtime(st.st_mtime))} {st.st_size:>9} {relf}")
            if NEVER_COPY.search(fn) or ".bak" in fn or not fn.lower().endswith(TEXT_EXT) or fn == "package-lock.json":
                continue
            if st.st_size > 512 * 1024 or total + st.st_size > 4 * 1024 * 1024:
                continue
            try:
                text = open(full, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            save(os.path.join("room", "files", relf), text)
            copied += 1
            total += st.st_size
    say(f"    copied {copied} file(s) ({total} bytes), secrets hidden")


def look_room_logs():
    code, out = run(["systemctl", "show", ROOM_UNIT, "-p", "StandardOutput,StandardError"], timeout=15)
    files = set()
    for ln in out.splitlines():
        v = ln.partition("=")[2]
        m = re.match(r"^(?:append|file|truncate):(.+)$", v)
        if m:
            files.add(m.group(1))
    for pattern in ("/var/log/open77-room.log", "/var/log/open77-room/room.log", ROOM_DIR + "/room.log", ROOM_DIR + "/server.log"):
        if os.path.isfile(path(pattern)):
            files.add(pattern)
    for f in sorted(files):
        try:
            lines = open(path(f), encoding="utf-8", errors="replace").read().splitlines()[-300:]
        except OSError as e:
            say(f"  its log {f}: unreadable ({e.strerror})")
            continue
        save("room/log-" + os.path.basename(f) + ".txt", "\n".join(lines))
        say(f"  its log {f} (last {len(lines)} lines, repeats folded):")
        for ln in collapse(lines, keep=30):
            say("    " + ln)
    code, out = run(["journalctl", "-u", ROOM_UNIT, "-n", "300", "--no-pager", "-o", "short-iso"], timeout=30)
    lines = [ln for ln in out.splitlines() if ln.strip() and not ln.startswith("-- ")]
    save("room/journal.txt", out)
    say(f"  its journal: {len(lines)} line(s)" + (", repeats folded:" if lines else ""))
    for ln in collapse(lines, keep=30):
        say("    " + ln)


NEKO_RX = re.compile(r"(?i)button (?:down|up) failed|key (?:down|up) failed|host changed|control|implicit|session (?:connected|"
                     r"disconnected|created|deleted)|peer (?:connected|disconnected)|websocket|legacy|panic|fatal")


def look_container(c, js_sha):
    say("")
    say(f"===== container {c}")
    room, home = room_of(c)
    env = neko_env(c)
    prefix = env.get("NEKO_SERVER_PATH_PREFIX", "")
    bind = env.get("NEKO_SERVER_BIND", ":8080")
    info = {"room": room, "home": home, "prefix": prefix}
    FIND["containers"][c] = info
    say(f"  room {room or '?'} (home page {home}); neko serves {prefix or '/'} on {bind}; implicit hosting "
        f"{env.get('NEKO_SESSION_IMPLICIT_HOSTING', '?')}; screen {env.get('NEKO_DESKTOP_SCREEN', '?')}")
    nets = inspect_json(c, ".NetworkSettings.Networks") or {}
    gateways = []
    for name, n in sorted(nets.items()):
        say(f"  network {name}: address {n.get('IPAddress')}, gateway {n.get('Gateway')}")
        if n.get("Gateway"):
            gateways.append(n["Gateway"])
    labels = inspect_json(c, ".Config.Labels") or {}
    if labels:
        say("  image labels: " + ", ".join(f"{k}={v}" for k, v in sorted(labels.items()) if "version" in k or "created" in k)[:240])

    # The room service, from inside: the address its home page names, and the bridge's gateway.
    specs = []
    for host in sorted(set([GATEWAY] + gateways)):
        specs.append(f"{host} {ROOM_PORT} /?room={room or 'production'}")
        for port in ANDROID_PORTS:
            specs.append(f"{host} {port} /")
    code, out = dexec(c, ["python3", "-c", PROBE_SRC] + specs, timeout=60)
    try:
        results = json.loads(out.strip().splitlines()[-1]) if code == 0 else []
    except (ValueError, IndexError):
        results = []
    if not results:
        say(f"  probes from inside: could not run (exit {code}): {out.strip()[-160:]}")
    reach = {}
    for r in results:
        say("  from inside: " + probe_text(r))
        reach[r["target"]] = "ok" if answered(r) else (r.get("connect") or r.get("answer") or "?")
    info["reach"] = reach
    info["reach_room"] = next((v for k, v in reach.items() if f":{ROOM_PORT}/" in k), "?")

    # The click guard in its client page.
    code, html = dexec(c, ["cat", "/var/www/index.html"], timeout=15)
    tags = re.findall(r"""<script[^>]*\bsrc=["']?([^"'\s>]+)""", html) if code == 0 else []
    info["guard_tag"] = bool(INPUT_TAG_RX.search(html or ""))
    code, js = dexec(c, ["cat", "/var/www/" + INPUT_JS], timeout=15, binary=True)
    info["guard_js_sha"] = hashlib.sha256(js).hexdigest() if code == 0 else ""
    info["guard_current"] = info["guard_tag"] and info["guard_js_sha"] == js_sha
    say(f"  its client page loads: {', '.join(tags[:10]) or '(unreadable)'}")
    say("  the click guard: " + ("installed, this kit's version" if info["guard_current"] else
                                 "installed, another version" if info["guard_tag"] else "not installed"))
    code, out = dsh(c, "ls /var/www/js 2>/dev/null | head -20; for f in /var/www/js/app*.js; do [ -f \"$f\" ] || continue; "
                       "echo \"== $f\"; for m in isMouseDown reqMouseUp implicitHostingRequest onMouseUp onMouseDown "
                       "pointerup pointerdown createDataChannel; do printf '%s=%s ' $m $(grep -o $m \"$f\" | wc -l); done; echo; done",
                    timeout=20)
    for ln in out.splitlines():
        if ln.startswith("== ") or "=" in ln:
            say("  client bundle " + ln.strip())

    # X: the pointer, the buttons X holds, the screen, the windows.
    display = env.get("DISPLAY", ":99.0")
    code, out = dexec(c, ["python3", "-c", XCAP_SRC, "info", display], timeout=20)
    try:
        x = json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        x = {"error": out.strip()[-160:] or f"exit {code}"}
    if "error" in x:
        say(f"  X ({display}): {x['error']}")
    else:
        held = [name for bit, name in ((8, "left"), (9, "middle"), (10, "right")) if x["mask"] & (1 << bit)]
        info["x_buttons_held"] = held
        say(f"  X ({display}): screen {x['width']}x{x['height']}, pointer at {x['x']},{x['y']}, buttons held in X: "
            f"{', '.join(held) or 'none'} (mask {x['mask']})")
    code, png = dexec(c, ["python3", "-c", XCAP_SRC, "png", display], timeout=60, binary=True)
    if code == 0 and png[:8] == b"\x89PNG\r\n\x1a\n":
        save(f"containers/{c}/screen.png", png)
        say(f"  its screen: one picture, {len(png)} bytes (fetched)")
    else:
        say(f"  its screen: not captured (exit {code})")
    code, out = dsh(c, "xprop -root _NET_ACTIVE_WINDOW _NET_CLIENT_LIST_STACKING 2>&1", timeout=15, env={"DISPLAY": display})
    ids = re.findall(r"0x[0-9a-f]+", out.split("_NET_CLIENT_LIST_STACKING")[-1]) if "_NET_CLIENT_LIST_STACKING" in out else []
    active = re.findall(r"_NET_ACTIVE_WINDOW\(WINDOW\): window id # (0x[0-9a-f]+)", out)
    if ids:
        say(f"  its windows ({len(ids)}, bottom to top; active {active[0] if active else '?'}):")
        for wid in ids[-8:]:
            code, p = dsh(c, f"xprop -id {wid} WM_NAME _NET_WM_STATE WM_STATE 2>&1; xwininfo -id {wid} 2>&1 | "
                             "grep -E 'Absolute upper-left|Width:|Height:|Map State'", timeout=15, env={"DISPLAY": display})
            name = re.search(r'WM_NAME\(\w+\) = "(.*)"', p)
            state = re.search(r"_NET_WM_STATE\(ATOM\) = (.*)", p)
            geo = re.findall(r"(?:Absolute upper-left [XY]|Width|Height):\s+(-?\d+)", p)
            mapped = re.search(r"Map State: (\w+)", p)
            say(f"    {wid} {name.group(1) if name else '?'!r}: {'x'.join(geo[2:4]) if len(geo) >= 4 else '?'}+"
                f"{'+'.join(geo[:2]) if len(geo) >= 2 else '?'}, {mapped.group(1) if mapped else '?'}, "
                f"state {state.group(1) if state else 'none'}")
    else:
        code, out = dsh(c, "xwininfo -root -tree 2>/dev/null | grep -E '^ +0x[0-9a-f]+ \"' | head -30", timeout=15,
                        env={"DISPLAY": display})
        say("  its named windows: " + ("; ".join(ln.strip()[:120] for ln in out.splitlines()) or "(none listed)"))

    # Chromium: its command line, its window placement, its tabs.
    code, out = dsh(c, "for p in $(pgrep -f '/usr/lib/chromium/[c]hromium' 2>/dev/null); do a=$(tr '\\0' ' ' < /proc/$p/cmdline); "
                       "case \"$a\" in *--type=*) ;; *) echo \"$p $a\";; esac; done | head -3", timeout=15)
    for ln in out.splitlines()[:2]:
        say("  chromium: " + ln.strip()[:300])
    code, out = dexec(c, ["python3", "-c", PREFS_SRC, "/profiles/active/Default/Preferences"], timeout=15)
    if out.strip():
        say("  its window placement: " + out.strip()[-260:])
    code, out = dexec(c, ["python3", "-c", INSIDE_FETCH, "http://127.0.0.1:9223/json/list"], timeout=15)
    try:
        targets = json.loads(out) if code == 0 else []
    except ValueError:
        targets = []
    say(f"  its tabs: " + ("; ".join(f"{t.get('type')} {t.get('title', '')[:40]!r} {t.get('url', '')[:100]}" for t in targets[:6])
                          or f"(DevTools did not answer: exit {code})"))

    # The room's browser's own log (its stderr), and whether it can play what Netflix and the like protect (Widevine).
    code, out = dsh(c, "for f in /var/log/neko/chromium*.log; do [ -f \"$f\" ] && tail -n 400 \"$f\"; done 2>/dev/null", timeout=20)
    lines = [ln for ln in out.splitlines() if ln.strip()]
    if lines:
        save(f"containers/{c}/chromium-log.txt", "\n".join(lines))
        worth = [ln for ln in lines if re.search(r"(?i)error|fatal|warn|crash|widevine|cdm|killed|oom|renderer|gpu|sandbox|"
                                                 r"certificate|net::|unsupported|failed", ln)]
        say(f"  its browser's own log ({len(lines)} last lines; {len(worth)} about errors, repeats folded):")
        for ln in collapse(worth[-300:], keep=16):
            say("    " + ln[:260])
    else:
        say("  its browser's own log: none found in /var/log/neko")
    code, out = dsh(c, "for d in /usr/lib/chromium/WidevineCdm /opt/google/chrome/WidevineCdm /profiles/active/WidevineCdm "
                       "/home/neko/.config/chromium/WidevineCdm; do [ -e \"$d\" ] && echo \"$d: $(ls \"$d\" 2>/dev/null | head -3 | tr '\\n' ' ')\"; "
                       "done; ls /usr/lib/chromium 2>/dev/null | grep -i -m3 widevine", timeout=15)
    info["widevine"] = out.strip().splitlines()[:4]
    say("  Widevine (what Netflix needs to play a film): " + ("; ".join(info["widevine"]) if info["widevine"] else
                                                               "not found -- sign-in works without it, playing a film does not"))

    # openbox: what a click on the desktop does.
    files = {}
    for f in ("/etc/xdg/openbox/rc.xml", "/etc/xdg/openbox/menu.xml", "/home/neko/.config/openbox/rc.xml",
              "/home/neko/.config/openbox/menu.xml", "/home/neko/.config/openbox/autostart", "/var/log/neko/openbox.log"):
        code, text = dexec(c, ["cat", f], timeout=15)
        if code == 0:
            files[f] = text
            save(f"containers/{c}/openbox/" + f.strip("/").replace("/", "_"), text)
    rc = files.get("/home/neko/.config/openbox/rc.xml") or files.get("/etc/xdg/openbox/rc.xml") or ""
    root_ctx = re.search(r'(?s)<context name="Root">(.*?)</context>', rc)
    binds = re.findall(r'<mousebind button="(\w+)" action="(\w+)">\s*<action name="(\w+)"(?:>\s*<menu>([\w-]+)</menu>)?', root_ctx.group(1)) \
        if root_ctx else []
    say("  openbox: " + (f"a click on the desktop does: " + ", ".join(f"{b} {a} -> {n}{(' ' + m) if m else ''}" for b, a, n, m in binds)
                         if binds else "no desktop mouse bindings read") + f" ({len(files)} file(s) read)")
    log = files.get("/var/log/neko/openbox.log", "")
    for ln in log.splitlines()[-6:]:
        say("    openbox.log: " + ln.strip()[:240])

    # neko's lines about presses, releases and who holds the controls, with their times.
    code, out = run(["docker", "logs", "--timestamps", "--since", "72h", c], timeout=60)
    lines = [ANSI.sub("", ln) for ln in out.splitlines()]
    hits = [ln for ln in lines if NEKO_RX.search(ln)]
    counts = {"button down failed": 0, "button up failed": 0, "key down failed": 0, "key up failed": 0}
    per_hour = {}
    for ln in hits:
        for k in counts:
            if k in ln:
                counts[k] += 1
                hour = ln[:13]
                per_hour.setdefault(hour, {}).setdefault(k, 0)
                per_hour[hour][k] += 1
    info["neko_counts"] = counts
    say(f"  neko, the last 72 h: " + ", ".join(f"{k} {v}" for k, v in counts.items()))
    for hour, kinds in sorted(per_hour.items())[-12:]:
        say(f"    {hour}h: " + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())))
    save(f"containers/{c}/neko-lines.txt", "\n".join(hits))
    say(f"  neko's last lines about input and control ({min(len(hits), 60)} of {len(hits)}, repeats folded):")
    for ln in collapse(hits[-400:], keep=60):
        say("    " + ln[:300])


def look_android():
    say("")
    say("===== Android (Waydroid)")
    code, out = run(["waydroid", "status"], timeout=20)
    status = " | ".join(ln.strip() for ln in out.splitlines() if ln.strip())
    say("  waydroid status: " + status)
    m = re.search(r"Container:\s*(\w+)", out)
    FIND["android"]["container"] = m.group(1) if m else "?"
    units = ["waydroid-container.service", "open77-waydroid-session.service", "open77-waydroid-display.service",
             "open77-android-native-display.service", "open77-android-native-vnc.service", "open77-android-native-web.service",
             "open77-android-native-adb.service", "open77-android-web.service", "open77-android-emulator.service"]
    code, out = run(["systemctl", "show"] + units + ["-p", "Id,ActiveState,SubState,NRestarts,ActiveEnterTimestamp"], timeout=20)
    for block in out.split("\n\n"):
        p = dict(ln.split("=", 1) for ln in block.splitlines() if "=" in ln)
        if p.get("Id"):
            say(f"  {p['Id']}: {p.get('ActiveState')}/{p.get('SubState')}, restarts {p.get('NRestarts')}, since {p.get('ActiveEnterTimestamp')}")
            if p["Id"] == "open77-waydroid-session.service":
                FIND["android"]["session_restarts"] = p.get("NRestarts")
    for unit in ("waydroid-container.service", "open77-waydroid-session.service"):
        code, out = run(["journalctl", "-u", unit, "-n", "400", "--no-pager", "-o", "short-iso"], timeout=30)
        lines = [ln for ln in out.splitlines() if ln.strip() and not ln.startswith("-- ")]
        save(f"android/journal-{unit}.txt", out)
        say(f"  {unit}'s journal ({len(lines)} lines, repeats folded):")
        for ln in collapse(lines, keep=18):
            say("    " + ln[:300])
    for f in ("/var/lib/waydroid/waydroid.log", "/var/lib/waydroid/lxc/waydroid/waydroid.log", "/var/lib/waydroid/waydroid.cfg"):
        try:
            lines = open(path(f), encoding="utf-8", errors="replace").read().splitlines()
        except OSError:
            continue
        save("android/" + f.strip("/").replace("/", "_"), "\n".join(lines[-400:]))
        if f.endswith(".cfg"):
            say(f"  {f}: " + "; ".join(ln.strip() for ln in lines if ln.strip() and not ln.startswith("["))[:280])
            continue
        say(f"  {f} (last {min(len(lines), 200)} lines, repeats folded):")
        for ln in collapse(lines[-200:], keep=24):
            say("    " + ln[:300])
    code, k = run(["uname", "-r"], timeout=10)
    code, fs = run(["grep", "-E", "binder|ashmem", path("/proc/filesystems")], timeout=10)
    code, mods = run(["sh", "-c", "lsmod 2>/dev/null | grep -E '^(binder|ashmem)' ; ls " + path("/dev/binderfs") + " 2>&1 | head -5"],
                     timeout=10)
    say(f"  kernel {k.strip()}; binder in /proc/filesystems: {' '.join(fs.split()) or 'no'}; " +
        " | ".join(ln.strip() for ln in mods.splitlines() if ln.strip())[:200])
    adb = "/opt/android-sdk/platform-tools/adb"
    code, out = run([adb if os.path.exists(path(adb)) or not ROOT else "adb", "devices", "-l"], timeout=15)
    say("  adb devices: " + (" | ".join(ln.strip() for ln in out.splitlines()[1:] if ln.strip()) or f"none (exit {code})"))


def conclude(js_sha):
    say("")
    say("===== what this means")
    notes = []
    room = FIND["room"]
    if room.get("active") != "active":
        notes.append(f"the room service is {room.get('active')}/{room.get('sub')}: the home screen's tiles have no server")
    elif not room.get("answers_loopback"):
        notes.append("the room service runs but does not answer (" + room.get("probe_loopback", "?") + "): every tile waits "
                     "on it, so a click on Chrome, Netflix or Android does nothing" +
                     (f"; it has {len(room['long_children'])} child process(es) running for a long time (below)"
                      if room.get("long_children") else ""))
        for k in (room.get("long_children") or [])[:4]:
            notes.append(f"    under it for {k['seconds']} s: {k['command'][:200]}")
    else:
        notes.append("the room service answers on this machine")
    blocked = [c for c, i in FIND["containers"].items() if i.get("reach_room") not in ("ok", "?")]
    if blocked:
        notes.append("from inside " + ", ".join(blocked) + " the room service does not answer (" +
                     "; ".join(f"{c}: {FIND['containers'][c].get('reach_room')}" for c in blocked) + ")")
    for c, i in FIND["containers"].items():
        n = (i.get("neko_counts") or {}).get("button down failed", 0)
        if n:
            notes.append(f"{c}: neko refused {n} press(es) in 72 h as 'debounced' -- the release of a click never "
                         "reached it, so the next press was refused (the click guard fixes this and logs every click)")
        if i.get("x_buttons_held"):
            notes.append(f"{c}: X holds {', '.join(i['x_buttons_held'])} right now")
    a = FIND["android"]
    if a.get("container") not in ("RUNNING", "?"):
        notes.append(f"Android: Waydroid's container is {a.get('container')} (its session restarted {a.get('session_restarts')} "
                     "times): the Android tile has nothing to show")
    for n in notes:
        say("  * " + n)
    plan = ["install the click guard (open77-input.js) on " + ", ".join(sorted(FIND["containers"])) if FIND["containers"] else ""]
    if room.get("active") == "active" and not room.get("answers_loopback"):
        plan.append("restart the room service (it does not answer)")
    FIND["notes"] = notes
    FIND["plan"] = [p for p in plan if p]
    FIND["guard_sha"] = js_sha
    say("  the fix T4 offers: " + "; ".join(FIND["plan"]) + " -- each checked, the guard put back by itself where it does not check out")


def pack():
    with tarfile.open(os.path.join(W, "clicks.tgz"), "w:gz") as t:
        t.add(C, arcname="clicks")
    n = sum(len(fns) for _, _, fns in os.walk(C))
    say(f"  packed: {n} file(s) in clicks.tgz ({os.path.getsize(os.path.join(W, 'clicks.tgz'))} bytes)")


def look():
    os.makedirs(C, exist_ok=True)
    js_sha = sha_file(os.path.join(W, INPUT_JS))
    code, host = run(["hostname"], timeout=10)
    say(f"the TV room's clicks, read only, on {host.strip()} at {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}")
    look_room()
    look_room_files()
    look_room_logs()
    live = containers()
    say("  shared-browser containers: " + ", ".join(f"{c}{'' if up else ' (not running)'}" for c, up in live))
    rooms = []
    for c, up in live:
        if not up:
            continue
        look_container(c, js_sha)
        rooms.append(FIND["containers"][c].get("room") or "")
    say("")
    say("===== the home pages")
    look_room_pages([r for r in rooms if r])
    look_android()
    conclude(js_sha)
    with open(os.path.join(C, "findings.json"), "w") as f:
        json.dump(FIND, f, indent=1, default=str)
    pack()
    return 0


# -- fix ---------------------------------------------------------------------------------------------------------

def sha_file(p):
    try:
        return hashlib.sha256(open(p, "rb").read()).hexdigest()
    except OSError:
        return ""


def arg(name, default=""):
    return ARGS[ARGS.index(name) + 1] if name in ARGS and ARGS.index(name) + 1 < len(ARGS) else default


def neko_get(c, url_path, env):
    """A GET to neko inside the container, as its browser-side client gets the page (body kept)."""
    bind = env.get("NEKO_SERVER_BIND", ":8080")
    port = int(bind.rsplit(":", 1)[-1] or 8080)
    code, out = dexec(c, ["python3", "-c", PROBE_SRC, "--keep=1048576", f"127.0.0.1 {port} {url_path}"], timeout=30)
    try:
        return json.loads(out.strip().splitlines()[-1])[0]
    except (ValueError, IndexError, KeyError):
        return {"target": url_path, "connect": f"the probe did not run (exit {code})"}


def put_file(c, local, dest):
    os.chmod(local, 0o644)
    code, out = run(["docker", "cp", local, f"{c}:{dest}"], timeout=60)
    if code != 0:
        raise RuntimeError(f"docker cp to {c}:{dest}: exit {code}: {out.strip()[-160:]}")


def install_input(c, js, js_sha, keep_dir):
    env = neko_env(c)
    prefix = env.get("NEKO_SERVER_PATH_PREFIX", "").rstrip("/")
    code, html = dexec(c, ["cat", "/var/www/index.html"], timeout=15)
    if code != 0 or "<head>" not in html:
        say(f"  {c}: its index.html could not be read or has no <head> (exit {code}): not changed")
        return {"result": "skipped", "why": "index.html unreadable"}
    code, old_js = dexec(c, ["cat", "/var/www/" + INPUT_JS], timeout=15, binary=True)
    tag = f'<script src="{INPUT_JS}?v={js_sha[:8]}"></script>'
    if INPUT_TAG_RX.search(html):
        new_html = INPUT_TAG_RX.sub(lambda m: tag, html, count=1)
        how = "its earlier tag replaced"
    elif FIT_TAG_RX.search(html):
        new_html = FIT_TAG_RX.sub(lambda m: m.group(0) + tag, html, count=1)
        how = "tag added after open77-fit.js"
    else:
        new_html = html.replace("<head>", "<head>" + tag, 1)
        how = "tag added at the top of <head>"
    if new_html == html and code == 0 and hashlib.sha256(old_js).hexdigest() == js_sha:
        say(f"  {c}: the click guard is already there, this version: nothing to do")
        return {"result": "already", "tag": tag}
    d = os.path.join(keep_dir, c)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "index.html"), "w", encoding="utf-8") as f:
        f.write(html)
    if code == 0:
        with open(os.path.join(d, INPUT_JS + ".before"), "wb") as f:
            f.write(old_js)
    say(f"  {c}: index.html kept in {d} ({len(html)} bytes, sha256 {hashlib.sha256(html.encode()).hexdigest()[:12]})")
    staged = os.path.join(W, f"index-{c}.html")
    with open(staged, "w", encoding="utf-8") as f:
        f.write(new_html)
    record = {"result": "", "tag": tag, "had_js": code == 0, "how": how}
    try:
        put_file(c, js, "/var/www/" + INPUT_JS)
        put_file(c, staged, "/var/www/index.html")
        # The check: what is in the container is what was sent, and neko serves both.
        code, now_html = dexec(c, ["cat", "/var/www/index.html"], timeout=15)
        code2, now_js = dexec(c, ["cat", "/var/www/" + INPUT_JS], timeout=15, binary=True)
        problems = []
        if code != 0 or now_html != new_html:
            problems.append("index.html in the container is not what was sent")
        if len(INPUT_TAG_RX.findall(now_html)) != 1:
            problems.append(f"index.html carries the guard's tag {len(INPUT_TAG_RX.findall(now_html))} times")
        if FIT_TAG_RX.search(html) and not FIT_TAG_RX.search(now_html):
            problems.append("open77-fit.js's tag went missing")
        if code2 != 0 or hashlib.sha256(now_js).hexdigest() != js_sha:
            problems.append("open77-input.js in the container is not what was sent")
        served = neko_get(c, f"{prefix}/{INPUT_JS}?v={js_sha[:8]}", env)
        page = neko_get(c, f"{prefix}/", env)
        served_ok = str(served.get("status", "")).split(" ")[1:2] == ["200"]
        if not served_ok:
            problems.append("neko does not serve the script: " + probe_text(served))
        elif hashlib.sha256((served.get("body") or "").encode("utf-8")).hexdigest() != js_sha:
            problems.append("neko serves a different script than the one sent")
        if tag not in (page.get("body") or ""):
            problems.append("neko's page does not carry the tag: " + probe_text(page))
        if problems:
            raise RuntimeError("; ".join(problems))
        say(f"  {c}: the click guard installed ({how}); neko serves it ({probe_text(served)}) and its page loads it")
        record["result"] = "installed"
        return record
    except RuntimeError as e:
        say(f"  {c}: NOT right: {e} -- putting it back")
        record["result"] = "put back"
        record["why"] = str(e)
        putback_one(c, d, record)
        return record


def putback_one(c, d, record):
    saved = os.path.join(d, "index.html")
    try:
        put_file(c, saved, "/var/www/index.html")
        before = os.path.join(d, INPUT_JS + ".before")
        if os.path.isfile(before):
            put_file(c, before, "/var/www/" + INPUT_JS)
        else:
            dexec(c, ["mv", "-f", "/var/www/" + INPUT_JS, f"/var/www/{INPUT_JS}.off-{time.strftime('%Y%m%d-%H%M%S', time.gmtime())}"],
                  timeout=15)
        code, now = dexec(c, ["cat", "/var/www/index.html"], timeout=15)
        ok = code == 0 and now == open(saved, encoding="utf-8").read()
        say(f"  {c}: put back " + ("(its index.html is the kept one again)" if ok else "-- BUT its index.html does not match the kept one"))
        return ok
    except RuntimeError as e:
        say(f"  {c}: could NOT put back: {e}")
        return False


def restart_room_if_stuck():
    say("===== the room service: restarted only if it does not answer")
    first = probe("127.0.0.1", ROOM_PORT, "/?room=production")
    say("  " + probe_text(first))
    if answered(first):
        say("  it answers: not restarted")
        return {"restarted": False, "why": "answers"}
    time.sleep(3)
    second = probe("127.0.0.1", ROOM_PORT, "/?room=production")
    say("  again: " + probe_text(second))
    if answered(second):
        say("  it answers now: not restarted")
        return {"restarted": False, "why": "answered the second time"}
    look_room()     # its process tree and sockets, said once more before the restart
    code, out = run(["systemctl", "restart", ROOM_UNIT], timeout=120)
    say(f"  systemctl restart {ROOM_UNIT}: exit {code} {out.strip()[-200:]}")
    for i in range(15):
        time.sleep(2)
        r = probe("127.0.0.1", ROOM_PORT, "/?room=production")
        if answered(r):
            say(f"  after the restart: {probe_text(r)}")
            return {"restarted": True, "answers": True}
    say("  after the restart: it still does not answer -- " + probe_text(r))
    return {"restarted": True, "answers": False}


def fix():
    ts = arg("--ts") or time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    js = os.path.join(W, INPUT_JS)
    js_sha = sha_file(js)
    record = {"ts": ts, "containers": {}, "room": None, "guard_sha": js_sha}
    rc = 0
    if "--input" in ARGS:
        if not js_sha:
            say(f"!! {INPUT_JS} is not next to this script: nothing installed")
            return 2
        keep = path(f"{BACKUPS}/tvinput-{ts}")
        os.makedirs(keep, exist_ok=False)
        say(f"===== the click guard ({INPUT_JS}, sha256 {js_sha[:12]}) on every running shared browser; kept in {keep}")
        for c, up in containers():
            if not up:
                say(f"  {c}: not running: not changed")
                continue
            r = install_input(c, js, js_sha, keep)
            record["containers"][c] = r
            if r["result"] in ("put back", "skipped"):
                rc = 1
        with open(os.path.join(keep, "done.json"), "w") as f:
            json.dump(record, f, indent=1)
    if "--room-if-stuck" in ARGS:
        record["room"] = restart_room_if_stuck()
        if record["room"].get("restarted") and not record["room"].get("answers"):
            rc = 1
    if arg("--home"):
        record["home"] = home_room(arg("--home"))
        if not record["home"].get("ok"):
            rc = 1
    if os.path.isdir(C) and any(files for _, _, files in os.walk(C)):
        pack()        # the pictures the fix took, for the kit to copy back
    say("")
    say("done: " + "; ".join(f"{c} {r['result']}" for c, r in record["containers"].items()) +
        (f"; {record['home'].get('room')}: {record['home'].get('result')}" if record.get("home") else "") +
        (f"; room service {'restarted' if record['room'].get('restarted') else 'not restarted'}"
         f"{', answers' if record['room'].get('answers') or not record['room'].get('restarted') else ', DOES NOT answer'}"
         if record["room"] else ""))
    return rc


ROOM_CONTAINER = {"production": "open77-tvbrowser-xb", "staging": "open77-tvbrowser-xbs",
                  "f2077": "open77-tvbrowser-f2077", "fr-us": "open77-tvbrowser-fr-us"}


def room_state(role):
    r = probe("127.0.0.1", ROOM_PORT, f"/api/state?room={role}", keep=262144)
    try:
        return json.loads(r.get("body") or "{}"), r
    except ValueError:
        return {}, r


def screen_stats(c):
    """One picture of the container's screen, kept, and how much of it is one colour (a blank or white screen)."""
    env = neko_env(c)
    code, png = dexec(c, ["python3", "-c", XCAP_SRC, "png", env.get("DISPLAY", ":99.0")], timeout=60, binary=True)
    if code != 0 or png[:8] != b"\x89PNG\r\n\x1a\n":
        return None, "no picture"
    import zlib
    import struct
    i, idat, w, h = 8, b"", 0, 0
    while i < len(png):
        n = struct.unpack(">I", png[i:i + 4])[0]
        kind = png[i + 4:i + 8]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", png[i + 8:i + 16])
        elif kind == b"IDAT":
            idat += png[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    seen = {}
    for y in range(0, h, 12):
        row = raw[y * stride + 1:(y + 1) * stride]
        for x in range(0, w, 12):
            px = row[x * 3:x * 3 + 3]
            seen[px] = seen.get(px, 0) + 1
    total = sum(seen.values())
    top, count = max(seen.items(), key=lambda kv: kv[1])
    return png, f"{count * 100 // total}% of it one colour (#{top.hex()})"


def home_room(role):
    """The room's browser back on its own home screen, through the room service."""
    say("")
    say(f"===== the {role} room back on its home screen")
    c = ROOM_CONTAINER.get(role)
    if not c:
        say(f"  no room called {role}")
        return {"room": role, "result": "no such room", "ok": False}
    st, r = room_state(role)
    if not st:
        say("  the room service does not answer: " + probe_text(r))
        return {"room": role, "result": "the room service does not answer", "ok": False}
    home = f"http://{GATEWAY}:{ROOM_PORT}/?room={role}"
    url = st.get("roomUrl") or ""
    png, look = screen_stats(c)
    if png:
        save(f"home/{c}-before.png", png)
    say(f"  it shows {mask(url)[:120] or '(nothing the room service can read)'}; its screen: {look}; chromium "
        f"{str(st.get('chromium'))[:60]}")
    if url == home or url.startswith(home + "&"):
        say("  already on its home screen: nothing to do")
        return {"room": role, "result": "already home", "ok": True}
    body = json.dumps({"room": role, "action": "open", "url": home}).encode()
    a = probe_post("/api/action", body)
    say(f"  \"open\" its home screen (what its tiles ask for): {probe_text(a)} -> {(a.get('body') or '')[:160]}")
    time.sleep(3)
    st2, _ = room_state(role)
    url2 = st2.get("roomUrl") or ""
    png2, look2 = screen_stats(c)
    if png2:
        save(f"home/{c}-after.png", png2)
    ok = url2 == home or url2.startswith(home + "&")
    say(f"  now it shows {mask(url2)[:120] or '?'}; its screen: {look2}" + ("" if ok else " -- NOT its home screen"))
    if not ok:
        say("  asking the room service to restart its browser (it comes back on its home screen, its start page)")
        a = probe_post("/api/action", json.dumps({"room": role, "action": "restart"}).encode(), timeout=60)
        say(f"  restart: {probe_text(a)} -> {(a.get('body') or '')[:160]}")
        time.sleep(8)
        st3, _ = room_state(role)
        url3 = st3.get("roomUrl") or ""
        png3, look3 = screen_stats(c)
        if png3:
            save(f"home/{c}-after-restart.png", png3)
        ok = url3 == home or url3.startswith(home + "&")
        say(f"  after the restart it shows {mask(url3)[:120] or '?'}; its screen: {look3}")
    return {"room": role, "result": "home" if ok else "NOT home", "ok": ok, "was": mask(url)[:120]}


def probe_post(url_path, body, timeout=30):
    """A POST to the room service on loopback: (probe-like dict with code and body)."""
    import http.client
    t0 = time.time()
    try:
        conn = http.client.HTTPConnection("127.0.0.1", ROOM_PORT, timeout=timeout)
        conn.request("POST", url_path, body=body, headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        data = resp.read(65536).decode("utf-8", "replace")
        conn.close()
        ms = int((time.time() - t0) * 1000)
        return {"target": f"127.0.0.1:{ROOM_PORT}{url_path}", "connect_ms": 0, "first_byte_ms": ms,
                "status": f"HTTP/1.1 {resp.status} {resp.reason}", "bytes": len(data), "body": data}
    except (OSError, http.client.HTTPException) as e:
        return {"target": f"127.0.0.1:{ROOM_PORT}{url_path}", "connect": f"{e.__class__.__name__}: {e}"}


def putback():
    """Every running shared browser's page made not to load the guard: its tag taken out of index.html (the page as
    it was kept first) and the script set aside; checked.  The installs' own copies stay in /opt/open77-backups."""
    ts = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    keep = path(f"{BACKUPS}/tvinput-putback-{ts}")
    say(f"===== the click guard put back on every running shared browser; each page kept first in {keep}")
    rc, changed = 0, 0
    for c, up in containers():
        if not up:
            say(f"  {c}: not running: not changed")
            continue
        code, html = dexec(c, ["cat", "/var/www/index.html"], timeout=15)
        if code != 0:
            say(f"  {c}: its index.html could not be read (exit {code}): not changed")
            rc = 1
            continue
        if not INPUT_TAG_RX.search(html):
            say(f"  {c}: its page does not load the guard: nothing to put back")
            continue
        d = os.path.join(keep, c)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "index.html"), "w", encoding="utf-8") as f:
            f.write(html)
        new_html = INPUT_TAG_RX.sub("", html)
        staged = os.path.join(W, f"index-{c}.html")
        with open(staged, "w", encoding="utf-8") as f:
            f.write(new_html)
        try:
            put_file(c, staged, "/var/www/index.html")
        except RuntimeError as e:
            say(f"  {c}: could NOT put back: {e}")
            rc = 1
            continue
        dexec(c, ["mv", "-f", "/var/www/" + INPUT_JS, f"/var/www/{INPUT_JS}.off-{ts}"], timeout=15)
        code, now = dexec(c, ["cat", "/var/www/index.html"], timeout=15)
        first = sorted(x for x in (os.listdir(path(BACKUPS)) if os.path.isdir(path(BACKUPS)) else [])
                       if re.fullmatch(r"tvinput-[\d-]+", x) and os.path.isfile(os.path.join(path(BACKUPS), x, c, "index.html")))
        same = ""
        if first:
            before = open(os.path.join(path(BACKUPS), first[0], c, "index.html"), encoding="utf-8").read()
            same = "; the page is again exactly as it was before the first install" if before == now else \
                "; the page differs from the one kept before the first install (something else changed it since)"
        if code == 0 and now == new_html and not INPUT_TAG_RX.search(now):
            say(f"  {c}: put back (its page no longer loads the guard; the script set aside as {INPUT_JS}.off-{ts}){same}")
            changed += 1
        else:
            say(f"  {c}: NOT put back cleanly: its index.html is not what was sent")
            rc = 1
    say(f"done: {changed} page(s) put back")
    return rc


if __name__ == "__main__":
    if MODE == "look":
        sys.exit(look())
    if MODE == "fix":
        sys.exit(fix())
    if MODE == "putback":
        sys.exit(putback())
    print(__doc__)
    sys.exit(2)
