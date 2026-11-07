"""The TVs' shared browsers (the open77-tvbrowser* neko containers) given the machine's own public address again
(run as root on the dedicated machine by tv_kit.py; 2026-11-06).

Why: the move to this machine (2026-11-04) made every container again from a `docker commit` of the old one.  A
commit keeps the old container's environment in the image, so the containers here still tell players to send the
picture's connection to the old machine's address (NEKO_WEBRTC_NAT1TO1 and any other setting naming it) -- the
neko.env files here say the new one, but the containers never read them again.

    python3 tv_fix.py plan --ip NEW_IP [--names C[,C]]          READ ONLY: what each container would get
    python3 tv_fix.py fix  --ip NEW_IP [--names C[,C]] --ts TS  each container whose settings name an old address:
                                                               kept whole (docker commit), stopped, renamed aside
                                                               (not removed, not started again), made again with
                                                               exactly its own settings but the address, started,
                                                               checked (running, the new address, its page answers);
                                                               one that fails is put back by itself
    python3 tv_fix.py undo [--names C[,C]] --ts TS              every container of the last fix put back as it was

Records: /opt/open77-backups/tvkit-TS/ (each container's inspect, record.json); tvkit-last.json points at the last.
Prints no secrets: a setting is named, never shown, but for the addresses in it.  Exit 0: done (or nothing to do);
1: something failed (said); 2: refused before anything changed (said).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

OLD_IPS = ["88.214.59.166", "104.251.218.227", "10.77.0.2", "192.168.0.213"]
IP_RX = re.compile(r"(?<![\d.])(" + "|".join(re.escape(i) for i in OLD_IPS) + r")(?![\d])")
NAME_RX = re.compile(r"^open77-tvbrowser[A-Za-z0-9_.-]*$")
BACKUPS = os.environ.get("TVKIT_BACKUPS", "/opt/open77-backups")
LAST = os.path.join(BACKUPS, "tvkit-last.json")


def say(s):
    print(f"[{time.strftime('%H:%M:%S')}] {s}", flush=True)


def run(cmd, timeout=180):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        return 124, "timed out"
    except FileNotFoundError as e:
        return 127, str(e)


def inspect(name):
    rc, out = run(["docker", "inspect", name])
    if rc != 0:
        return None
    return json.loads(out)[0]


def image_cfg(ref):
    rc, out = run(["docker", "image", "inspect", "-f", "{{json .Config}}", ref])
    try:
        return json.loads(out) if rc == 0 else {}
    except ValueError:
        return {}


def machine_ips():
    rc, out = run(["ip", "-4", "-o", "addr", "show", "scope", "global"])
    return {l.split()[3].split("/")[0] for l in out.splitlines() if len(l.split()) > 3}


def wrong_env(c):
    """[(key, old value, new value)] for every env entry of the container that names an old address."""
    out = []
    for e in (c.get("Config") or {}).get("Env") or []:
        k, _, v = e.partition("=")
        if IP_RX.search(v):
            out.append((k, v, None))
    return out


def shown(k, v, new_ip):
    """A setting named, never shown: only the addresses in it."""
    olds = IP_RX.findall(v)
    return f"{k}: " + ", ".join(f"{ip} x{olds.count(ip)}" for ip in sorted(set(olds))) + f" -> {new_ip}"


def docker_args(c, icfg, image, env_over):
    """`docker create` arguments that make the container C (docker inspect) again from IMAGE, ENV_OVER (KEY=VALUE)
    on top -- the move's own rule (move_place.py docker_args), which made these containers on 2026-11-04."""
    cfg, hc = c.get("Config") or {}, c.get("HostConfig") or {}
    a = ["--name", c["Name"].lstrip("/")]
    rp = hc.get("RestartPolicy") or {}
    if rp.get("Name") and rp["Name"] != "no":
        a += ["--restart", rp["Name"] + (f":{rp['MaximumRetryCount']}" if rp["Name"] == "on-failure" and rp.get("MaximumRetryCount") else "")]
    img_env = set(icfg.get("Env") or [])
    over = {e.split("=", 1)[0]: e for e in env_over}
    for e in cfg.get("Env") or []:
        k = e.split("=", 1)[0]
        if k in over:
            continue
        if e not in img_env:
            a += ["-e", e]
    for e in env_over:
        a += ["-e", e]
    for cport, binds in (hc.get("PortBindings") or {}).items():
        for b in binds or []:
            ip = b.get("HostIp") or ""
            ip = f"[{ip}]" if ":" in ip else ip
            a += ["-p", (f"{ip}:" if ip else "") + f"{b.get('HostPort') or ''}:{cport}"]
    for b in hc.get("Binds") or []:
        a += ["-v", b]
    binds = {b.split(":")[1] for b in hc.get("Binds") or [] if ":" in b}
    for m in c.get("Mounts") or []:
        if m.get("Type") == "volume" and m.get("Destination") not in binds:
            a += ["-v", f"{m.get('Name')}:{m.get('Destination')}" + ("" if m.get("RW", True) else ":ro")]
    if hc.get("ShmSize") and hc["ShmSize"] != 67108864:
        a += ["--shm-size", str(hc["ShmSize"])]
    for k, flag in (("CapAdd", "--cap-add"), ("CapDrop", "--cap-drop"), ("SecurityOpt", "--security-opt"), ("ExtraHosts", "--add-host"),
                    ("Dns", "--dns"), ("GroupAdd", "--group-add")):
        for v in hc.get(k) or []:
            a += [flag, v]
    for dv in hc.get("Devices") or []:
        a += ["--device", f"{dv['PathOnHost']}:{dv['PathInContainer']}:{dv.get('CgroupPermissions', 'rwm')}"]
    if hc.get("Privileged"):
        a += ["--privileged"]
    if hc.get("Init"):
        a += ["--init"]
    nm = hc.get("NetworkMode")
    if nm and nm not in ("default", "bridge"):
        a += ["--network", nm]
    if hc.get("Memory"):
        a += ["--memory", str(hc["Memory"])]
    if hc.get("NanoCpus"):
        a += ["--cpus", str(hc["NanoCpus"] / 1e9)]
    for u in hc.get("Ulimits") or []:
        a += ["--ulimit", f"{u['Name']}={u['Soft']}:{u['Hard']}"]
    for k, v in (hc.get("Tmpfs") or {}).items():
        a += ["--tmpfs", f"{k}:{v}" if v else k]
    lc = hc.get("LogConfig") or {}
    if lc.get("Type") and lc["Type"] != "json-file":
        a += ["--log-driver", lc["Type"]]
    for k, v in (lc.get("Config") or {}).items():
        a += ["--log-opt", f"{k}={v}"]
    if cfg.get("User") and cfg["User"] != icfg.get("User"):
        a += ["--user", cfg["User"]]
    if cfg.get("WorkingDir") and cfg["WorkingDir"] != icfg.get("WorkingDir"):
        a += ["--workdir", cfg["WorkingDir"]]
    hcfg = cfg.get("Healthcheck")
    if hcfg and hcfg != icfg.get("Healthcheck") and hcfg.get("Test"):
        if hcfg["Test"][:1] == ["NONE"]:
            a += ["--no-healthcheck"]
        else:
            a += ["--health-cmd", " ".join(hcfg["Test"][1:]) if hcfg["Test"][0] == "CMD-SHELL" else json.dumps(hcfg["Test"][1:])]
    for k, v in (cfg.get("Labels") or {}).items():
        if (icfg.get("Labels") or {}).get(k) != v:
            a += ["--label", f"{k}={v}"]
    ep = cfg.get("Entrypoint")
    if ep and ep != icfg.get("Entrypoint"):
        a += ["--entrypoint", ep[0]]
    a += [image]
    cmd = cfg.get("Cmd")
    if ep and ep != icfg.get("Entrypoint"):
        a += ep[1:] + (cmd or [])
    elif cmd and cmd != icfg.get("Cmd"):
        a += cmd
    return a


def page_port(c):
    for cport, binds in ((c.get("HostConfig") or {}).get("PortBindings") or {}).items():
        if cport == "8080/tcp":
            for b in binds or []:
                if b.get("HostPort"):
                    return int(b["HostPort"])
    return None


def page_answers(port):
    if not port:
        return True, "no page port published"
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=4) as r:
            return True, f"HTTP {r.status}"
    except urllib.error.HTTPError as e:
        return e.code < 500, f"HTTP {e.code}"
    except Exception as e:  # noqa: BLE001
        return False, f"{e.__class__.__name__}"


def healthy(name, new_ip, port, wait=75):
    """Running, its settings name the new address and none of the old, its page answers."""
    deadline = time.time() + wait
    why = "not checked"
    while time.time() < deadline:
        c = inspect(name)
        st = (c or {}).get("State") or {}
        if c and st.get("Running") and not st.get("Restarting"):
            env = (c.get("Config") or {}).get("Env") or []
            if any(IP_RX.search(e.partition("=")[2]) for e in env):
                return False, "its settings still name an old address"
            if not any(new_ip in e for e in env):
                return False, f"no setting names {new_ip}"
            ok, what = page_answers(port)
            if ok:
                return True, f"running, {what} from its page"
            why = f"its page does not answer yet ({what})"
        else:
            why = f"state {st.get('Status', '?')}"
        time.sleep(3)
    return False, why


def containers(names):
    rc, out = run(["docker", "ps", "-a", "--format", "{{.Names}}"])
    if rc != 0:
        raise SystemExit(f"docker does not answer: {out[-300:]}")
    every = sorted(n for n in out.splitlines() if NAME_RX.match(n) and "-tvkit-" not in n)
    if names:
        missing = [n for n in names if n not in every]
        if missing:
            say(f"!! no such shared-browser container here: {', '.join(missing)} (here: {', '.join(every)})")
            sys.exit(2)
        return names
    return every


def cmd_plan(a, fix=False):
    if a.ip not in machine_ips():
        say(f"!! {a.ip} is not an address of this machine ({', '.join(sorted(machine_ips()))}): NOTHING changed")
        return 2
    names = containers([n for n in (a.names or "").split(",") if n])
    say(f"the shared-browser containers here: {', '.join(names) or 'none'}; the address players must be given: {a.ip}")
    todo = []
    for n in names:
        c = inspect(n)
        st = (c.get("State") or {}).get("Status")
        icfg = image_cfg(c.get("Image"))
        wrong = wrong_env(c)
        from_image = [k for k, v, _ in wrong if f"{k}={v}" in set(icfg.get("Env") or [])]
        if not wrong:
            say(f"  {n} ({st}): no setting names an old address -- nothing to change")
            continue
        say(f"  {n} ({st}, image {c['Config'].get('Image')}): {len(wrong)} setting(s) name an old address"
            + (f" ({len(from_image)} of them from the image the move made)" if from_image else ""))
        for k, v, _ in wrong:
            say(f"      {shown(k, v, a.ip)}")
        todo.append(n)
    if not fix:
        say(f"READ ONLY: {len(todo)} container(s) would be made again with {a.ip}" if todo else "READ ONLY: nothing to change")
        return 0
    if not todo:
        say("nothing to change: every shared-browser container already gives players this machine's address")
        return 0
    rec_dir = os.path.join(BACKUPS, f"tvkit-{a.ts}")
    os.makedirs(rec_dir, mode=0o700, exist_ok=True)
    rec = {"ts": a.ts, "ip": a.ip, "containers": []}
    rc_all = 0
    for n in todo:
        c = inspect(n)
        with open(os.path.join(rec_dir, f"{n}.inspect.json"), "w") as f:
            json.dump(c, f, indent=1)
        was_running = bool((c.get("State") or {}).get("Running"))
        policy = ((c.get("HostConfig") or {}).get("RestartPolicy") or {}).get("Name") or "no"
        keep_img = f"tvkit-keep/{n}:{a.ts}"
        old_name = f"{n}-tvkit-old-{a.ts}"
        say(f"===== {n}")
        t = time.time()
        rc, out = run(["docker", "commit", n, keep_img], timeout=900)
        if rc != 0:
            say(f"  !! could not keep it whole (docker commit): {out[-300:]} -- NOTHING changed for {n}")
            rc_all = 1
            continue
        say(f"  kept whole as the image {keep_img} ({time.time() - t:.0f} s)")
        icfg = image_cfg(keep_img)
        over = [f"{k}={IP_RX.sub(a.ip, v)}" for k, v, _ in wrong_env(c)]
        args = docker_args(c, icfg, keep_img, over)
        run(["docker", "stop", "-t", "20", n], timeout=120)
        rc, out = run(["docker", "rename", n, old_name])
        if rc != 0:
            say(f"  !! could not rename it aside: {out[-300:]} -- starting it again as it was")
            if was_running:
                run(["docker", "start", n])
            rc_all = 1
            continue
        run(["docker", "update", "--restart=no", old_name])
        say(f"  stopped and renamed aside: {old_name} (kept, restart off; not removed)")
        entry = {"name": n, "old": old_name, "image": keep_img, "restart": policy, "was_running": was_running,
                 "changed": [k for k, _, _ in wrong_env(c)]}
        rc, out = run(["docker", "create"] + args)
        ok, why = False, f"docker create: {out[-300:]}"
        if rc == 0 and not was_running:
            env = ((inspect(n) or {}).get("Config") or {}).get("Env") or []
            ok = not any(IP_RX.search(e.partition("=")[2]) for e in env)
            why = "made, left stopped as it was" if ok else "its settings still name an old address"
        elif rc == 0:
            rc2, out2 = run(["docker", "start", n])
            ok, why = healthy(n, a.ip, page_port(c)) if rc2 == 0 else (False, f"docker start: {out2[-300:]}")
        if ok:
            say(f"  made again with {a.ip}" + (" and started" if was_running else "") + f": {why}")
            for k in entry["changed"]:
                say(f"      {k}: now names {a.ip}")
            entry["new"] = "made"
            rec["containers"].append(entry)
            continue
        say(f"  !! the new one is not right ({why}): putting the old one back")
        failed = f"{n}-tvkit-failed-{a.ts}"
        run(["docker", "stop", "-t", "10", n], timeout=60)
        if inspect(n) is not None:
            run(["docker", "rename", n, failed])
            run(["docker", "update", "--restart=no", failed])
        run(["docker", "rename", old_name, n])
        run(["docker", "update", f"--restart={policy}", n])
        if was_running:
            run(["docker", "start", n])
        say(f"  put back as it was ({'running' if was_running else 'stopped'}); the new one is kept, stopped, as {failed}")
        rc_all = 1
    with open(os.path.join(rec_dir, "record.json"), "w") as f:
        json.dump(rec, f, indent=1)
    if rec["containers"]:
        with open(LAST, "w") as f:
            json.dump({"ts": a.ts, "dir": rec_dir}, f)
    say(f"record: {rec_dir}/record.json ({len(rec['containers'])} container(s) made again; undo: the kit's T2b)")
    return rc_all


def cmd_undo(a):
    if not os.path.isfile(LAST):
        say("nothing to undo: no fix by this kit is recorded here")
        return 0
    last = json.load(open(LAST))
    rec = json.load(open(os.path.join(last["dir"], "record.json")))
    only = [n for n in (a.names or "").split(",") if n]
    left = []
    rc_all = 0
    for e in rec["containers"]:
        n = e["name"]
        if only and n not in only:
            left.append(e)
            continue
        say(f"===== {n}: back to the container as it was before the fix ({e['old']})")
        if inspect(e["old"]) is None:
            say(f"  !! {e['old']} is not here any more: {n} stays as it is")
            left.append(e)
            rc_all = 1
            continue
        undone = f"{n}-tvkit-undone-{a.ts}"
        run(["docker", "stop", "-t", "20", n], timeout=120)
        if inspect(n) is not None:
            run(["docker", "rename", n, undone])
            run(["docker", "update", "--restart=no", undone])
        rc, out = run(["docker", "rename", e["old"], n])
        if rc != 0:
            say(f"  !! could not rename {e['old']} back: {out[-300:]}")
            rc_all = 1
            continue
        run(["docker", "update", f"--restart={e['restart']}", n])
        if e.get("was_running", True):
            rc, out = run(["docker", "start", n])
            say(f"  {n} is the old container again, " + ("started" if rc == 0 else f"!! not started: {out[-200:]}"))
            rc_all = rc_all or (0 if rc == 0 else 1)
        else:
            say(f"  {n} is the old container again (stopped, as it was)")
        say(f"  the fixed one is kept, stopped, as {undone}")
    rec["containers"] = left
    with open(os.path.join(last["dir"], "record.json"), "w") as f:
        json.dump(rec, f, indent=1)
    if not left:
        os.replace(LAST, os.path.join(last["dir"], "undone.json"))
    return rc_all


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["plan", "fix", "undo"])
    ap.add_argument("--ip", default="")
    ap.add_argument("--names", default="")
    ap.add_argument("--ts", default=time.strftime("%Y%m%d-%H%M%S", time.gmtime()))
    a = ap.parse_args()
    if a.names and not all(NAME_RX.match(n) for n in a.names.split(",") if n):
        say("!! --names: shared-browser containers (open77-tvbrowser...) only")
        return 2
    if a.mode in ("plan", "fix") and not re.match(r"^\d{1,3}(\.\d{1,3}){3}$", a.ip or ""):
        say("!! --ip: this machine's public IPv4 address")
        return 2
    if a.mode == "undo":
        return cmd_undo(a)
    return cmd_plan(a, fix=a.mode == "fix")


if __name__ == "__main__":
    sys.exit(main())
