#!/usr/bin/env python3
"""The TV room's repair (2026-11-07): the three things the deeper look (tv_clicks.py, T4) found behind "a click on
Chrome, Netflix or Android does nothing".  tv_kit.py sends this, with the room service's new server.mjs, to a work
folder of its own on the dedicated machine:

    python3 tv_repair.py look    WORKDIR [--room] [--android] [--windows]             READ ONLY
    python3 tv_repair.py fix     WORKDIR [--room] [--android] [--windows] --ts TS
    python3 tv_repair.py putback WORKDIR [--room] [--android]

No part named means all of them.

--room      The room service (open77-room, the home screen's server on port 8002) answered nothing: every answer it
            gave was worked out by synchronous child processes on the request path (adb devices and adb connect --
            eight seconds while Waydroid is down --, du over every profile, sqlite3 over every cookie jar, docker inspect
            and docker exec for every room), and four televisions ask /api/state every five seconds, so the line never
            emptied and a tile's click (POST /api/action) was never served; a restart did not help.  fix puts in the
            server.mjs that keeps those answers fresh in the background instead (only over the exact version it was
            made for, sha256 b7339da1...), kept first in /opt/open77-backups/open77-room-TS/, checked with node
            --check, restarted, and checked the way the televisions ask (every room's state, side by side, a few
            rounds); if it does not answer it is put back by itself.  One "open" is then asked for a room that is
            already showing its home screen (the same page again: the screen reloads) to see a click go through.
--android   Waydroid's container never starts: waydroid.cfg names binder nodes (/dev/binder, /dev/vndbinder,
            /dev/hwbinder) this machine does not have; its binderfs has anbox-binder, anbox-vndbinder and
            anbox-hwbinder (what Waydroid itself makes when it finds none).  fix points waydroid.cfg and the LXC
            node list (config_nodes) at those -- what `waydroid init` writes on a machine like this one --, both kept
            first in /opt/open77-backups/waydroid-TS/, then restarts the container service and the session and waits
            for the container to run and adb to see Android.  If the container does not come up, both files are put
            back and the services restarted as they were.
--windows   Three of the four televisions' Chromium windows are 1050x700 on a 1280x720 screen (Chromium started
            before openbox, so its kiosk window was never made fullscreen): the picture is cut off, and the uncovered
            desktop opens openbox's menu on a right click.  fix asks openbox for fullscreen, the way wmctrl does
            (_NET_WM_STATE add FULLSCREEN), for every Chromium window that does not fill its screen, and reads the
            windows again.  Nothing is written anywhere; a Chromium restart decides the window again.

putback puts the room service's and Waydroid's files back from their newest copies in /opt/open77-backups (each only
if it is still what fix wrote) and restarts what it changed.  Passwords, tokens and keys are never printed: a value
whose name says secret is <hidden>.  The last line is "TV result: {...}".
"""
import hashlib
import json
import os
import re
import shlex
import shutil
import socket
import subprocess
import sys
import threading
import time

MODE = sys.argv[1] if len(sys.argv) > 1 else ""
W = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else ".")
ARGS = sys.argv[3:]
ROOT = os.environ.get("O77ROOT", "")                      # "" on the machine: the rehearsal's root of fake files
ROOM_UNIT = "open77-room.service"
ROOM_JS = "/opt/open77-room/server.mjs"
ROOM_PORT = int(os.environ.get("O77_ROOM_PORT", "8002"))  # the rehearsal's stand-in listens elsewhere
GATEWAY = os.environ.get("O77_GATEWAY", "172.17.0.1")
# The versions are compared by their content with every CRLF read as LF: rl31's file has Windows line endings
# (35462 bytes, sha256 d5bd80c2...), the same text as b7339da1... (34649 bytes); the new file is written back with the
# line endings the old one had.
ROOM_OLD_SHA = "b7339da1bbdee665484cd08b8ff618dc049d57b0f01f82eed9c466ea60484487"
ROOM_NEW_SHA = "f780e71f27ccb34ff4c6edd3016b8a763dfa8bc21aa9450432df3d1f15813c4b"
ROOMS = {"production": "open77-tvbrowser-xb", "staging": "open77-tvbrowser-xbs",
         "f2077": "open77-tvbrowser-f2077", "fr-us": "open77-tvbrowser-fr-us"}
WD_CFG = "/var/lib/waydroid/waydroid.cfg"
WD_NODES = "/var/lib/waydroid/lxc/waydroid/config_nodes"
WD_UNIT = "waydroid-container.service"
WD_SESSION = "open77-waydroid-session.service"
WD_UNITS = [WD_UNIT, WD_SESSION, "open77-waydroid-display.service", "open77-android-native-display.service",
            "open77-android-native-vnc.service", "open77-android-native-web.service", "open77-android-native-adb.service",
            "open77-android-web.service", "open77-android-emulator.service"]
BINDERS = (("binder", "anbox-binder"), ("vndbinder", "anbox-vndbinder"), ("hwbinder", "anbox-hwbinder"))
WAIT_CONTAINER = int(os.environ.get("O77_WAIT_CONTAINER", "150"))
WAIT_ANDROID = int(os.environ.get("O77_WAIT_ANDROID", "150"))
BACKUPS = "/opt/open77-backups"
LINE_MAX = 320

SECRET_NAME = re.compile(r"(?i)pass|secret|token|cred|apikey|api_key|private|cookie|pwd")
URL_SECRET = re.compile(r"(?i)\b((?:pwd|password|passwd|pass|token|access_token|api_?key|secret|auth|sig|signature|"
                        r"ticket|session|sid|key)=)[^&\s\"'#<>`]+")
JSON_SECRET = re.compile(r'(?i)("[^"]*(?:pass|secret|token|cred|apikey|api_key|private|pwd)[^"]*"\s*:\s*")[^"]*(")')
KV_SECRET = re.compile(r"(?i)(?<![\w.-])([\w.-]*(?:pass|secret|token|cred|apikey|api_key|private|pwd)[\w.-]*=)(?!=|process\.env)"
                       r"[^\s&\"',;`]+")
KEY_SECRET = re.compile(r"(?i)(?<![\w.-])([\w.-]*(?:_|-)?key(?:[_-][\w.-]*)?\s*=(?!=)\s*)(?!process\.env|<hidden>)[^\s&\"',;`]+")
ENV_SECRET = re.compile(r"(?im)^(\s*(?:export\s+|Environment=\"?)?[\w.-]*(?:pass|secret|token|cred|apikey|api_key|private|pwd)"
                        r"[\w.-]*\s*=\s*).+$")
AUTH_SECRET = re.compile(r"(?i)(\b(?:authorization|x-api-key|x-auth-token)\b['\"]?\s*[:=,]\s*['\"]?(?:bearer\s+|basic\s+|token\s+)?)"
                         r"[A-Za-z0-9._~+/=-]{8,}")
PWD_ARG = re.compile(r"(?i)(--[\w-]*(?:pass|secret|token|pwd|key)[\w-]*[= ])\S+")


def mask(text):
    text = URL_SECRET.sub(r"\1<hidden>", text)
    text = JSON_SECRET.sub(r"\1<hidden>\2", text)
    text = AUTH_SECRET.sub(r"\1<hidden>", text)
    text = ENV_SECRET.sub(r"\1<hidden>", text)
    text = KV_SECRET.sub(r"\1<hidden>", text)
    text = KEY_SECRET.sub(r"\1<hidden>", text)
    text = PWD_ARG.sub(r"\1<hidden>", text)
    return text


def say(text="", limit=LINE_MAX):
    for line in str(text).splitlines() or [""]:
        print("TV " + mask(line)[:limit], flush=True)


def run(cmd, timeout=20, env=None):
    """(exit code, output): never raises; a command that is not there is exit 127, one that hangs is 124."""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env)
    except FileNotFoundError:
        return 127, f"{cmd[0]}: not installed"
    except subprocess.TimeoutExpired:
        return 124, f"(no answer in {timeout} s)"
    except OSError as e:
        return 126, str(e)
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


def dexec(c, argv, timeout=20, env=None):
    cmd = ["docker", "exec"]
    for k, v in (env or {}).items():
        cmd += ["-e", f"{k}={v}"]
    return run(cmd + [c] + list(argv), timeout=timeout)


def dsh(c, script, timeout=20, env=None):
    return dexec(c, ["sh", "-c", script], timeout=timeout, env=env)


def path(p):
    return ROOT + p


def sha_file(p):
    try:
        with open(p, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return ""


def read_bytes(p):
    try:
        with open(p, "rb") as f:
            return f.read()
    except OSError:
        return None


def lf(data):
    return data.replace(b"\r\n", b"\n")


def eol_of(data):
    crlf, n = data.count(b"\r\n"), data.count(b"\n")
    return "CRLF" if n and crlf == n else "LF" if not crlf else f"mixed ({crlf} of {n} CRLF)"


def room_version(data):
    """"old" (the version this fix was made for), "new" (this fix's), or "unknown" -- line endings aside."""
    if data is None:
        return "unreadable"
    sha = hashlib.sha256(lf(data)).hexdigest()
    return "old" if sha == ROOM_OLD_SHA else "new" if sha == ROOM_NEW_SHA else "unknown"


def room_diff(data, keep=60):
    """What differs from the version this fix was made for (its copy sent along as server-orig.mjs), secrets hidden."""
    import difflib
    orig = read_bytes(os.path.join(W, "server-orig.mjs"))
    if orig is None or data is None:
        return ["(no copy of the version this fix was made for to compare with)"]
    a = lf(orig).decode("utf-8", "replace").splitlines()
    b = lf(data).decode("utf-8", "replace").splitlines()
    out = list(difflib.unified_diff(a, b, "made for", "on this machine", n=1, lineterm=""))
    return out[:keep] + ([f"... ({len(out) - keep} more diff lines)"] if len(out) > keep else [])


def kept_copies(prefix, must_hold):
    """The repair's copies (BACKUPS/<prefix>-<time>/ holding `must_hold`), oldest first by when each was made: its
    README.txt's time (the copies inside keep their own files' times)."""
    base = path(BACKUPS)
    found = []
    for d in (os.listdir(base) if os.path.isdir(base) else []):
        full = os.path.join(base, d)
        if re.fullmatch(re.escape(prefix) + r"-[\d-]+", d) and os.path.isfile(os.path.join(full, must_hold)) \
                and os.path.isfile(os.path.join(full, "README.txt")):
            found.append((os.stat(os.path.join(full, "README.txt")).st_mtime, d))
    return [d for _, d in sorted(found)]


def arg(name, default=""):
    return ARGS[ARGS.index(name) + 1] if name in ARGS and ARGS.index(name) + 1 < len(ARGS) else default


def unit_props(units, props):
    """systemctl show for several units: {Id: {prop: value}}."""
    code, out = run(["systemctl", "show"] + list(units) + ["-p", "Id," + ",".join(props)], timeout=30)
    found = {}
    for block in out.split("\n\n"):
        p = dict(ln.split("=", 1) for ln in block.splitlines() if "=" in ln)
        if p.get("Id"):
            found[p["Id"]] = p
    return found


def journal(unit, since, n=40):
    code, out = run(["journalctl", "-u", unit, "--since", since, "--no-pager", "-o", "short-iso", "-n", str(n)], timeout=30)
    return [ln for ln in out.splitlines() if ln.strip() and not ln.startswith("-- ")]


def folded(lines, keep=24):
    """Repeated lines once each, with how many times (the journal's spam made readable)."""
    seen, order = {}, []
    for ln in lines:
        body = re.sub(r"\[\d+\]", "[N]", re.sub(r"^\S+T\S+\s+", "", ln)).strip()
        if body not in seen:
            seen[body] = 0
            order.append(body)
        seen[body] += 1
    return [(f"{seen[b]}x " if seen[b] > 1 else "") + b for b in order[-keep:]]


# -- one GET, how long to connect and to the first byte --------------------------------------------------------------

def probe(host, port, url_path, timeout=8.0, keep=0, method="GET", body=None):
    r = {"target": "%s:%d%s" % (host, port, url_path)}
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
        head = ("%s %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: open77-tvkit-repair\r\nConnection: close\r\n"
                % (method, url_path, host, port))
        if body is not None:
            head += "Content-Type: application/json\r\nContent-Length: %d\r\n" % len(body)
        s.sendall((head + "\r\n").encode() + (body or b""))
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
    head, _, rest = data.partition(b"\r\n\r\n")
    r["status"] = head.split(b"\r\n", 1)[0].decode("latin-1")[:80]
    r["code"] = int(r["status"].split(" ")[1]) if len(r["status"].split(" ")) > 1 and r["status"].split(" ")[1].isdigit() else 0
    r["bytes"] = len(rest)
    if keep:
        r["body"] = rest[:keep].decode("utf-8", "replace")
    return r


def probe_text(r):
    if "connect" in r:
        return f"{r['target']}: cannot connect ({r['connect']})"
    if "answer" in r:
        return f"{r['target']}: connected in {r['connect_ms']} ms, {r['answer']}"
    return (f"{r['target']}: {r.get('status', '?')} after {r['connect_ms'] + r['first_byte_ms']} ms, {r.get('bytes', 0)} bytes")


def took(r):
    return r.get("connect_ms", 0) + r.get("first_byte_ms", 0) if r.get("code") else None


# =================================================================================================================
# the room service
# =================================================================================================================

def room_requests(timeout=8.0):
    """What the televisions ask, side by side: the home page, every room's state, and the status."""
    specs = [("GET /", "/?room=production", 0)] + [(f"state {r}", f"/api/state?room={r}", 262144) for r in ROOMS] + \
            [("status", "/api/status", 65536)]
    results = {}

    def one(label, url_path, keep):
        results[label] = probe("127.0.0.1", ROOM_PORT, url_path, timeout=timeout, keep=keep)
    threads = [threading.Thread(target=one, args=s, daemon=True) for s in specs]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout + 8)
    return {label: results.get(label, {"target": label, "answer": "the probe did not finish"}) for label, _, _ in specs}


def room_round_text(res):
    good = [k for k, r in res.items() if r.get("code") == 200]
    slow = max([took(r) for r in res.values() if took(r) is not None] or [0])
    return good, slow


def room_status(res):
    try:
        return json.loads(res["status"].get("body") or "{}")
    except ValueError:
        return {}


def room_look():
    say("===== the room service (open77-room: the home screen's server, port %d)" % ROOM_PORT)
    p = unit_props([ROOM_UNIT], ["ActiveState", "SubState", "MainPID", "NRestarts", "ActiveEnterTimestamp", "ExecStart",
                                 "Environment"]).get(ROOM_UNIT, {})
    data = read_bytes(path(ROOM_JS))
    sha = hashlib.sha256(data).hexdigest() if data is not None else ""
    ver = room_version(data)
    which = {"old": "the version this fix was made for (it waits on its own children)",
             "new": "this fix's version (it does not wait)", "unknown": "a version this fix does not know",
             "unreadable": "(cannot be read)"}[ver]
    say(f"  {ROOM_UNIT}: {p.get('ActiveState', '?')}/{p.get('SubState', '?')}, main pid {p.get('MainPID', '?')}, restarts "
        f"{p.get('NRestarts', '?')}, since {p.get('ActiveEnterTimestamp', '?')}")
    say(f"  {ROOM_JS}: {len(data) if data is not None else '?'} bytes, line endings {eol_of(data) if data else '?'}, "
        f"sha256 {sha[:16] or '-'} ({hashlib.sha256(lf(data)).hexdigest()[:16] if data else '-'} with LF): {which}")
    if ver == "unknown":
        say("  how it differs from the version this fix was made for:")
        for ln in room_diff(data):
            say("    " + ln)
    say(f"  node: {node_bin(p)} ({run([node_bin(p), '--version'], timeout=15)[1].strip()[:40]})")
    res = room_requests()
    good, slow = room_round_text(res)
    say(f"  what the televisions ask, side by side: {len(good)} of {len(res)} answered" +
        (f", the slowest after {slow} ms" if good else ""))
    for label, r in res.items():
        say(f"    {label}: " + probe_text(r))
    st = room_status(res)
    if st.get("rooms"):
        say("  its rooms: " + "; ".join(f"{k} {v.get('container')} running {v.get('running')} chromium "
                                         f"{str(v.get('chromium'))[:40]}" for k, v in st["rooms"].items()))
    return {"sha": sha[:16], "version": ver, "eol": eol_of(data) if data else "?", "answered": len(good), "asked": len(res)}


def node_bin(props=None):
    props = props if props is not None else unit_props([ROOM_UNIT], ["ExecStart"]).get(ROOM_UNIT, {})
    m = re.search(r"path=(\S+)", props.get("ExecStart", ""))
    return m.group(1) if m and os.path.basename(m.group(1)).startswith("node") else (shutil.which("node") or "node")


def room_env():
    p = unit_props([ROOM_UNIT], ["Environment"]).get(ROOM_UNIT, {})
    env = {}
    try:
        words = shlex.split(p.get("Environment", ""))
    except ValueError:
        words = p.get("Environment", "").split()
    for w in words:
        k, sep, v = w.partition("=")
        if sep:
            env[k] = v
    return env


def room_wait_answer(seconds=40):
    """The home page asked every second until it answers (or `seconds` pass)."""
    end = time.time() + seconds
    r = {}
    while time.time() < end:
        r = probe("127.0.0.1", ROOM_PORT, "/?room=production", timeout=3)
        if r.get("code") == 200:
            return r
        time.sleep(1)
    return r


def room_check(rounds=3, gap=5.0):
    """A few rounds of what the televisions ask, side by side: ok when every question gets a 200 (within the probe's
    8 s).  A slow answer (3 s or more) is said, but it is not a reason to go back to a server that answers nothing."""
    ok, worst, lines = True, 0, []
    for i in range(rounds):
        res = room_requests()
        good, slow = room_round_text(res)
        worst = max(worst, slow)
        bad = [f"{k}: {probe_text(r)}" for k, r in res.items() if r.get("code") != 200]
        late = [f"{k} after {took(r)} ms" for k, r in res.items() if r.get("code") == 200 and (took(r) or 0) >= 3000]
        lines.append(f"round {i + 1}: {len(good)} of {len(res)} answered, the slowest after {slow} ms" +
                     (f"; slow: {', '.join(late)}" if late else "") + (f"; not right: {'; '.join(bad)}" if bad else ""))
        if bad:
            ok = False
        if i + 1 < rounds:
            time.sleep(gap)
    return ok, worst, lines, res


def room_restart(why):
    t0 = time.strftime("%Y-%m-%d %H:%M:%S")
    code, out = run(["systemctl", "restart", ROOM_UNIT], timeout=120)
    say(f"  systemctl restart {ROOM_UNIT} ({why}): exit {code} {out.strip()[-160:]}")
    return t0


def room_action_check():
    """One "open" through the room service, the way a tile asks: for a room that already shows its own home page,
    that page again (its screen reloads).  A room showing anything else is left alone."""
    for role in ["staging"] + [r for r in ROOMS if r != "staging"]:
        r = probe("127.0.0.1", ROOM_PORT, f"/api/state?room={role}", keep=262144)
        try:
            st = json.loads(r.get("body") or "{}")
        except ValueError:
            continue
        url = st.get("roomUrl") or ""
        home = f"http://{GATEWAY}:{ROOM_PORT}/?room={role}"
        if url != home and not url.startswith(home + "&"):
            say(f"  {role}: shows {mask(url)[:90] or '(nothing it can read)'}: not its home page, left alone")
            continue
        body = json.dumps({"room": role, "action": "open", "url": url}).encode()
        a = probe("127.0.0.1", ROOM_PORT, "/api/action", timeout=30, keep=4096, method="POST", body=body)
        try:
            answer = json.loads(a.get("body") or "{}")
        except ValueError:
            answer = {}
        good = a.get("code") == 200 and answer.get("ok") is True
        say(f"  {role}: \"open\" its home page again (a tile's click): {probe_text(a)} -> "
            f"{json.dumps(answer)[:160] if answer else '(no JSON)'}")
        return {"room": role, "ok": good, "ms": took(a)}
    say("  no room shows its own home page right now: no click tried")
    return {"room": None, "ok": None}


def room_fix(ts):
    say("===== the room service: the server.mjs that does not wait")
    new = os.path.join(W, "server.mjs")
    cur = path(ROOM_JS)
    sha_new, sha_cur = sha_file(new), sha_file(cur)
    if sha_new != ROOM_NEW_SHA:
        say(f"  the new server.mjs did not arrive whole (sha256 {sha_new[:16] or 'unreadable'}): nothing changed")
        return {"result": "refused", "why": "upload"}
    cur_data = read_bytes(cur)
    ver = room_version(cur_data)
    eol = eol_of(cur_data) if cur_data else "?"
    installed, keep = False, ""
    if ver == "new":
        say(f"  {ROOM_JS} is already this fix's version: not written again")
        ok, worst, lines, res = room_check(rounds=1)
        for ln in lines:
            say("  " + ln)
        if ok:
            say("  it answers: not restarted")
            return {"result": "ok", "worst_ms": worst, "click": room_action_check()}
    elif ver != "old":
        say(f"  {ROOM_JS} is a version this fix was not made for (sha256 {sha_cur[:16] or 'unreadable'}, line endings "
            f"{eol}): nothing changed; how it differs:")
        for ln in room_diff(cur_data):
            say("    " + ln)
        return {"result": "refused", "why": "unknown version"}
    else:
        node = node_bin()
        data = read_bytes(new)
        if eol == "CRLF":
            data = data.replace(b"\n", b"\r\n")
        staged = os.path.join(W, "server-install.mjs")
        with open(staged, "wb") as f:
            f.write(data)
        say(f"  {ROOM_JS} is the version this fix was made for (line endings {eol}); the new one is written with the same")
        code, out = run([node, "--check", staged], timeout=60)
        say(f"  {node} --check (the new file): exit {code} {out.strip()[-200:]}")
        if code != 0:
            say("  the new file does not pass node's check: nothing changed")
            return {"result": "refused", "why": "node --check"}
        keep = path(f"{BACKUPS}/open77-room-{ts}")
        os.makedirs(keep, exist_ok=False)
        shutil.copy2(cur, os.path.join(keep, "server.mjs"))
        with open(os.path.join(keep, "README.txt"), "w") as f:
            f.write(f"{ROOM_JS} as it was before the TV kit's repair at {ts} (sha256 {sha_cur}); the repair wrote "
                    f"sha256 {ROOM_NEW_SHA}.  T5b (tv_kit.py repair-putback --room) puts this back.\n")
        if sha_file(os.path.join(keep, "server.mjs")) != sha_cur:
            say(f"  the copy in {keep} is not whole: nothing changed")
            return {"result": "refused", "why": "backup"}
        say(f"  kept first: {keep}/server.mjs")
        st = os.stat(cur)
        tmp = cur + f".new-{ts}"
        shutil.copyfile(staged, tmp)
        os.chmod(tmp, st.st_mode & 0o7777)
        try:
            os.chown(tmp, st.st_uid, st.st_gid)
        except PermissionError:
            pass
        os.replace(tmp, cur)
        installed = room_version(read_bytes(cur)) == "new"
        say(f"  written: {ROOM_JS} (sha256 {sha_file(cur)[:16]}, line endings {eol_of(read_bytes(cur))}, mode "
            f"{oct(st.st_mode & 0o7777)}, owner {st.st_uid}:{st.st_gid})" + ("" if installed else ": NOT what was meant"))
    since = room_restart("to run the new file" if installed else "it does not answer")
    first = room_wait_answer(40)
    say("  after the restart: " + probe_text(first))
    ok, worst, lines, res = room_check() if first.get("code") == 200 else (False, 0, ["it never answered"], {})
    for ln in lines:
        say("  " + ln)
    if not ok and installed:
        say("  it does not answer the way it must: put back")
        shutil.copy2(os.path.join(keep, "server.mjs"), cur + f".back-{ts}")
        os.replace(cur + f".back-{ts}", cur)
        say(f"  put back: {ROOM_JS} sha256 {sha_file(cur)[:16]} (the one kept in {keep})")
        room_restart("the old file again")
        say("  after that: " + probe_text(room_wait_answer(20)))
        for ln in folded(journal(ROOM_UNIT, since, 80), keep=30):
            say("    journal: " + ln[:260])
        return {"result": "put back", "worst_ms": worst}
    st = room_status(res)
    if st.get("rooms"):
        say("  its rooms: " + "; ".join(f"{k} running {v.get('running')}, chromium {str(v.get('chromium'))[:40]}"
                                         for k, v in st["rooms"].items()))
    click = room_action_check() if ok else {"room": None, "ok": None}
    time.sleep(2)
    say(f"  its journal since the restart:")
    for ln in folded(journal(ROOM_UNIT, since, 80), keep=30):
        say("    " + ln[:260])
    good = ok and click.get("ok") is not False
    return {"result": "fixed" if good and installed else "ok" if good else "not right", "worst_ms": worst,
            "click": click, "kept": keep}


def room_putback():
    say("===== the room service put back")
    cur = path(ROOM_JS)
    kept = kept_copies("open77-room", "server.mjs")
    if not kept:
        say("  no copy kept by the repair: nothing to put back")
        return {"result": "nothing"}
    src = os.path.join(path(BACKUPS), kept[-1], "server.mjs")
    if sha_file(cur) == sha_file(src):
        say(f"  {ROOM_JS} is already the copy kept in {kept[-1]}: nothing to put back")
        return {"result": "nothing"}
    if room_version(read_bytes(cur)) != "new":
        say(f"  {ROOM_JS} is not what the repair wrote (sha256 {sha_file(cur)[:16]}): something else changed it; "
            f"not touched")
        return {"result": "refused"}
    ts = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    shutil.copy2(src, cur + f".back-{ts}")
    os.replace(cur + f".back-{ts}", cur)
    say(f"  put back from {kept[-1]}: sha256 {sha_file(cur)[:16]}")
    room_restart("the old file again")
    say("  after the restart: " + probe_text(room_wait_answer(20)))
    return {"result": "put back"}


# =================================================================================================================
# Android (Waydroid)
# =================================================================================================================

def wd_cfg():
    """(text, the [waydroid] section's values) of waydroid.cfg, (None, {}) when it cannot be read."""
    try:
        text = open(path(WD_CFG), encoding="utf-8").read()
    except OSError:
        return None, {}
    vals, section = {}, ""
    for ln in text.splitlines():
        s = ln.strip()
        m = re.match(r"^\[(.+)\]$", s)
        if m:
            section = m.group(1)
            continue
        m = re.match(r"^([\w.-]+)\s*=\s*(.*)$", s)
        if m and section == "waydroid":
            vals[m.group(1)] = m.group(2).strip()
    return text, vals


def wd_cfg_written(text, changes):
    out, section, done = [], "", set()
    for ln in text.splitlines(keepends=True):
        body = ln.rstrip("\r\n")
        m = re.match(r"^\s*\[(.+)\]\s*$", body)
        if m:
            section = m.group(1)
        m = re.match(r"^(\s*)([\w.-]+)(\s*=\s*)(.*?)(\s*)$", body)
        if section == "waydroid" and m and m.group(2) in changes:
            ln = m.group(1) + m.group(2) + m.group(3) + changes[m.group(2)][1] + ln[len(body):]
            done.add(m.group(2))
        out.append(ln)
    if done != set(changes):
        raise RuntimeError(f"waydroid.cfg: {sorted(set(changes) - done)} not found")
    return "".join(out)


NODE_LINE = re.compile(r"^(\s*lxc\.mount\.entry\s*=\s*)(\S+)(\s+dev/(binder|vndbinder|hwbinder)\s.*)$")


def wd_nodes_written(text, changes):
    out, done = [], set()
    for ln in text.splitlines(keepends=True):
        body = ln.rstrip("\r\n")
        m = NODE_LINE.match(body)
        if m and m.group(4) in changes and m.group(2) == "/dev/" + changes[m.group(4)][0]:
            ln = m.group(1) + "/dev/" + changes[m.group(4)][1] + m.group(3) + ln[len(body):]
            done.add(m.group(4))
        out.append(ln)
    return "".join(out), done


def node_state(name):
    p = path("/dev/" + name)
    if os.path.islink(p):
        target = os.readlink(p)
        return f"/dev/{name} -> {target}" + ("" if os.path.exists(p) else " (missing)")
    if os.path.exists(p):
        return f"/dev/{name}"
    return f"/dev/{name}: none"


def binderfs():
    try:
        return sorted(os.listdir(path("/dev/binderfs")))
    except OSError:
        return []


def wd_plan():
    """What the fix would change: {key: (from, to)}, or {"error": why}."""
    text, vals = wd_cfg()
    if text is None:
        return {"error": f"{WD_CFG} cannot be read"}
    fs_nodes = binderfs()
    changes = {}
    for key, anbox in BINDERS:
        cur = vals.get(key)
        if not cur:
            return {"error": f"waydroid.cfg has no {key} line"}
        if os.path.exists(path("/dev/" + cur)):
            continue
        if cur == anbox:
            if anbox in fs_nodes:
                continue          # Waydroid links it from its binderfs when its container starts
            return {"error": f"waydroid.cfg names {cur}, which is neither in /dev nor in /dev/binderfs"}
        if os.path.exists(path("/dev/" + anbox)) or anbox in fs_nodes:
            changes[key] = (cur, anbox)
        else:
            return {"error": f"/dev/{cur} is missing, and there is no {anbox} to use instead"}
    return {"changes": changes}


def wd_status():
    code, out = run(["waydroid", "status"], timeout=30)
    d = {}
    for ln in out.splitlines():
        k, sep, v = ln.partition(":")
        if sep:
            d[k.strip()] = v.strip()
    return d, " | ".join(ln.strip() for ln in out.splitlines() if ln.strip())


def adb_device(udid):
    code, out = run(["adb", "devices"], timeout=20)
    for ln in out.splitlines()[1:]:
        f = ln.split()
        if len(f) >= 2 and f[0] == udid:
            return f[1]
    return ""


def wd_look():
    say("===== Android (Waydroid)")
    code, ver = run(["waydroid", "--version"], timeout=20)
    code2, pkg = run(["dpkg-query", "-W", "-f", "${Version}", "waydroid"], timeout=20)
    say(f"  waydroid {ver.strip()[:40]} (package {pkg.strip()[:40] if code2 == 0 else '?'})")
    st, line = wd_status()
    say("  waydroid status: " + line)
    for u, p in unit_props(WD_UNITS, ["LoadState", "ActiveState", "SubState", "NRestarts", "ActiveEnterTimestamp"]).items():
        if p.get("LoadState") == "not-found":
            continue
        say(f"  {u}: {p.get('ActiveState')}/{p.get('SubState')}, restarts {p.get('NRestarts')}, since {p.get('ActiveEnterTimestamp')}")
    text, vals = wd_cfg()
    say("  waydroid.cfg: " + ("; ".join(f"{k} = {vals.get(k, '(none)')}" for k in ("binder", "vndbinder", "hwbinder",
                                                                             "binder_protocol", "service_manager_protocol",
                                                                             "vendor_type", "images_path"))
                               if text is not None else "(cannot be read)"))
    say("  the nodes: " + "; ".join(node_state(n) for n in ("binder", "vndbinder", "hwbinder", "anbox-binder",
                                                            "anbox-vndbinder", "anbox-hwbinder")))
    code, out = run(["findmnt", "-n", "-o", "FSTYPE,SOURCE", path("/dev/binderfs")], timeout=15)
    say(f"  /dev/binderfs: {out.strip() or 'not mounted'}; holds {', '.join(binderfs()) or 'nothing'}")
    try:
        nodes = [ln.strip() for ln in open(path(WD_NODES), encoding="utf-8").read().splitlines() if NODE_LINE.match(ln)]
    except OSError:
        nodes = ["(cannot be read)"]
    say(f"  {WD_NODES}: " + " | ".join(nodes))
    udid = room_env().get("ANDROID_UDID", "192.168.240.112:5555")
    say(f"  adb sees {udid}: {adb_device(udid) or 'no'}")
    plan = wd_plan()
    if "error" in plan:
        say(f"  the fix: cannot ({plan['error']})")
    elif plan["changes"]:
        say("  the fix: " + ", ".join(f"{k} {a} -> {b}" for k, (a, b) in plan["changes"].items()) +
            " in waydroid.cfg and config_nodes, then the container service and the session restarted")
    else:
        say("  the fix: nothing to change in Waydroid's binder settings")
    return {"container": st.get("Container", "?"), "session": st.get("Session", "?"),
            "plan": plan.get("error") or {k: v[1] for k, v in plan.get("changes", {}).items()}}


def wd_restart(why):
    for cmd in (["systemctl", "stop", WD_SESSION], ["systemctl", "restart", WD_UNIT], ["systemctl", "start", WD_SESSION]):
        code, out = run(cmd, timeout=120)
        say(f"  {' '.join(cmd)} ({why}): exit {code} {out.strip()[-160:]}")


def wd_wait_container(seconds):
    end = time.time() + seconds
    t0 = time.time()
    st, line = wd_status()
    while time.time() < end:
        st, line = wd_status()
        if st.get("Container") == "RUNNING":
            return True, line, int(time.time() - t0)
        time.sleep(5)
    return False, line, int(time.time() - t0)


def wd_wait_android(udid, seconds):
    end = time.time() + seconds
    t0 = time.time()
    seen, boot = "", ""
    while time.time() < end:
        run(["adb", "connect", udid], timeout=15)
        seen = adb_device(udid)
        if seen == "device":
            code, out = run(["adb", "-s", udid, "shell", "getprop", "sys.boot_completed"], timeout=15)
            boot = out.strip()
            if boot == "1":
                return True, seen, boot, int(time.time() - t0)
        time.sleep(5)
    return False, seen, boot, int(time.time() - t0)


def wd_fix(ts):
    say("===== Android: Waydroid pointed at the binder nodes this machine has")
    st, line = wd_status()
    say("  before: " + line)
    plan = wd_plan()
    if "error" in plan:
        say(f"  cannot: {plan['error']}; nothing changed")
        return {"result": "refused", "why": plan["error"]}
    changes = plan["changes"]
    if not changes:
        say("  waydroid.cfg already names nodes that exist: nothing to change")
        return {"result": "nothing", "container": st.get("Container", "?")}
    text, vals = wd_cfg()
    try:
        nodes_text = open(path(WD_NODES), encoding="utf-8").read()
    except OSError as e:
        say(f"  {WD_NODES} cannot be read ({e}): nothing changed")
        return {"result": "refused", "why": "config_nodes"}
    new_cfg = wd_cfg_written(text, changes)
    new_nodes, node_done = wd_nodes_written(nodes_text, changes)
    keep = path(f"{BACKUPS}/waydroid-{ts}")
    os.makedirs(keep, exist_ok=False)
    shutil.copy2(path(WD_CFG), os.path.join(keep, "waydroid.cfg"))
    shutil.copy2(path(WD_NODES), os.path.join(keep, "config_nodes"))
    with open(os.path.join(keep, "README.txt"), "w") as f:
        f.write(f"{WD_CFG} and {WD_NODES} as they were before the TV kit's repair at {ts}; the repair changed "
                + ", ".join(f"{k} {a} -> {b}" for k, (a, b) in changes.items())
                + ".  T5b (tv_kit.py repair-putback --android) puts these back.\n")
    say(f"  kept first: {keep}/waydroid.cfg and config_nodes")
    try:
        for p_, data in ((path(WD_CFG), new_cfg), (path(WD_NODES), new_nodes)):
            st_ = os.stat(p_)
            tmp = p_ + f".new-{ts}"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(data)
            os.chmod(tmp, st_.st_mode & 0o7777)
            try:
                os.chown(tmp, st_.st_uid, st_.st_gid)
            except PermissionError:
                pass
            os.replace(tmp, p_)
    except OSError as e:
        say(f"  writing failed ({e}): both files put back as they were kept")
        for name, p_ in (("waydroid.cfg", path(WD_CFG)), ("config_nodes", path(WD_NODES))):
            shutil.copy2(os.path.join(keep, name), p_)
        return {"result": "put back", "why": str(e)}
    say("  waydroid.cfg now: " + ", ".join(f"{k} = {b}" for k, (a, b) in changes.items()))
    say(f"  config_nodes now: " + (" | ".join(ln.strip() for ln in new_nodes.splitlines() if NODE_LINE.match(ln))
                                  if node_done else "(it had no line naming the old nodes; unchanged)"))
    since = time.strftime("%Y-%m-%d %H:%M:%S")
    wd_restart("to read the new names")
    up, line, waited = wd_wait_container(WAIT_CONTAINER)
    say(f"  after {waited} s: " + line)
    say("  the nodes: " + "; ".join(node_state(b) for _, b in BINDERS))
    if not up:
        say("  the container did not start: what Waydroid said meanwhile")
        for u in (WD_UNIT, WD_SESSION):
            for ln in folded(journal(u, since, 200), keep=14):
                say(f"    {u}: " + ln[:260])
        say("  put back: waydroid.cfg and config_nodes as they were, and the services restarted")
        for name, p_ in (("waydroid.cfg", path(WD_CFG)), ("config_nodes", path(WD_NODES))):
            shutil.copy2(os.path.join(keep, name), p_ + f".back-{ts}")
            os.replace(p_ + f".back-{ts}", p_)
        wd_restart("the old names again")
        return {"result": "put back", "waited": waited}
    udid = room_env().get("ANDROID_UDID", "192.168.240.112:5555")
    ready, seen, boot, waited2 = wd_wait_android(udid, WAIT_ANDROID)
    say(f"  adb {udid}: {seen or 'not seen'}, boot completed {boot or '?'} (after {waited2} s more)")
    for u in (WD_UNIT, WD_SESSION):
        for ln in folded(journal(u, since, 120), keep=8):
            say(f"    {u}: " + ln[:260])
    probe_novnc = probe("127.0.0.1", int(room_env().get("ANDROID_PORT", "8001")), "/", timeout=5)
    say("  the Android screen's page: " + probe_text(probe_novnc))
    return {"result": "fixed" if ready else "running, Android not ready yet", "container": "RUNNING",
            "adb": seen, "boot_completed": boot, "kept": keep}


def wd_putback():
    say("===== Android: Waydroid's files put back")
    kept = kept_copies("waydroid", "waydroid.cfg")
    if not kept:
        say("  no copy kept by the repair: nothing to put back")
        return {"result": "nothing"}
    d = os.path.join(path(BACKUPS), kept[-1])
    text, vals = wd_cfg()
    if text is not None and sha_file(path(WD_CFG)) == sha_file(os.path.join(d, "waydroid.cfg")):
        say(f"  waydroid.cfg is already the copy kept in {kept[-1]}: nothing to put back")
        return {"result": "nothing"}
    if any(vals.get(k) != b for k, b in BINDERS if k in vals):
        say("  waydroid.cfg's binder lines are not what the repair wrote (" +
            ", ".join(f"{k} = {vals.get(k)}" for k, _ in BINDERS) + "): something else changed it; not touched")
        return {"result": "refused"}
    ts = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
    for name, p_ in (("waydroid.cfg", path(WD_CFG)), ("config_nodes", path(WD_NODES))):
        shutil.copy2(os.path.join(d, name), p_ + f".back-{ts}")
        os.replace(p_ + f".back-{ts}", p_)
    text, vals = wd_cfg()
    say(f"  put back from {kept[-1]}: " + ", ".join(f"{k} = {vals.get(k)}" for k, _ in BINDERS))
    wd_restart("the old names again")
    return {"result": "put back"}


# =================================================================================================================
# the televisions' windows
# =================================================================================================================

WINDOW_SRC = r'''
import ctypes, ctypes.util, json, sys, time
mode, display = sys.argv[1], sys.argv[2]
x = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
VP, UL = ctypes.c_void_p, ctypes.c_ulong
x.XOpenDisplay.restype = VP
x.XOpenDisplay.argtypes = [ctypes.c_char_p]
ERR = ctypes.CFUNCTYPE(ctypes.c_int, VP, VP)
_keep = ERR(lambda d, e: 0)
x.XSetErrorHandler.argtypes = [ERR]
x.XSetErrorHandler(_keep)
dpy = x.XOpenDisplay(display.encode())
if not dpy:
    print(json.dumps({"error": "cannot open display " + display}))
    sys.exit(2)
x.XDefaultScreen.argtypes = [VP]
x.XRootWindow.argtypes = [VP, ctypes.c_int]
x.XRootWindow.restype = UL
x.XDisplayWidth.argtypes = [VP, ctypes.c_int]
x.XDisplayHeight.argtypes = [VP, ctypes.c_int]
x.XInternAtom.argtypes = [VP, ctypes.c_char_p, ctypes.c_int]
x.XInternAtom.restype = UL
x.XGetAtomName.argtypes = [VP, UL]
x.XGetAtomName.restype = VP
x.XFree.argtypes = [VP]
x.XGetWindowProperty.argtypes = [VP, UL, UL, ctypes.c_long, ctypes.c_long, ctypes.c_int, UL, ctypes.POINTER(UL),
                                 ctypes.POINTER(ctypes.c_int), ctypes.POINTER(UL), ctypes.POINTER(UL), ctypes.POINTER(VP)]
x.XGetGeometry.argtypes = [VP, UL, ctypes.POINTER(UL), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                           ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                           ctypes.POINTER(ctypes.c_uint)]
x.XTranslateCoordinates.argtypes = [VP, UL, UL, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int),
                                    ctypes.POINTER(ctypes.c_int), ctypes.POINTER(UL)]
x.XSendEvent.argtypes = [VP, UL, ctypes.c_int, ctypes.c_long, VP]
x.XSync.argtypes = [VP, ctypes.c_int]
scr = x.XDefaultScreen(dpy)
root = x.XRootWindow(dpy, scr)
SW, SH = x.XDisplayWidth(dpy, scr), x.XDisplayHeight(dpy, scr)
def atom(name):
    return x.XInternAtom(dpy, name.encode(), 0)
def prop(win, name, kind, longs=1024):
    t, f, n, after, data = UL(), ctypes.c_int(), UL(), UL(), VP()
    if x.XGetWindowProperty(dpy, win, atom(name), 0, longs, 0, kind, ctypes.byref(t), ctypes.byref(f), ctypes.byref(n),
                            ctypes.byref(after), ctypes.byref(data)) != 0 or not data.value:
        return None, 0, 0
    return data, f.value, n.value
def ids(win, name, kind):
    data, fmt, n = prop(win, name, kind)
    if not data:
        return []
    out = list(ctypes.cast(data, ctypes.POINTER(UL))[:n]) if fmt == 32 else []
    x.XFree(data)
    return out
def text(win, name, kind):
    data, fmt, n = prop(win, name, kind)
    if not data:
        return ""
    raw = ctypes.string_at(data, n) if fmt == 8 else b""
    x.XFree(data)
    return raw.decode("utf-8", "replace")
def atom_name(a):
    p = x.XGetAtomName(dpy, a)
    if not p:
        return str(a)
    s = ctypes.string_at(p).decode()
    x.XFree(p)
    return s
def windows():
    out = []
    for w in ids(root, "_NET_CLIENT_LIST", 33):
        r, gx, gy, gw, gh, b, d = UL(), ctypes.c_int(), ctypes.c_int(), ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
        if not x.XGetGeometry(dpy, w, ctypes.byref(r), ctypes.byref(gx), ctypes.byref(gy), ctypes.byref(gw), ctypes.byref(gh),
                              ctypes.byref(b), ctypes.byref(d)):
            continue
        ax, ay, child = ctypes.c_int(), ctypes.c_int(), UL()
        x.XTranslateCoordinates(dpy, w, root, 0, 0, ctypes.byref(ax), ctypes.byref(ay), ctypes.byref(child))
        cls = [c for c in text(w, "WM_CLASS", 31).split("\0") if c]
        name = text(w, "_NET_WM_NAME", atom("UTF8_STRING")) or text(w, "WM_NAME", 31)
        states = [atom_name(a).replace("_NET_WM_STATE_", "") for a in ids(w, "_NET_WM_STATE", 4)]
        out.append({"id": hex(w), "class": cls[-1] if cls else "", "name": name[:80], "x": ax.value, "y": ay.value,
                    "w": gw.value, "h": gh.value, "states": states,
                    "fills": ax.value == 0 and ay.value == 0 and gw.value == SW and gh.value == SH})
    return out
class XClientMessageEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", UL), ("send_event", ctypes.c_int), ("display", VP), ("window", UL),
                ("message_type", UL), ("format", ctypes.c_int), ("l", ctypes.c_long * 5)]
class XEvent(ctypes.Union):
    _fields_ = [("xclient", XClientMessageEvent), ("pad", ctypes.c_long * 24)]
def fullscreen(w):
    ev = XEvent()
    ev.xclient.type = 33
    ev.xclient.send_event = 1
    ev.xclient.window = w
    ev.xclient.message_type = atom("_NET_WM_STATE")
    ev.xclient.format = 32
    ev.xclient.l[0] = 1
    ev.xclient.l[1] = atom("_NET_WM_STATE_FULLSCREEN")
    ev.xclient.l[2] = 0
    ev.xclient.l[3] = 1
    ev.xclient.l[4] = 0
    x.XSendEvent(dpy, root, 0, (1 << 20) | (1 << 19), ctypes.byref(ev))
    x.XSync(dpy, 0)
wm = ids(root, "_NET_SUPPORTING_WM_CHECK", 33)
wm_name = text(wm[0], "_NET_WM_NAME", atom("UTF8_STRING")) if wm else ""
before = windows()
result = {"screen": [SW, SH], "wm": wm_name, "windows": before}
if mode == "fullscreen":
    want = [w for w in before if "chrom" in w["class"].lower() and not w["fills"]]
    for w in want:
        fullscreen(int(w["id"], 16))
    result["asked"] = [w["id"] for w in want]
    if want:
        for _ in range(24):
            time.sleep(0.25)
            if all(a["fills"] for a in windows() if a["id"] in result["asked"]):
                break
        result["windows"] = windows()
print(json.dumps(result))
'''


def tv_containers():
    code, out = run(["docker", "ps", "--format", "{{.Names}}", "--filter", "name=open77-tvbrowser"], timeout=30)
    return sorted({n.strip() for n in out.splitlines() if n.strip().startswith("open77-tvbrowser")})


def display_of(c):
    code, out = run(["docker", "inspect", "--format", "{{json .Config.Env}}", c], timeout=15)
    try:
        env = dict(e.split("=", 1) for e in json.loads(out) if "=" in e)
    except (ValueError, TypeError):
        env = {}
    return env.get("DISPLAY", ":99.0")


def win_list(c, mode="list"):
    code, out = dexec(c, ["python3", "-c", WINDOW_SRC, mode, display_of(c)], timeout=40)
    try:
        return json.loads(out.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"error": out.strip()[-160:] or f"exit {code}"}


def win_text(w):
    return (f"{w['id']} {w['class']} {w['name'][:40]!r}: {w['w']}x{w['h']}+{w['x']}+{w['y']}, state "
            f"{','.join(w['states']) or 'none'}" + (" (fills the screen)" if w["fills"] else ""))


def win_look():
    say("===== the televisions' windows")
    found = {}
    for c in tv_containers():
        r = win_list(c)
        if "error" in r:
            say(f"  {c}: X could not be read ({r['error']})")
            found[c] = "?"
            continue
        say(f"  {c}: screen {r['screen'][0]}x{r['screen'][1]}, window manager {r['wm'] or '(none)'}")
        for w in r["windows"]:
            say("    " + win_text(w))
        chrom = [w for w in r["windows"] if "chrom" in w["class"].lower()]
        found[c] = "fills" if chrom and all(w["fills"] for w in chrom) else "small" if chrom else "no window"
        code, out = dsh(c, "for p in $(pgrep -x openbox 2>/dev/null); do tr '\\0' ' ' < /proc/$p/cmdline; echo; done | head -2",
                        timeout=15)
        cmdline = out.strip().splitlines()[0] if out.strip() else ""
        m = re.search(r"--config-file[= ](\S+)", cmdline)
        rc = m.group(1) if m else ""
        if not rc:
            code, o2 = dexec(c, ["sh", "-c", "[ -f /home/neko/.config/openbox/rc.xml ] && echo /home/neko/.config/openbox/rc.xml "
                                             "|| echo /etc/xdg/openbox/rc.xml"], timeout=15)
            rc = o2.strip() or "/etc/xdg/openbox/rc.xml"
        code, xml = dexec(c, ["cat", rc], timeout=15)
        xml = re.sub(r"(?s)<!--.*?-->", "", xml if code == 0 else "")
        root_ctx = re.search(r'(?s)<context name="Root">(.*?)</context>', xml)
        binds = re.findall(r'<mousebind button="(\w+)" action="(\w+)">\s*<action name="(\w+)"(?:>\s*<menu>([\w-]+)</menu>)?',
                           root_ctx.group(1)) if root_ctx else []
        apps = re.findall(r'<application ([^>]*)>', xml)
        say(f"    openbox: {cmdline[:120] or '(not running)'}; reads {rc}: a click on the desktop does "
            + (", ".join(f"{b} {a} -> {n}{(' ' + mm) if mm else ''}" for b, a, n, mm in binds) or "nothing")
            + (f"; window rules for {', '.join(apps)[:120]}" if apps else "; no window rules"))
        code, out = dsh(c, "for p in $(pgrep -f '/usr/lib/chromium/[c]hromium' 2>/dev/null); do a=$(tr '\\0' ' ' < /proc/$p/cmdline); "
                           "case \"$a\" in *--type=*) ;; *) echo \"$a\";; esac; done | head -1", timeout=15)
        if out.strip():
            flags = [f for f in out.split() if f.startswith("--")]
            say("    chromium's flags: " + " ".join(flags)[:300])
            say("    chromium's last words: " + " ".join(out.split()[-2:])[:200])
    return found


def win_fix():
    say("===== the televisions' windows: fullscreen, asked of openbox")
    done = {}
    for c in tv_containers():
        r = win_list(c, "fullscreen")
        if "error" in r:
            say(f"  {c}: X could not be read ({r['error']}): not changed")
            done[c] = "error"
            continue
        chrom = [w for w in r["windows"] if "chrom" in w["class"].lower()]
        if not r.get("asked"):
            say(f"  {c}: " + ("its Chromium window already fills the screen" if chrom else "no Chromium window") +
                " -- " + "; ".join(win_text(w) for w in chrom)[:220])
            done[c] = "fills" if chrom else "no window"
            continue
        ok = all(w["fills"] for w in r["windows"] if w["id"] in r["asked"])
        say(f"  {c}: asked for {', '.join(r['asked'])} -> " + "; ".join(win_text(w) for w in chrom)[:240])
        done[c] = "fullscreen now" if ok else "asked, not filled"
    return done


# =================================================================================================================

def parts():
    chosen = [p for p in ("room", "android", "windows") if f"--{p}" in ARGS]
    return chosen or ["room", "android", "windows"]


def main():
    if MODE not in ("look", "fix", "putback"):
        print(__doc__)
        return 2
    want = parts()
    result, rc = {"mode": MODE}, 0
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
        if "room" in want:
            result["room"] = part("room", room_look)
        if "android" in want:
            say("")
            result["android"] = part("android", wd_look)
        if "windows" in want:
            say("")
            result["windows"] = part("windows", win_look)
    elif MODE == "fix":
        if os.geteuid() != 0 and not ROOT:
            say("this needs root: nothing changed")
            return 2
        if "room" in want:
            result["room"] = part("room", room_fix, ts)
            rc |= 0 if result["room"]["result"] in ("fixed", "ok") else 1
        if "windows" in want:
            say("")
            result["windows"] = part("windows", win_fix)
            rc |= 0 if all(v in ("fills", "fullscreen now", "no window") for v in result["windows"].values()) else 1
        if "android" in want:
            say("")
            result["android"] = part("android", wd_fix, ts)
            rc |= 0 if result["android"]["result"] in ("fixed", "nothing", "running, Android not ready yet") else 1
    else:
        if "room" in want:
            result["room"] = part("room", room_putback)
        if "android" in want:
            say("")
            result["android"] = part("android", wd_putback)
        if "windows" in want and "--windows" in ARGS:
            say("")
            say("===== the televisions' windows: nothing to put back (fullscreen is not written anywhere; a Chromium "
                "restart decides the window again)")
        rc = 0 if all(v.get("result") in ("put back", "nothing") for k, v in result.items() if isinstance(v, dict)) else 1
    say("")
    say("result: " + json.dumps(result, separators=(",", ":")), limit=100000)
    return rc


if __name__ == "__main__":
    sys.exit(main())
