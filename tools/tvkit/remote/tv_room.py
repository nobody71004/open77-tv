#!/usr/bin/env python3
"""READ ONLY: the TV room's home screen -- the shared browsers' own Chromium (neko, open77-tvbrowser*) -- and why its
tiles (Chrome, the services, Android, the mirror, the profiles) do not answer a click.  tv_kit.py `room` sends this
to a work folder of its own on the dedicated machine and runs it there (2026-11-07).

    python3 tv_room.py WORKDIR

It changes nothing: no container, unit, file or setting is started, stopped or written outside WORKDIR.  It reads,
for every open77-tvbrowser* container, how its browser runs (processes, supervisord's programs, Chromium's command
line, its policies, the tabs its DevTools endpoint lists, neko's settings and its last lines about control and
input), the pages its home screen is made of (fetched read only, as the browser would, with what they link to), and
one picture of its screen (one frame of its X display, read with GStreamer's ximagesrc straight to this script, no
file written in the container); for every /opt/open77-tvbrowser* folder its files and the image's build folder; on
the host what listens, the units the room's tiles lean on (rbhost, the Android ones, the mirrors) with their last
journal lines, Waydroid, nginx's locations, and every place that still names an old machine.  What it fetched is
packed into WORKDIR/room.tgz for the kit to copy back.  Passwords, tokens and keys are never printed: a value whose
name says secret is shown as <hidden>, in lines and in the fetched files alike.  Exit 0 when it ran.
"""
import html.parser
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.parse
import urllib.request

W = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
R = os.path.join(W, "room")
ROOT = os.environ.get("O77ROOT", "")          # "" on the machine: the rehearsal's own root of fake files
OLD_NAMES = ["xbuniverse.duckdns.org", "xbuniverse-vm.duckdns.org", "88.214.59.166", "104.251.218.227",
             "10.77.0.2", "192.168.0.213"]
UNIT_RX = re.compile(r"(?i)rbhost|waydroid|android|mirror|tvbrowser|tv-|room|profile|watch|neko|cdp|chrome")
SECRET_NAME = re.compile(r"(?i)pass|secret|token|cred|apikey|api_key|private|cookie")
URL_SECRET = re.compile(r"(?i)\b((?:pwd|password|passwd|pass|token|access_token|api_?key|secret|auth|sig|signature|"
                        r"ticket|session|sid)=)[^&\s\"'#<>]+")
JSON_SECRET = re.compile(r'(?i)("[^"]*(?:pass|secret|token|cred|apikey|api_key|private)[^"]*"\s*:\s*")[^"]*(")')
YAML_SECRET = re.compile(r"(?im)^(\s*[\w.-]*(?:pass|secret|token|cred|apikey|api_key|private)[\w.-]*\s*:\s*).+$")
KV_SECRET = re.compile(r"(?i)(?<![\w.-])([\w.-]*(?:pass|secret|token|cred|apikey|api_key|private)[\w.-]*=)[^\s&\"',;]+")
ENV_SECRET = re.compile(r"(?im)^(\s*(?:export\s+)?[\w.-]*(?:pass|secret|token|cred|apikey|api_key|private)[\w.-]*\s*=\s*).+$")
NEVER_COPY = re.compile(r"(?i)secret|\.key$|\.pem$|\.p12$|id_rsa|id_ed25519|credential|password|\.env$")
LINE_MAX = 300
FETCH_MAX = 2 * 1024 * 1024
BUILD_FILE_MAX = 256 * 1024
BUILD_TOTAL_MAX = 6 * 1024 * 1024
TEXT_EXT = (".html", ".htm", ".js", ".mjs", ".css", ".json", ".conf", ".sh", ".py", ".txt", ".yaml", ".yml", ".md",
            ".ini", ".env", ".service", ".toml", ".xml", ".svg", "dockerfile")


def say(text=""):
    for line in str(text).splitlines() or [""]:
        print("TV " + mask(line)[:LINE_MAX], flush=True)


def mask(text):
    text = URL_SECRET.sub(r"\1<hidden>", text)
    text = JSON_SECRET.sub(r"\1<hidden>\2", text)
    text = YAML_SECRET.sub(r"\1<hidden>", text)
    text = ENV_SECRET.sub(r"\1<hidden>", text)
    text = KV_SECRET.sub(r"\1<hidden>", text)
    return text


def mask_env(line):
    k, sep, v = line.partition("=")
    if sep and SECRET_NAME.search(k):
        return k + "=<hidden>"
    return mask(line)


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


def dsh(c, script, timeout=20, binary=False):
    """A shell line inside container c (sh -c), as docker exec runs it."""
    return run(["docker", "exec", c, "sh", "-c", script], timeout=timeout, binary=binary)


def path(p):
    return ROOT + p


def save_text(rel, text):
    dest = os.path.join(R, rel)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "w", encoding="utf-8") as f:
        f.write(mask(text))
    return dest


def old_names_in(text):
    return [n for n in OLD_NAMES if n in text]


# -- the policies ------------------------------------------------------------------------------

POLICY_KEYS = ["HomepageLocation", "HomepageIsNewTabPage", "RestoreOnStartup", "RestoreOnStartupURLs",
               "NewTabPageLocation", "ShowHomeButton", "URLBlocklist", "URLAllowlist", "DefaultPopupsSetting",
               "PopupsAllowedForUrls", "PopupsBlockedForUrls", "ExtensionInstallForcelist", "ExtensionInstallBlocklist",
               "DeveloperToolsAvailability", "BrowserSignin", "IncognitoModeAvailability", "BrowserGuestModeEnabled",
               "AutoLaunchProtocolsFromOrigins", "ExternalProtocolDialogShowAlwaysOpenCheckbox", "URLAllowlistNotes",
               "DefaultJavaScriptSetting", "JavaScriptBlockedForUrls", "BookmarkBarEnabled", "ManagedBookmarks"]


def show_policies(text, where):
    try:
        p = json.loads(text)
    except ValueError as e:
        say(f"  {where}: not JSON ({e})")
        return []
    say(f"  {where}: {len(p)} setting(s)" + (f"; it names an old machine: {', '.join(old_names_in(text))}"
                                              if old_names_in(text) else ""))
    urls = []
    for k in POLICY_KEYS:
        if k not in p:
            continue
        v = p[k]
        shown = json.dumps(v)
        if len(shown) > 240:
            shown = shown[:240] + f" ... ({len(v) if isinstance(v, (list, dict)) else len(shown)} entries)"
        say(f"    {k} = {shown}")
        if k in ("HomepageLocation", "NewTabPageLocation") and isinstance(v, str):
            urls.append(v)
        if k == "RestoreOnStartupURLs" and isinstance(v, list):
            urls += [u for u in v if isinstance(u, str)]
    others = sorted(set(p) - set(POLICY_KEYS))
    if others:
        say("    and: " + ", ".join(others)[:LINE_MAX - 10])
    return urls


# -- the pages ---------------------------------------------------------------------------------

class PageParts(html.parser.HTMLParser):
    """What a home screen page is made of: its scripts, styles and frames, and what each tile does."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.assets, self.tiles, self._open = [], [], None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "script" and a.get("src"):
            self.assets.append(a["src"])
        elif tag == "link" and "stylesheet" in (a.get("rel") or "") and a.get("href"):
            self.assets.append(a["href"])
        elif tag in ("iframe", "frame") and a.get("src"):
            self.assets.append(a["src"])
        if tag in ("a", "button") or any(k.startswith("data-") for k in a) or "onclick" in a:
            what = {k: v for k, v in a.items() if k in ("href", "onclick", "target", "id", "class") or k.startswith("data-")}
            if what:
                self._open = [tag, what, ""]
                self.tiles.append(self._open)

    def handle_data(self, data):
        if self._open is not None and data.strip():
            self._open[2] = (self._open[2] + " " + data.strip()).strip()[:60]

    def handle_endtag(self, tag):
        if self._open is not None and tag == self._open[0]:
            self._open = None


INSIDE_FETCH = ("import sys, urllib.request\n"
                "u = sys.argv[1]\n"
                "r = urllib.request.urlopen(urllib.request.Request(u, headers={'User-Agent': 'open77-tvkit-room'}), timeout=10)\n"
                "sys.stdout.buffer.write(r.read(%d))\n" % FETCH_MAX)


def fetch(c, url):
    """The bytes of a URL as the container's browser reaches it: from inside the container for its own names
    (localhost, 127.0.0.1, the bridge's gateway), from the host for public ones; file:// read inside it."""
    parts = urllib.parse.urlsplit(url)
    if parts.scheme == "file":
        code, data = run(["docker", "exec", c, "cat", parts.path], timeout=15, binary=True)
        return (data[:FETCH_MAX], "read inside the container") if code == 0 else (None, f"cat: exit {code}")
    if parts.scheme not in ("http", "https"):
        return None, f"not fetched ({parts.scheme or 'no'} scheme)"
    host = (parts.hostname or "").lower()
    if host in ("localhost", "127.0.0.1", "0.0.0.0", "::1") or host.startswith("172.17.") or "." not in host:
        code, data = run(["docker", "exec", c, "python3", "-c", INSIDE_FETCH, url], timeout=20, binary=True)
        if code == 0:
            return data, "fetched inside the container"
        code, data = run(["docker", "exec", c, "sh", "-c", 'curl -fsS --max-time 10 "$0" 2>/dev/null || '
                          'wget -qO- -T 10 "$0" 2>/dev/null', url], timeout=20, binary=True)
        return (data, "fetched inside the container") if code == 0 and data else (None, "the container could not fetch it")
    try:
        r = urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "open77-tvkit-room"}), timeout=10)
        return r.read(FETCH_MAX), f"fetched from the host: HTTP {r.status}"
    except Exception as e:  # noqa: BLE001 -- what failed is the finding
        return None, f"the host could not fetch it: {e.__class__.__name__}: {e}"


def show_pages(c, urls):
    seen, n = set(), 0
    queue = [(u, 0) for u in urls]
    while queue and n < 14:
        url, depth = queue.pop(0)
        if url in seen or not url:
            continue
        seen.add(url)
        n += 1
        data, how = fetch(c, url)
        say(f"  page {mask(url)}: {how}" + (f", {len(data)} bytes" if data is not None else ""))
        if data is None:
            continue
        text = data.decode("utf-8", "replace")
        ext = os.path.splitext(urllib.parse.urlsplit(url).path)[1].lower() or ".html"
        saved = save_text(os.path.join("pages", f"{c}-{n:02d}{ext if len(ext) <= 6 else '.txt'}"), text)
        say(f"    saved as {os.path.relpath(saved, R)}" + (f"; it names an old machine: {', '.join(old_names_in(text))}"
                                                         if old_names_in(text) else ""))
        if ext in (".html", ".htm") or "<html" in text[:2000].lower():
            parts = PageParts()
            try:
                parts.feed(text)
            except Exception as e:  # noqa: BLE001 -- a page the parser chokes on is still saved
                say(f"    (its HTML did not parse: {e})")
            for tag, what, label in parts.tiles[:40]:
                say(f"    {tag} '{label}': " + ", ".join(f"{k}={v}" for k, v in what.items())[:220])
            if depth == 0:
                for asset in parts.assets[:14]:
                    queue.append((urllib.parse.urljoin(url, asset), depth + 1))
        # What a script reaches for: hosts, ports and APIs its tiles call.
        for m in sorted(set(re.findall(r"(?:https?|wss?)://[A-Za-z0-9.\-]+(?::\d+)?(?:/[^\s\"'`<>)]*)?", text)))[:30]:
            say(f"    reaches: {mask(m)[:200]}")


# -- one container -----------------------------------------------------------------------------

def container(c):
    say(f"===== container {c}")
    code, out = run(["docker", "inspect", c, "--format",
                     "state {{.State.Status}} since {{.State.StartedAt}}, restarts {{.RestartCount}}, image "
                     "{{.Config.Image}}, health {{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}"])
    say("  " + out.strip())
    code, running = run(["docker", "inspect", c, "--format", "{{.State.Running}}"])
    if running.strip() != "true":
        say("  (not running: nothing inside it to read)")
        return
    code, out = dsh(c, "ps -eo pid,etimes,stat,pcpu,rss,args 2>/dev/null || ps")
    keep = [ln for ln in out.splitlines() if re.search(r"(?i)chrom|neko|supervis|xorg|xvfb|pulse|cdp|socat|python|node|PID", ln)]
    say(f"  processes ({len(out.splitlines())} in all; the browser's and neko's):")
    for ln in keep[:40]:
        say("    " + ln.strip())
    zombies = [ln for ln in out.splitlines() if re.search(r"\sZ[a-z+<NsLl]*\s", " " + ln)]
    if zombies:
        say(f"  {len(zombies)} defunct process(es): " + "; ".join(z.split(None, 5)[-1][:60] for z in zombies[:5]))
    code, out = dsh(c, "supervisorctl -c /etc/neko/supervisord.conf status 2>/dev/null || supervisorctl status 2>&1")
    say("  supervisord's programs:")
    for ln in out.splitlines()[:20]:
        say("    " + ln.strip())
    # neko's settings: every NEKO_* the neko process runs with (control, implicit hosting, locks...).
    code, out = dsh(c, 'for p in /proc/[0-9]*; do c=$(tr "\\0" " " < $p/cmdline 2>/dev/null); case "$c" in '
                       '*"sh -c"*) ;; *neko*) tr "\\0" "\\n" < $p/environ 2>/dev/null;; esac; done')
    env = sorted({ln for ln in out.splitlines() if re.match(r"^(NEKO_|DISPLAY=|XDG_|PULSE_)", ln)})
    display = next((ln.split("=", 1)[1] for ln in env if ln.startswith(("NEKO_DISPLAY=", "DISPLAY="))), "") or ":99.0"
    say(f"  neko's settings ({len(env)}):")
    for ln in env:
        say("    " + mask_env(ln))
    code, out = dsh(c, "cat /etc/neko/neko.yaml /etc/neko/neko.yml 2>/dev/null | head -150")
    if out.strip():
        save_text(os.path.join("configs", f"{c}-neko.yaml"), out)
        say(f"  neko.yaml ({len(out.splitlines())} lines, saved masked): " +
            "; ".join(ln.strip() for ln in mask(out).splitlines() if re.search(r"(?i)control|host|lock|implicit|plugin|inactive|file", ln))[:240])
    code, out = dsh(c, 'ls /etc/neko/supervisord/ 2>/dev/null; for f in /etc/neko/supervisord/*.conf; do '
                       'echo "== $f"; grep -E "^(\\[program|command|user|autorestart|environment)" "$f"; done')
    say("  supervisord's program files:")
    flags = ""
    for ln in out.splitlines()[:60]:
        say("    " + ln.strip())
        if "chrom" in ln and ln.startswith("command"):
            flags = ln
    m = re.search(r"--remote-debugging-port[= ](\d+)", out)
    cdp_port = int(m.group(1)) if m else 9222
    start_urls = [u for u in re.findall(r"(?:https?|file)://\S+", flags)]
    code, out = dsh(c, "cat /etc/chromium/policies/managed/*.json 2>/dev/null")
    if out.strip():
        save_text(os.path.join("configs", f"{c}-policies.json"), out)
        start_urls += show_policies(out, "the browser's policies (as the container sees them)")
    # The tabs the browser has open, from its own DevTools endpoint (inside the container: it listens there only).
    for what in ("json/version", "json/list"):
        code, out = run(["docker", "exec", c, "python3", "-c", INSIDE_FETCH, f"http://127.0.0.1:{cdp_port}/{what}"],
                        timeout=12)
        if code != 0:
            say(f"  DevTools {what} on {cdp_port}: no answer (exit {code}) -- " + out.strip()[-160:])
            continue
        try:
            j = json.loads(out)
        except ValueError:
            say(f"  DevTools {what}: not JSON: {out[:120]}")
            continue
        if what == "json/version":
            say(f"  DevTools: {j.get('Browser')} ({j.get('User-Agent', '')[:80]})")
        else:
            say(f"  DevTools: {len(j)} target(s) open:")
            for t in j[:20]:
                say(f"    {t.get('type')}: '{(t.get('title') or '')[:60]}' {t.get('url', '')[:200]}")
                if t.get("type") == "page" and t.get("url"):
                    start_urls.append(t["url"])
    code, out = dsh(c, "ls -la /var/www 2>/dev/null | tail -n +2")
    say("  the neko client's folder (/var/www):")
    for ln in out.splitlines()[:40]:
        say("    " + ln.strip())
    for name in re.findall(r"(?m)\s((?:open77|home|room|tv|launcher|apps?)(?:[-_.][\w.-]*)?\.(?:js|html|css|json))$", out):
        if NEVER_COPY.search(name):
            continue
        code, text = dsh(c, f"cat /var/www/{name}")
        if code == 0:
            save_text(os.path.join("www", c, name), text)
            say(f"    copied {name} ({len(text)} bytes)" + (f"; it names an old machine: {', '.join(old_names_in(text))}"
                                                           if old_names_in(text) else ""))
    code, out = dsh(c, "find / -xdev -maxdepth 4 \\( -path /proc -o -path /sys -o -path /usr -o -path /var/www/js "
                       "-o -path /profiles -o -path /var/lib \\) -prune -o -type f -name '*.html' -print 2>/dev/null | head -40")
    if out.strip():
        say("  the pages the image carries (*.html, outside /usr and the client's bundle):")
        for ln in out.splitlines():
            say("    " + ln.strip())
    show_pages(c, start_urls)
    # One picture of its screen: one frame of the X display, straight to this script (nothing written inside it).
    code, png = run(["docker", "exec", "-e", f"DISPLAY={display}", c, "sh", "-c",
                     f"gst-launch-1.0 -q ximagesrc display-name={display} num-buffers=1 use-damage=false show-pointer=true "
                     "! videoconvert ! pngenc ! fdsink fd=1"], timeout=30, binary=True)
    if code == 0 and png[:8] == b"\x89PNG\r\n\x1a\n":
        os.makedirs(os.path.join(R, "screens"), exist_ok=True)
        with open(os.path.join(R, "screens", f"{c}.png"), "wb") as f:
            f.write(png)
        say(f"  its screen ({display}): one frame, {len(png)} bytes, saved as screens/{c}.png")
    else:
        say(f"  its screen ({display}): not captured (exit {code}, {len(png)} bytes)")
    code, out = dsh(c, f"DISPLAY={display} xwininfo -root -tree 2>/dev/null | grep -E '^ +0x' | head -40")
    if out.strip():
        say("  its windows:")
        for ln in out.splitlines()[:25]:
            say("    " + ln.strip()[:200])
    code, out = run(["docker", "logs", "--tail", "2500", c], timeout=30)
    hits = [ln for ln in out.splitlines()
            if re.search(r"(?i)control|host|implicit|input|mouse|keyboard|openinapp|plugin|locked|error|warn|fatal|panic", ln)]
    say(f"  neko's lines about control, input and errors (the last {min(len(hits), 45)} of {len(hits)}):")
    for ln in hits[-45:]:
        say("    " + ln.strip())
    code, out = dsh(c, "ls -la /var/log/neko 2>/dev/null; for f in /var/log/neko/*.log; do echo \"== $f\"; "
                       "tail -n 400 \"$f\" | grep -i -E 'error|warn|crash|fatal|check failed|gpu|denied|refused|blocked' | tail -n 25; done")
    if out.strip():
        say("  the browser's own logs (/var/log/neko, their errors):")
        for ln in out.splitlines()[:120]:
            say("    " + ln.strip())


# -- the folders and the host -------------------------------------------------------------------

def folders():
    root = path("/opt")
    names = sorted(n for n in os.listdir(root) if n.startswith("open77-tvbrowser")) if os.path.isdir(root) else []
    budget = BUILD_TOTAL_MAX
    for n in names:
        d = os.path.join(root, n)
        say(f"===== folder /opt/{n}")
        for e in sorted(os.listdir(d)):
            if e == "profiles":
                continue
            p = os.path.join(d, e)
            say(f"  {time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(p)))} {e}{'/' if os.path.isdir(p) else ''}")
        pol = os.path.join(d, "policies.json")
        if os.path.isfile(pol):
            text = open(pol, encoding="utf-8", errors="replace").read()
            save_text(os.path.join("configs", f"{n}-policies.json"), text)
            show_policies(text, "policies.json (mounted into the container)")
        build = os.path.join(d, "build")
        if os.path.isdir(build):
            files = []
            for dp, dns, fns in os.walk(build):
                dns[:] = sorted(x for x in dns if x not in ("node_modules", ".git", "profiles"))
                for fn in sorted(fns):
                    files.append(os.path.join(dp, fn))
            say(f"  build/: {len(files)} file(s)")
            for fp in files[:120]:
                rel = os.path.relpath(fp, d)
                size = os.path.getsize(fp)
                low = fp.lower()
                copy = size <= BUILD_FILE_MAX and budget >= size and (low.endswith(TEXT_EXT) or "dockerfile" in low) \
                    and not NEVER_COPY.search(os.path.basename(fp))
                say(f"    {rel} ({size} bytes)" + (" -- copied" if copy else ""))
                if copy:
                    text = open(fp, encoding="utf-8", errors="replace").read()
                    save_text(os.path.join("build", n, rel), text)
                    budget -= size


def host():
    say("===== the host")
    code, out = run(["ss", "-ltnp"])
    say("  listening (TCP):")
    for ln in out.splitlines()[1:80]:
        parts = ln.split()
        if len(parts) >= 4:
            say(f"    {parts[3]} {parts[-1][:120] if len(parts) > 5 else ''}")
    code, out = run(["systemctl", "list-units", "--all", "--type=service", "--no-legend", "--plain"])
    units = [ln.split()[0] for ln in out.splitlines() if ln.split() and UNIT_RX.search(ln.split()[0])]
    say(f"  the units the room leans on ({len(units)}):")
    for u in units[:30]:
        code, show = run(["systemctl", "show", u, "-p", "ActiveState,SubState,NRestarts,ExecMainStartTimestamp,FragmentPath"])
        info = dict(ln.split("=", 1) for ln in show.splitlines() if "=" in ln)
        say(f"    {u}: {info.get('ActiveState', '?')}/{info.get('SubState', '?')}, restarts {info.get('NRestarts', '?')}, "
            f"since {info.get('ExecMainStartTimestamp', '?')}, {info.get('FragmentPath', '?')}")
        code, cat = run(["systemctl", "cat", u])
        for ln in cat.splitlines():
            if re.match(r"^(ExecStart|WorkingDirectory|Environment|User)=", ln):
                say("      " + mask_env(ln) if ln.startswith("Environment") else "      " + ln)
        code, jl = run(["journalctl", "-u", u, "-n", "12", "--no-pager", "-o", "short-iso"], timeout=20)
        for ln in jl.splitlines()[-12:]:
            say("      | " + ln[:250])
    code, out = run(["waydroid", "status"], timeout=20)
    say("  waydroid status: " + " | ".join(ln.strip() for ln in out.splitlines() if ln.strip())[:250])
    code, out = run(["nginx", "-T"], timeout=20)
    say("  nginx (server names and locations):")
    n = 0
    for ln in out.splitlines():
        s = ln.strip()
        if re.match(r"^(# configuration file|server_name|location|proxy_pass|alias|root|return)\b", s) and n < 140:
            say("    " + s)
            n += 1


def old_names():
    say("===== what still names an old machine (" + ", ".join(OLD_NAMES) + ")")
    places = [R, path("/etc/systemd/system")] + [path("/opt/" + n) for n in os.listdir(path("/opt"))
                                                 if n.startswith("open77-tvbrowser") or UNIT_RX.search(n)] \
        if os.path.isdir(path("/opt")) else [R]
    found = 0
    for place in places:
        for dp, dns, fns in os.walk(place):
            dns[:] = [x for x in dns if x not in ("profiles", "node_modules", ".git", "images", "rootfs", "data")]
            for fn in fns:
                fp = os.path.join(dp, fn)
                try:
                    if os.path.getsize(fp) > 1024 * 1024 or os.path.islink(fp):
                        continue
                    text = open(fp, encoding="utf-8", errors="strict").read()
                except (OSError, UnicodeDecodeError):
                    continue
                for i, ln in enumerate(text.splitlines(), 1):
                    hit = old_names_in(ln)
                    if hit and found < 60:
                        found += 1
                        shown = fp[len(R) + 1:] + " (fetched)" if fp.startswith(R) else fp[len(ROOT):]
                        say(f"  {shown}:{i}: {ln.strip()[:200]}")
    say(f"  {found} line(s)" + (" (the first 60 shown)" if found >= 60 else ""))


def main():
    os.makedirs(R, exist_ok=True)
    code, who = run(["hostname"])
    say(f"the TV room, read only, on {who.strip()} at {time.strftime('%Y-%m-%d %H:%M:%S %Z')}")
    code, out = run(["docker", "ps", "-a", "--format", "{{.Names}}"])
    names = sorted(n for n in out.split() if n.startswith("open77-tvbrowser"))
    say(f"shared-browser containers: {', '.join(names) or 'none'}")
    for c in names:
        try:
            container(c)
        except Exception as e:  # noqa: BLE001 -- one container's surprise must not hide the others
            say(f"  (reading {c} stopped: {e.__class__.__name__}: {e})")
    for part in (folders, host, old_names):
        try:
            part()
        except Exception as e:  # noqa: BLE001
            say(f"  ({part.__name__} stopped: {e.__class__.__name__}: {e})")
    with tarfile.open(os.path.join(W, "room.tgz"), "w:gz") as t:
        t.add(R, arcname="room")
    count = sum(len(f) for _, _, f in os.walk(R))
    say(f"packed: {count} file(s) in room.tgz ({os.path.getsize(os.path.join(W, 'room.tgz'))} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
