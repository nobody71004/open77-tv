#!/usr/bin/env python3
"""The TV room's own files as this machine runs them, copied exactly so they can go into the open77-tv repository
(2026-11-07) -- READ ONLY.  tv_kit.py sends this to a work folder of its own on the dedicated machine:

    python3 tv_sources.py collect WORKDIR

It copies, byte for byte, what makes the TV room here: every television's build folder and up.sh
(/opt/open77-tvbrowser*/, never its neko.env -- the passwords -- nor its profiles), the room service
(/opt/open77-room/: server.mjs, its pages and their font, README, package files; never node_modules), Android's
native folder (/opt/open77-android-native/: its compositor's and VNC's configs, its scripts), the systemd units of the
room service and of Android's native screen (with their drop-ins) and the scripts those units run from /opt or
/usr/local, the Android screen's page in noVNC's folder, and the nginx sites that route to the televisions or the
room.  A file is copied only when nothing in it looks like a secret (a password, token or key written out, a private
key): such a file is named, with the line it was stopped at, and left out.  Text is told by what a file holds (an
nginx site is named after its host); logs and the copies kept before an edit (.bak*, .orig*) are listed, not copied,
and so is every binary file but the pages' font.

WORKDIR/sources.tgz holds sources/<path on this machine> and sources/MANIFEST.json (path, size, sha256, copied or why
not).  The last line is "TV result: {...}".
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import tarfile
import time

MODE = sys.argv[1] if len(sys.argv) > 1 else ""
W = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else ".")
ROOT = os.environ.get("O77ROOT", "")                      # "" on the machine: the rehearsal's root of fake files
OUT = os.path.join(W, "sources")
LINE_MAX = 320
MAX_BYTES = 768 * 1024
TV_DIRS = ["/opt/open77-tvbrowser", "/opt/open77-tvbrowser-xb", "/opt/open77-tvbrowser-xbs", "/opt/open77-tvbrowser-f2077",
           "/opt/open77-tvbrowser-fr-us"]
ROOM_DIR = "/opt/open77-room"
ANDROID_DIR = "/opt/open77-android-native"
UNITS = ["open77-room.service", "open77-android-native-display.service", "open77-android-native-vnc.service",
         "open77-android-native-web.service", "open77-android-native-adb.service", "open77-waydroid-session.service",
         "open77-waydroid-display.service", "waydroid-container.service"]
SKIP_DIRS = {"node_modules", ".git", "profiles", "data", "__pycache__", "cache", "Cache"}
NEVER = re.compile(r"(?i)(^|/)(neko\.env[^/]*|\.env[^/]*|.*\.pem|.*\.key|.*\.p12|id_rsa.*|id_ed25519.*|.*secret.*|.*credential.*|"
                   r".*password.*|.*token.*)$")
# a secret written out: name = 'value' / name: "value" / NAME=value (a literal, not a $variable, $(command) or ${...})
SECRET_LINE = [
    re.compile(r"""(?i)\b[\w.-]*(pass(word|wd)?|secret|token|api[_-]?key|private[_-]?key|credential)[\w.-]*['"]?\s*[:=]\s*['"]"""
               r"""(?!\$|\{\{|<|%|process\.env)[^'"\s]{6,}['"]"""),
    re.compile(r"""(?i)^\s*(export\s+)?[A-Z0-9_]*(PASS(WORD)?|SECRET|TOKEN|API_?KEY|PRIVATE_?KEY|CREDENTIAL)[A-Z0-9_]*=(?!\$|"\$|'\$|$|\s)"""
               r"""[^\s#]{6,}"""),
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    re.compile(r"\b(AKIA[0-9A-Z]{16}|ghp_[0-9A-Za-z]{30,}|github_pat_[0-9A-Za-z_]{40,}|xox[baprs]-[0-9A-Za-z-]{10,}|sk-[A-Za-z0-9]{32,})\b"),
    re.compile(r"(?i)\b(pwd|password|token)=[A-Za-z0-9%._~+-]{8,}"),
]
TEXT_EXT = {".sh", ".mjs", ".js", ".cjs", ".json", ".md", ".txt", ".html", ".htm", ".css", ".conf", ".service", ".xml",
            ".yaml", ".yml", ".py", ".lua", ".ini", ".cfg", ""}
# never copied whatever they hold: logs and the copies kept before an edit (.bak, .bak-<when>, .orig-<when>, ~)
NOT_TEXT = re.compile(r"(?i)(\.log(\.\d+)?|\.bak[^/]*|\.orig[^/]*|\.old[^/]*|~|\.tvnative-off-[^/]*|\.swp)$")
# binary files that are part of what is collected: name -> the magic numbers it must start with
BINARY_OK = {"inter.woff2": (b"wOF2",)}


def is_text(p, data):
    """Text by what it holds, not only by its name: an nginx site is named after its host (xyberrp.duckdns.org), which
    an extension list called binary.  UTF-8 with no NUL and no log/backup name is text."""
    if NOT_TEXT.search(p) or b"\0" in data[:8192]:
        return False
    if os.path.splitext(p)[1].lower() in TEXT_EXT:
        return True
    try:
        data.decode("utf-8")
        return True
    except UnicodeDecodeError:
        return False


def path(p):
    return ROOT + p


def say(text="", limit=LINE_MAX):
    for line in str(text).splitlines() or [""]:
        print("TV " + line[:limit], flush=True)


def run(cmd, timeout=30):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, str(e)


MANIFEST = []


def take(p, why=""):
    """One file of this machine (p, as the machine names it) copied exactly, if it is text and holds no secret."""
    full = path(p)
    entry = {"path": p, "why": why}
    try:
        st = os.stat(full)
    except OSError as e:
        entry.update(copied=False, reason=f"not readable: {e.__class__.__name__}")
        MANIFEST.append(entry)
        return
    entry["size"] = st.st_size
    if NEVER.search(p):
        entry.update(copied=False, reason="its name says it holds secrets: never copied")
        MANIFEST.append(entry)
        return
    if st.st_size > MAX_BYTES:
        entry.update(copied=False, reason=f"over {MAX_BYTES // 1024} KB")
        MANIFEST.append(entry)
        return
    data = open(full, "rb").read()
    entry["sha256"] = hashlib.sha256(data).hexdigest()
    if os.path.basename(p) in BINARY_OK and data[:4] in BINARY_OK[os.path.basename(p)]:
        dest = os.path.join(OUT, p.lstrip("/"))      # a font the pages are drawn in: copied as it is, no secret in it
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
        entry["copied"] = True
        MANIFEST.append(entry)
        return
    if not is_text(p, data):
        entry.update(copied=False, reason="binary or a log/backup (listed, not copied)")
        MANIFEST.append(entry)
        return
    text = data.decode("utf-8", "replace")
    for n, line in enumerate(text.splitlines(), 1):
        for rx in SECRET_LINE:
            if rx.search(line):
                entry.update(copied=False, reason=f"line {n} looks like a secret: left out")
                MANIFEST.append(entry)
                return
    dest = os.path.join(OUT, p.lstrip("/"))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(data)
    os.chmod(dest, st.st_mode & 0o777)
    entry["copied"] = True
    MANIFEST.append(entry)


def walk(base, depth=2, why=""):
    full = path(base)
    if not os.path.isdir(full):
        return
    for dp, dns, fns in os.walk(full):
        rel_depth = dp[len(full):].count(os.sep)
        dns[:] = sorted(d for d in dns if d not in SKIP_DIRS and rel_depth < depth)
        for fn in sorted(fns):
            p = os.path.join(dp, fn)[len(ROOT):] if ROOT else os.path.join(dp, fn)
            if os.path.islink(path(p)):
                continue
            take(p, why)


def unit_files(unit):
    code, out = run(["systemctl", "show", unit, "-p", "FragmentPath,DropInPaths,ExecStart,LoadState"])
    props = dict(ln.split("=", 1) for ln in out.splitlines() if "=" in ln)
    files = [props.get("FragmentPath", "")] + props.get("DropInPaths", "").split()
    scripts = re.findall(r"argv\[\]=(\S+)", props.get("ExecStart", ""))
    scripts += re.findall(r"(?:^|\s)(/(?:opt|usr/local)/\S+\.(?:sh|py|mjs|js))", props.get("ExecStart", ""))
    return props.get("LoadState", "?"), [f for f in files if f], [s for s in scripts if s.startswith(("/opt/", "/usr/local/"))]


def collect():
    say("===== the TV room's own files, copied exactly for the open77-tv repository (READ ONLY)")
    for d in TV_DIRS:
        if os.path.isdir(path(d)):
            walk(d, depth=1, why="a television's build folder and up.sh")
    walk(ROOM_DIR, depth=2, why="the room service")
    walk(ANDROID_DIR, depth=1, why="Android's native screen (its compositor, VNC and switch scripts)")
    for u in UNITS:
        state, files, scripts = unit_files(u)
        if state != "loaded":
            continue
        for f in files:
            take(f, f"the unit {u}")
        for s in scripts:
            if os.path.isfile(path(s)):
                take(s, f"what {u} runs")
    take("/usr/share/novnc/open77-screen.html", "the Android screen's page")
    for d in ("/etc/nginx/sites-enabled", "/etc/nginx/conf.d"):
        full = path(d)
        if not os.path.isdir(full):
            continue
        for fn in sorted(os.listdir(full)):
            fp = os.path.join(full, fn)
            try:
                text = open(fp, errors="replace").read(MAX_BYTES)
            except OSError:
                continue
            if re.search(r"tv-browser|:8002|:8001|open77-room", text):
                take(os.path.realpath(fp)[len(ROOT):] if ROOT else os.path.realpath(fp), "an nginx site for the televisions")
    copied = [e for e in MANIFEST if e.get("copied")]
    left = [e for e in MANIFEST if not e.get("copied")]
    for e in copied:
        say(f"  copied  {e['path']} ({e['size']} bytes, sha256 {e['sha256'][:12]}) -- {e['why']}")
    for e in left:
        say(f"  NOT     {e['path']}: {e.get('reason')} -- {e['why']}")
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "MANIFEST.json"), "w") as f:
        json.dump({"collected": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "files": MANIFEST}, f, indent=1)
    with tarfile.open(os.path.join(W, "sources.tgz"), "w:gz") as t:
        t.add(OUT, arcname="sources")
    return {"copied": len(copied), "left_out": len(left), "secret_like": [e["path"] for e in left if "secret" in e.get("reason", "")]}


def main():
    if MODE != "collect":
        print(__doc__)
        return 2
    try:
        res = collect()
        rc = 0
    except Exception as e:  # noqa: BLE001
        import traceback
        say(f"!! {e.__class__.__name__}: {e}")
        for ln in traceback.format_exc().splitlines()[-6:]:
            say("   " + ln)
        res, rc = {"result": "error"}, 1
    say("")
    say("result: " + json.dumps(res, separators=(",", ":")), limit=100000)
    return rc


if __name__ == "__main__":
    sys.exit(main())
