#!/usr/bin/env python3
"""The room service, end to end, against the real thing.

Run on the game server as root:

    python3 room/test/e2e-room.py [staging|production|f2077]

It talks to the service on loopback (port 8002), so it sees exactly what the
room's Chromium sees, and reads the room's own DevTools socket to check that a
"open" really put the page on the screen. It creates, renames, resets and
deletes a temporary profile (never the active one), and leaves the room on its
splash page.

What it does not do is press keys: that is e2e-android-input.sh, which needs the
emulator.
"""

import glob
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROLE = sys.argv[1] if len(sys.argv) > 1 else "staging"
BASE = "http://127.0.0.1:8002"
CONTAINERS = {
    "staging": "open77-tvbrowser-xbs",
    "production": "open77-tvbrowser-xb",
    "f2077": "open77-tvbrowser-f2077",
}
CONTAINER = CONTAINERS.get(ROLE)
if CONTAINER is None:
    sys.exit("usage: e2e-room.py [staging|production|f2077]")

checks = 0
failures = 0


def check(label, ok, detail=None):
    global checks, failures
    checks += 1
    if ok:
        print(f"  ok   {label}")
    else:
        failures += 1
        print(f"  FAIL {label}" + (f"  -- {detail}" if detail is not None else ""))


def get(path, timeout=10):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as response:
        return response.status, response.read().decode("utf-8", "replace")


def post(body, timeout=30):
    data = json.dumps(body).encode()
    request = urllib.request.Request(BASE + "/api/action", data=data,
                                     headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, json.loads(response.read().decode())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read().decode())


def state():
    return json.loads(get(f"/api/state?room={ROLE}")[1])


def sh(*args):
    return subprocess.run(args, capture_output=True, text=True, timeout=30).stdout.strip()


def devtools_pages():
    ip = sh("docker", "inspect", "-f",
            "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", CONTAINER)
    if not ip:
        return []
    try:
        with urllib.request.urlopen(f"http://{ip}:9222/json/list", timeout=5) as response:
            return json.load(response)
    except Exception:
        return []


def room_url():
    pages = [t for t in devtools_pages() if t.get("type") == "page"]
    return pages[-1]["url"] if pages else None


def wait_for_url(fragment, seconds=25):
    for _ in range(seconds):
        url = room_url()
        if url and fragment in url:
            return url
        time.sleep(1)
    return None


def profile_names(payload):
    return [p["name"] for p in payload.get("profiles") or []]


def find(payload, name):
    for p in payload.get("profiles") or []:
        if p["name"] == name:
            return p
    return None


print(f"== the room service for {ROLE} ({CONTAINER})")

status, text = get("/api/status")
room = json.loads(text)["rooms"].get(ROLE) or {}
check("the status endpoint answers and names this room's container",
      status == 200 and room.get("container") == CONTAINER and room.get("running") is True, text[:200])

before = state()
active = before.get("profile")
check("the room has an active profile and a profile library to switch it",
      bool(active) and active in profile_names(before), active)
check("the state carries everything a screen draws: channels, services, sign-ins, Android",
      len(before.get("channels") or []) >= 4
      and any(c["id"] == "android" for c in before["channels"])
      and len(before.get("signin") or []) == 7
      and all(s.get("url", "").startswith("https://") for s in before["signin"])
      and "androidDevice" in before and "androidUp" in before)
android_channel = next(c for c in before["channels"] if c["id"] == "android")
check("the Android tile is the emulator's stream when it is up (keyboard capture included)",
      ("captureKeyboard=true" in android_channel["url"] and "action=stream" in android_channel["url"])
      if before.get("androidDevice") else android_channel["url"] == "/android",
      android_channel["url"][:120])

splash_status, splash = get(f"/?room={ROLE}")
profiles_status, profiles_page = get("/profiles")
check("the splash is a sign-in screen: a tile per service and the profile control",
      splash_status == 200 and "Sign in" in splash and 'id="signins"' in splash
      and 'id="channels"' in splash and 'id="profile"' in splash
      and "Manage profiles" in splash and "Start Android" in splash)
check("the profile library page lists profiles, details and the controls",
      profiles_status == 200 and "Rename" in profiles_page and "Reset" in profiles_page
      and "Delete" in profiles_page and "Details" in profiles_page)

name = "e2e" + time.strftime("%H%M%S")
code, value = post({"room": ROLE, "action": "create", "profile": name})
after = state()
check("a new profile is created on the host and listed",
      code == 200 and value.get("ok") is True and name in profile_names(after)
      and (find(after, name) or {}).get("dir", "").endswith("/" + name),
      value)

renamed = name + "b"
code, value = post({"room": ROLE, "action": "rename", "profile": name, "to": renamed})
after = state()
check("renaming moves the directory and the listing follows",
      code == 200 and renamed in profile_names(after) and name not in profile_names(after), value)
check("and the directory really is at the new name",
      os.path.isdir(os.path.join(find(after, renamed)["dir"])), find(after, renamed)["dir"])

code, value = post({"room": ROLE, "action": "reset", "profile": renamed})
after = state()
entry = find(after, renamed)
code2, value2 = post({"room": ROLE, "action": "delete", "profile": renamed})
after2 = state()
check("reset clears the sign-ins and keeps the old directory as a hidden backup",
      code == 200 and entry is not None and entry.get("cookies") == 0
      and bool(glob.glob(os.path.join(os.path.dirname(entry["dir"]), "." + renamed + ".reset-*"))),
      value)
check("delete removes it for good",
      code2 == 200 and renamed not in profile_names(after2) and not os.path.exists(entry["dir"]), value2)

check("a profile name that is not a name is refused, and so is an unknown action",
      post({"room": ROLE, "action": "create", "profile": "Not A Name"})[0] == 400
      and post({"room": ROLE, "action": "explode"})[0] == 400)

splash_url = f"http://172.17.0.1:8002/?room={ROLE}"
code, value = post({"room": ROLE, "action": "open", "url": splash_url})
check("opening a page puts it on the room's screen (a tab swap the television sees)",
      code == 200 and value.get("ok") is True and wait_for_url(f"8002/?room={ROLE}") is not None,
      value)

code, value = post({"room": ROLE, "action": "open", "url": "android"}, timeout=180)
url = wait_for_url("action=stream", seconds=40)
check("opening Android starts the emulator if needed and puts its stream on the screen",
      code == 200 and url is not None and "captureKeyboard=true" in url, (value, url))

post({"room": ROLE, "action": "open", "url": splash_url})
check("and the splash comes back on the same tab",
      wait_for_url(f"8002/?room={ROLE}") is not None)

print(f"\n{checks} checks, {failures} failed")
sys.exit(0 if failures == 0 else 1)
