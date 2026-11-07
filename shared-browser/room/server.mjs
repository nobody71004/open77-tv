"use strict";
// open77-room -- the page a shared browser opens on, the profile library behind
// it, and the one thing in the game that can drive the room: "open Android".
//
// Served on the game server, loopback and the Docker bridge only (port 8002):
// the room's Chromium reaches it at http://172.17.0.1:8002, and nothing off the
// host can. The same page is what a browser cinema shows the moment it is put
// up, because it is the room's start page (policies.json), not something the
// game has to inject.
//
// Rooms are the three shared-browser instances (see shared-browser/README.md).
// Each has its own profiles directory, mounted into its container at /profiles,
// and its Chromium runs with --user-data-dir=/profiles/active. "active" is a
// symlink in that directory: switching a profile is re-pointing it and letting
// supervisord restart Chromium, so cookies and sign-ins live on the host and a
// recreated container keeps them.

import http from "node:http";
import https from "node:https";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { execFileSync, execFile } from "node:child_process";
import { fileURLToPath } from "node:url";
import WebSocket from "ws";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const PORT = Number(process.env.PORT || 8002);
const WEB = path.join(__dirname, "web");

// role -> the container and the host directory that holds its profiles
const ROOMS = {
  production: { container: "open77-tvbrowser-xb",    dir: "/opt/open77-tvbrowser-xb",    label: "Production" },
  staging:    { container: "open77-tvbrowser-xbs",   dir: "/opt/open77-tvbrowser",       label: "Staging" },
  f2077:      { container: "open77-tvbrowser-f2077", dir: "/opt/open77-tvbrowser-f2077", label: "F2077" },
  "fr-us":    { container: "open77-tvbrowser-fr-us", dir: "/opt/open77-tvbrowser-fr-us", label: "Freeroam USA" },
};

// The game's watch-party API for each role, polled for "someone pressed
// Android". Empty means no game server for that room.
const GAME = {
  production: "https://xyberrp.duckdns.org/opx-watch-xb",
  staging:    "https://xyberrp.duckdns.org/opx-watch-xb-staging",
  f2077:      "",
  "fr-us":    "https://xyberrp.duckdns.org/opx-watch-fr-us",
};

// Where the Android screen is served, and which device it is. Two stacks run
// next to each other on this host and a room can be moved between them -- and
// back -- without a patch, because all of it is environment. The defaults are
// the emulator (Android 14 under qemu, port 8000, scrcpy's H.264 on ws-scrcpy);
// the native screen is Waydroid (Android 13 on this host's kernel, port 8001),
// which serves a VNC framebuffer through noVNC because this GPU-less VM cannot
// encode H.264 at all.
//   ANDROID_PORT      8000 | 8001
//   ANDROID_PAGE_PATH the page to show, when it is not ws-scrcpy's stream link
//                     (native: /open77-screen.html -- a canvas and nothing else,
//                     see ../../android-native/)
//   ANDROID_UDID      emulator-5554 | 192.168.240.112:5555
//   ANDROID_SERVICES  which units "put Android up" starts
//   ANDROID_LABEL     what the pages call it
const ANDROID_PORT = Number(process.env.ANDROID_PORT || 8000);
const ANDROID_PAGE_PATH = process.env.ANDROID_PAGE_PATH || "";
const ANDROID_UDID = process.env.ANDROID_UDID || "emulator-5554";
const ANDROID_SERVICES = (process.env.ANDROID_SERVICES || "open77-android-web.service open77-android-emulator.service")
  .split(/\s+/).filter(Boolean);
const ANDROID_LABEL = process.env.ANDROID_LABEL || "Android emulator";
const ANDROID_PAGE = `http://172.17.0.1:${ANDROID_PORT}/`;

// The screen mirror each room can show. The link -- relay page, channel and
// token -- is the game server's own opx_screenmirror config on this host, which
// is also the file the installers write, so the secret has one home. Read per
// request (tiny, and it changes only at deploy): a room whose server has no
// config simply has no mirror. The page never holds the link -- `/mirror`
// answers with a redirect -- so the token stays out of the room's HTML.
const MIRROR = {
  production: "/opt/open77-xbuniverse/resources/opx_screenmirror/server/config.lua",
  staging:    "/opt/open77-xbuniverse-staging/resources/opx_screenmirror/server/config.lua",
  f2077:      "/opt/open77-f2077/resources/opx_screenmirror/server/config.lua",
  "fr-us":    "/opt/open77-freeroamusa/resources/opx_screenmirror/server/config.lua",
};

function mirrorUrl(role) {
  const file = MIRROR[role];
  if (!file) return null;
  try {
    const found = /url\s*=\s*"([^"]+)"/.exec(fs.readFileSync(file, "utf8"));
    const link = found && found[1];
    if (!link || !/^https:\/\//.test(link) || /\s/.test(link)) return null;
    return link;
  } catch { return null; }
}

const SIGNIN_DOMAINS = {
  "Netflix": ["netflix.com"],
  "YouTube": ["youtube.com", "google.com"],
  "Prime Video": ["primevideo.com", "amazon.com"],
  "Disney+": ["disneyplus.com"],
  "Spotify": ["spotify.com"],
  "Twitch": ["twitch.tv"],
  "HBO Max": ["hbomax.com", "max.com"],
};

const CHANNELS = [
  { id: "chrome",  label: "Chrome",  url: "https://www.google.com/",           hint: "Browse the web" },
  { id: "netflix", label: "Netflix", url: "https://www.netflix.com/browse",  hint: "Films and series" },
  { id: "youtube", label: "YouTube", url: "https://www.youtube.com/",        hint: "Anything on the web" },
  { id: "prime",   label: "Prime Video", url: "https://www.primevideo.com/", hint: "Films and series" },
  { id: "android", label: "Android", url: "/android",                        hint: "Games, apps, Play Store" },
];

const started = Date.now();

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

function run(cmd, args, timeout = 20000) {
  return execFileSync(cmd, args, { encoding: "utf8", timeout, stdio: ["ignore", "pipe", "pipe"] }).trim();
}

function runAsync(cmd, args, timeout = 20000) {
  const began = Date.now();
  return new Promise((resolve) => {
    execFile(cmd, args, { encoding: "utf8", timeout }, (err, stdout, stderr) => {
      const took = Date.now() - began;
      if (took >= 3000) console.log(`[room] slow: ${cmd} ${args.slice(0, 3).join(" ")} took ${took} ms${err ? " and failed" : ""}`);
      resolve({ ok: !err, out: (stdout || "").trim(), err: (err && (stderr || err.message)) || "" });
    });
  });
}

// The same, for a step that must not go on when it fails.
async function runChecked(cmd, args, timeout = 20000) {
  const result = await runAsync(cmd, args, timeout);
  if (!result.ok) throw new Error((result.err || cmd + " failed").trim().split("\n").pop());
  return result.out;
}

// ---------------------------------------------------------------------------
// what the room asks the host, without stopping to wait for it (2026-11-07)
// ---------------------------------------------------------------------------
// Every answer below used to be a synchronous child process (execFileSync) on
// the request path: `adb devices` and `adb connect` (eight seconds whenever the
// native Android is down), `du` over every profile, `sqlite3` over every cookie
// jar, `docker inspect` and `docker exec ... supervisorctl status` for every room.
// Four televisions ask /api/state every five seconds, and while one answer was
// being worked out nothing else was served -- not the splash, not a tile's
// /api/action. With Waydroid down the line never emptied: measured on rl31 on
// 2026-11-07, GET / unanswered for minutes, and a restart did not help, because
// the televisions filled the line again at once. Now each answer is kept fresh in
// the background, one child at a time per question, by execFile, and a request
// reads what is known.
function kept(ttlMs, compute) {
  const values = new Map();   // key -> { value, at }
  const running = new Map();  // key -> the promise working it out
  const turns = new Map();    // key -> how often it was forgotten: an answer begun before a forget() is not kept
  function refresh(key) {
    if (running.has(key)) return running.get(key);
    const turn = turns.get(key) || 0;
    const promise = (async () => {
      await null;  // what follows runs once this promise is filed under its key
      let value;
      let worked = false;
      try {
        value = await compute(key);
        worked = true;
        if ((turns.get(key) || 0) === turn) values.set(key, { value, at: Date.now() });
      } catch { /* the last answer stands */ }
      finally { if (running.get(key) === promise) running.delete(key); }
      if (worked) return value;
      const hit = values.get(key);
      return hit ? hit.value : undefined;
    })();
    running.set(key, promise);
    return promise;
  }
  return {
    // What is known now (a stale answer starts a fresh one in the background).
    get(key, fallback) {
      const hit = values.get(key);
      if (!hit || Date.now() - hit.at > ttlMs) refresh(key);
      return hit ? hit.value : fallback;
    },
    // A fresh answer, waited for without blocking anything else.
    async fresh(key, fallback) {
      const hit = values.get(key);
      if (hit && Date.now() - hit.at <= ttlMs) return hit.value;
      const value = await refresh(key);
      return value === undefined ? fallback : value;
    },
    forget(key) {
      values.delete(key);
      running.delete(key);
      turns.set(key, (turns.get(key) || 0) + 1);
    },
  };
}

const sizes = kept(5 * 60 * 1000, async (dir) => {
  return Number((await runChecked("du", ["-sm", dir], 120000)).split(/\s+/)[0]) * 1024 * 1024;
});

function dirSize(dir) {
  return sizes.get(dir, 0);
}

function duHuman(bytes) {
  if (!bytes) return "-";
  if (bytes >= 1 << 30) return (bytes / (1 << 30)).toFixed(1) + " GB";
  return Math.round(bytes / (1 << 20)) + " MB";
}

function ago(ms) {
  if (!ms) return "-";
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (s < 90) return s + " s ago";
  const m = Math.round(s / 60);
  if (m < 90) return m + " min ago";
  const h = Math.round(m / 60);
  if (h < 48) return h + " h ago";
  return Math.round(h / 24) + " days ago";
}

function safeName(name) {
  return typeof name === "string" && /^[a-z0-9][a-z0-9_-]{0,31}$/.test(name) ? name : null;
}

function profileRoot(role) {
  const room = ROOMS[role];
  if (!room) return null;
  return path.join(room.dir, "profiles");
}

function activeProfile(role) {
  const root = profileRoot(role);
  if (!root) return null;
  try {
    const target = fs.readlinkSync(path.join(root, "active"));
    return path.basename(target);
  } catch { return null; }
}

function listProfiles(role) {
  const root = profileRoot(role);
  if (!root) return [];
  let names = [];
  try {
    names = fs.readdirSync(root, { withFileTypes: true })
      .filter((d) => d.isDirectory() && !d.name.startsWith("."))
      .map((d) => d.name)
      .sort();
  } catch { return []; }
  const active = activeProfile(role);
  return names.map((name) => {
    const dir = path.join(root, name);
    let stat = null;
    try { stat = fs.statSync(dir); } catch {}
    return {
      name,
      active: name === active,
      size: duHuman(dirSize(dir)),
      updated: stat ? ago(stat.mtimeMs) : "-",
      updatedAt: stat ? stat.mtimeMs : 0,
      dir,
      ...signedIn(dir),
    };
  });
}

// Which services have cookies in a profile. The Cookies database belongs to the
// running browser, so it is copied first and read from the copy -- opening the
// live one read-only still takes a lock the browser can trip over.
const jars = kept(60 * 1000, async (profileDir) => {
  const db = path.join(profileDir, "Default", "Cookies");
  if (!fs.existsSync(db)) return { services: [], cookies: 0 };
  const tmp = path.join(os.tmpdir(), "open77-cookies-" + process.pid + "-" + Math.random().toString(36).slice(2) + ".db");
  try {
    await fs.promises.copyFile(db, tmp);
    const rows = (await runChecked("sqlite3", [tmp, "select host_key from cookies;"], 8000))
      .split("\n").map((h) => h.trim().toLowerCase()).filter(Boolean);
    return {
      cookies: rows.length,
      services: Object.entries(SIGNIN_DOMAINS)
        .filter(([, domains]) => rows.some((h) => domains.some((d) => h === d || h.endsWith("." + d))))
        .map(([name]) => name),
    };
  } catch {
    return { services: [], cookies: 0 };
  } finally {
    try { fs.unlinkSync(tmp); } catch {}
  }
});

function signedIn(profileDir) {
  return jars.get(profileDir, { services: [], cookies: 0 });
}

const ips = kept(60 * 1000, async (container) => {
  const ip = await runChecked("docker", ["inspect", "-f",
    "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", container], 8000);
  if (!ip) throw new Error("no address");
  return ip;
});

const ups = kept(5000, async (container) => {
  const result = await runAsync("docker", ["inspect", "-f", "{{.State.Running}}", container], 8000);
  return result.ok && result.out === "true";
});

const chromes = kept(5000, async (role) => {
  if (!(await ups.fresh(ROOMS[role].container, false))) return "room down";
  const result = await runAsync("docker", ["exec", ROOMS[role].container, "supervisorctl", "status", "chromium"], 8000);
  return result.ok ? result.out.replace(/\s+/g, " ").trim() : "unknown";
});

async function containerIp(container) {
  return ips.fresh(container, null);
}

function containerUp(container) {
  return ups.get(container, false);
}

function chromiumState(role) {
  return chromes.get(role, "unknown");
}

// ---------------------------------------------------------------------------
// the room's own browser, over CDP
// ---------------------------------------------------------------------------

// The room's DevTools socket speaks plain CDP, and only what this file needs is
// used: list targets, open one, activate it, close the others. (puppeteer's
// connect() against the socat-forwarded endpoint reports no page targets at
// all, so the small client below is also the one that works.)
async function devtoolsBase(role) {
  const ip = await containerIp(ROOMS[role].container);
  if (!ip) throw new Error("room has no address");
  return `http://${ip}:9222`;
}

function httpJson(url, timeout = 5000) {
  return new Promise((resolve, reject) => {
    const req = http.get(url, { timeout }, (res) => {
      let body = "";
      res.on("data", (d) => { body += d; });
      res.on("end", () => {
        try { resolve(JSON.parse(body)); } catch (e) { reject(e); }
      });
    });
    req.on("error", reject);
    req.on("timeout", () => { req.destroy(new Error("timeout")); });
  });
}

async function roomTargets(role) {
  const list = await httpJson((await devtoolsBase(role)) + "/json/list");
  return Array.isArray(list) ? list : [];
}

async function currentTarget(role) {
  const targets = await roomTargets(role);
  const pages = targets.filter((t) => t.type === "page" && /^https?:/.test(t.url || ""));
  return pages[pages.length - 1] || null;
}

// This Chromium (154) answers /json/list and lets a browser-level client list
// and create targets, but refuses Target.attachToTarget ("Not allowed") and
// never replies on a page target's own socket. So the room is driven the way a
// person would: a new tab is opened on the URL, made the active tab (it is the
// window the television shows), and the tab it replaces is closed. One tab at a
// time keeps the stream on what was asked for and the profile's tab strip clean.
async function cdpSession(role, fn) {
  const version = await httpJson((await devtoolsBase(role)) + "/json/version");
  if (!version || !version.webSocketDebuggerUrl) throw new Error("the room has no DevTools socket");
  const socket = new WebSocket(version.webSocketDebuggerUrl, { handshakeTimeout: 8000 });
  await new Promise((resolve, reject) => {
    socket.on("open", resolve);
    socket.on("error", reject);
  });
  let next = 0;
  const waiters = new Map();
  socket.on("message", (raw) => {
    let message;
    try { message = JSON.parse(String(raw)); } catch { return; }
    const waiter = message.id != null && waiters.get(message.id);
    if (!waiter) return;
    waiters.delete(message.id);
    if (message.error) waiter.reject(new Error(message.error.message || "cdp error"));
    else waiter.resolve(message.result);
  });
  const send = (method, params = {}, timeout = 12000) => new Promise((resolve, reject) => {
    const id = ++next;
    const timer = setTimeout(() => { waiters.delete(id); reject(new Error("cdp timeout: " + method)); }, timeout);
    waiters.set(id, {
      resolve: (value) => { clearTimeout(timer); resolve(value); },
      reject: (error) => { clearTimeout(timer); reject(error); },
    });
    socket.send(JSON.stringify({ id, method, params }));
  });
  try { return await fn(send); } finally { try { socket.close(); } catch {} }
}

async function roomUrl(role) {
  try {
    const target = await currentTarget(role);
    return target ? target.url : null;
  } catch { return null; }
}

// Puts the URL on the room's screen: the tab that is open is swapped for one
// showing it. That tab is the window the television shows, so this is the whole
// of "put Android on the screen" -- no navigation, no page script, nothing the
// page can refuse.
async function openInRoom(role, url) {
  return cdpSession(role, async (send) => {
    const { targetInfos } = await send("Target.getTargets");
    const pages = (targetInfos || []).filter((t) => t.type === "page").map((t) => t.targetId);
    const created = await send("Target.createTarget", { url }, 20000);
    if (!created || !created.targetId) throw new Error("the room would not open a tab");
    await send("Target.activateTarget", { targetId: created.targetId });
    // The tab it replaces is closed only once the new one exists: closing the
    // last page of the window would take the room's Chromium down with it.
    for (const targetId of pages) {
      try { await send("Target.closeTarget", { targetId }); } catch { /* already gone */ }
    }
    return url;
  });
}

// ---------------------------------------------------------------------------
// profiles
// ---------------------------------------------------------------------------

async function switchProfile(role, name) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  const dir = path.join(root, name);
  if (!fs.existsSync(dir)) throw new Error("no profile called " + name);
  const link = path.join(root, "active");
  const tmp = link + ".new";
  try { fs.unlinkSync(tmp); } catch {}
  fs.symlinkSync(name, tmp, "dir");
  fs.renameSync(tmp, link);                 // atomic: the browser never sees no profile
  await runAsync("chown", ["-h", "1000:1000", link], 8000);
  // Chromium reads --user-data-dir once, at start: the restart is the switch.
  await runChecked("docker", ["exec", ROOMS[role].container, "supervisorctl", "restart", "chromium"], 30000);
  chromes.forget(role);
  return activeProfile(role);
}

async function createProfile(role, name) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  const dir = path.join(root, name);
  if (fs.existsSync(dir)) throw new Error("that profile already exists");
  fs.mkdirSync(dir, { recursive: true });
  await runChecked("chown", ["-R", "1000:1000", dir], 8000);
  return name;
}

// Renaming is a rename of the directory: the profile is the directory, and the
// active link follows it so the room keeps running on the same sign-ins.
async function renameProfile(role, from, to) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  const src = path.join(root, from);
  const dst = path.join(root, to);
  if (!fs.existsSync(src)) throw new Error("no profile called " + from);
  if (fs.existsSync(dst)) throw new Error("there is already a profile called " + to);
  const wasActive = activeProfile(role) === from;
  if (wasActive) await runChecked("docker", ["exec", ROOMS[role].container, "supervisorctl", "stop", "chromium"], 40000);
  fs.renameSync(src, dst);
  await runChecked("chown", ["-R", "1000:1000", dst], 8000);
  if (wasActive) {
    const link = path.join(root, "active");
    const tmp = link + ".new";
    try { fs.unlinkSync(tmp); } catch {}
    fs.symlinkSync(to, tmp, "dir");
    fs.renameSync(tmp, link);
    await runAsync("chown", ["-h", "1000:1000", link], 8000);
    await runChecked("docker", ["exec", ROOMS[role].container, "supervisorctl", "start", "chromium"], 40000);
    chromes.forget(role);
  }
  return { renamed: to };
}

async function resetProfile(role, name) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  if (activeProfile(role) === name) throw new Error("that profile is in use: switch to another first");
  const dir = path.join(root, name);
  if (!fs.existsSync(dir)) throw new Error("no profile called " + name);
  const stamp = new Date().toISOString().replace(/[-:T]/g, "").slice(0, 14);
  const bak = path.join(root, "." + name + ".reset-" + stamp);
  fs.renameSync(dir, bak);
  fs.mkdirSync(dir);
  await runChecked("chown", ["-R", "1000:1000", dir], 8000);
  sizes.forget(dir);
  jars.forget(dir);
  return { kept: path.basename(bak) };
}

async function deleteProfile(role, name) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  if (activeProfile(role) === name) throw new Error("that profile is in use: switch to another first");
  const dir = path.join(root, name);
  if (!fs.existsSync(dir)) throw new Error("no profile called " + name);
  // A Chromium profile is thousands of files: removed without holding up the screens.
  await fs.promises.rm(dir, { recursive: true, force: true });
  sizes.forget(dir);
  jars.forget(dir);
  return name;
}

// ---------------------------------------------------------------------------
// the Android screen
// ---------------------------------------------------------------------------

// The stream link ws-scrcpy's own device page would give you. That page builds
// the anchor in the browser (the HTML it serves is a 300-byte shell), so there is
// nothing to parse: the proxy is the page's own proxy-adb action and 8886 is the
// scrcpy server's port on the device, which is what the link always carried.
// `captureKeyboard` is not decoration: the player leaves keyboard capture off by
// default, so without it the mouse would play and nothing typed would arrive.
// Measured on 2026-10-22: no captureKeyboard, Settings' search field stays empty
// however much is typed; with it, the field takes the text and the results
// filter. The player also has a Capture-keyboard button in its side bar for when
// someone lands on it some other way.
function androidStreamUrl() {
  const base = "172.17.0.1:" + ANDROID_PORT;
  // A stack that serves its own page (the native screen's noVNC) needs no link
  // built here: the page takes the pointer and the keyboard itself.
  if (ANDROID_PAGE_PATH) return "http://" + base + ANDROID_PAGE_PATH;
  const udid = ANDROID_UDID;
  const proxy = "ws://" + base + "/?action=proxy-adb&remote=tcp%3A8886&udid=" + udid;
  return "http://" + base + "/#!action=stream&udid=" + udid +
    "&player=broadway&fitToScreen=true&captureKeyboard=true&ws=" + encodeURIComponent(proxy);
}

// The Android stack is a systemd pair on this host, not a container, so the
// question "is it up" is the page's own answer.
async function httpOk(url, timeout = 3000) {
  return await new Promise((resolve) => {
    const req = http.get(url, { timeout }, (res) => {
      res.resume();
      resolve(res.statusCode === 200);
    });
    req.on("error", () => resolve(false));
    req.on("timeout", () => { req.destroy(); resolve(false); });
  });
}

async function androidUp() {
  return httpOk(ANDROID_PAGE);
}

// Whether the configured device is actually booted, not just whether the
// screen's web server answers: ws-scrcpy is up before the device is, and a
// stream opened at that moment is a black rectangle. It is asked for by name --
// this host can have two Android devices on adb at once (the emulator and the
// native one), and "the first device adb lists" would put the wrong one on the
// television the moment both are running.
async function adbDevices() {
  const result = await runAsync("adb", ["devices"], 6000);
  if (!result.ok) return {};
  const rows = result.out.split("\n")
    .map((l) => l.trim()).filter((l) => l && !l.startsWith("List of devices"));
  const seen = {};
  for (const row of rows) {
    const [id, status] = row.split(/\s+/);
    if (id) seen[id] = status;
  }
  return seen;
}

// adb's answer, kept: asked again in the background when it is five seconds old.
// A device that lives on the network (Waydroid at host:port, not the emulator's
// local one) is forgotten by adb whenever anything restarts the adb server -- and
// the emulator's own stack starts one. Asking again is what `adb connect` is for;
// without it the room calls a running Android down for as long as nobody
// reconnects it. While that device is down a connect waits for its timeout, so it
// is tried at most every thirty seconds, and never on a request's own time --
// except while a start is waiting for it, when it is every five.
const adbKnown = { seen: {}, at: 0, connectAt: 0 };
let adbAsking = null;
function refreshAndroid(connectEveryMs = 30000) {
  if (adbAsking) return adbAsking;
  adbAsking = (async () => {
    try {
      let seen = await adbDevices();
      if (seen[ANDROID_UDID] !== "device" && /^[\w.-]+:\d+$/.test(ANDROID_UDID) && Date.now() - adbKnown.connectAt > connectEveryMs) {
        adbKnown.connectAt = Date.now();
        await runAsync("adb", ["connect", ANDROID_UDID], 8000);
        seen = await adbDevices();
      }
      adbKnown.seen = seen;
      adbKnown.at = Date.now();
    } finally {
      adbAsking = null;
    }
    return adbKnown.seen;
  })();
  return adbAsking;
}

function androidDevices() {
  if (Date.now() - adbKnown.at > 5000) refreshAndroid();
  return adbKnown.seen;
}

function androidDevice() {
  return androidDevices()[ANDROID_UDID] === "device" ? ANDROID_UDID : null;
}

async function androidDeviceNow(connectEveryMs) {
  await refreshAndroid(connectEveryMs);
  return adbKnown.seen[ANDROID_UDID] === "device" ? ANDROID_UDID : null;
}

// "Run the Android emulator" is a control, not an assumption: the game can ask
// for it when it is stopped, and the splash can start it again. One start at a
// time; the answer is the same for everyone asking meanwhile.
let androidStarting = null;
function startAndroid(timeoutMs = 150000) {
  if (androidStarting) return androidStarting;
  androidStarting = (async () => {
    try {
      if (await androidDeviceNow() && await androidUp()) return { ok: true, already: true, device: androidDevice() };
      await runChecked("systemctl", ["start", ...ANDROID_SERVICES], 60000);
      const until = Date.now() + timeoutMs;
      while (Date.now() < until) {
        if (await androidDeviceNow(5000) && await androidUp()) return { ok: true, device: androidDevice(), waited: Math.round((timeoutMs - (until - Date.now())) / 1000) };
        await new Promise((r) => setTimeout(r, 3000));
      }
      return { ok: false, error: "the Android screen did not come up in " + Math.round(timeoutMs / 1000) + " s" };
    } catch (error) {
      return { ok: false, error: error.message };
    } finally {
      androidStarting = null;
    }
  })();
  return androidStarting;
}

// ---------------------------------------------------------------------------
// "someone pressed Android in the game"
// ---------------------------------------------------------------------------

// The game answers GET /v1/pending with the newest Android request it has taken
// ({ id, by, at }). The first answer after the room starts is adopted as "seen"
// rather than acted on, so a restart does not replay an old request. Polling is
// two seconds: a person waiting for a screen.
//
// Requests are ordered by their own `at`, not by `id`. The game numbers them
// from a counter it keeps for the life of *its* process, so the id can go
// backwards when the game restarts -- and a room that compared ids alone then
// ignored every press until the counter caught up. Measured 2026-10-23: the game
// back at 2, the room holding 3, and a press doing nothing on the television.
// `at` is the request's timestamp, so it orders them across that restart too.
const lastPoll = {};
const seenAndroid = {};
const androidBusy = {};
// The game's public base is https and the room's own listeners are http: one
// client that picks by protocol, because node:http refuses an https URL outright
// (`Protocol "https:" not supported`), which is a poll that silently never ran.
function getText(url, timeout = 4000) {
  return new Promise((resolve, reject) => {
    const client = /^https:/i.test(url) ? https : http;
    const req = client.get(url, { timeout }, (res) => {
      let body = "";
      res.on("data", (d) => { body += d; });
      res.on("end", () => {
        if (res.statusCode !== 200) reject(new Error("HTTP " + res.statusCode));
        else resolve(body);
      });
    });
    req.on("error", reject);
    req.on("timeout", () => req.destroy(new Error("timeout")));
  });
}

async function pollGame(role) {
  const base = (GAME[role] || "").replace(/\/+$/, "");
  if (!base) return;
  lastPoll[role] = Date.now();
  try {
    const want = await getText(base + "/v1/pending");
    const parsed = JSON.parse(want || "{}");
    const id = Number(parsed && parsed.id) || 0;
    const at = Number(parsed && parsed.at) || 0;
    const seen = seenAndroid[role];
    if (seen === undefined) { seenAndroid[role] = { id, at }; return; }
    // `at` when the game gives one (it does); id alone when it does not, which
    // is the same rule an older game server could offer.
    const newer = (at && seen.at)
      ? (at > seen.at || (at === seen.at && id > seen.id))
      : id > seen.id;
    if (!newer || androidBusy[role]) return;
    seenAndroid[role] = { id, at };
    androidBusy[role] = true;
    try {
      if (!(await androidDeviceNow())) {
        console.log(`[room] ${role}: Android asked for by ${parsed.by || "the game"}; starting the emulator`);
        await startAndroid();
      }
      if (await androidDeviceNow()) {
        await openInRoom(role, androidStreamUrl());
        console.log(`[room] ${role}: Android on the screen (asked by ${parsed.by || "the game"})`);
      } else {
        await openInRoom(role, "http://172.17.0.1:8002/?room=" + role + "&android=down");
        console.log(`[room] ${role}: Android screen is not up; showed the splash instead`);
      }
    } finally {
      androidBusy[role] = false;
    }
  } catch { /* the game server may be down; the room is not */ }
}

// ---------------------------------------------------------------------------
// http
// ---------------------------------------------------------------------------

function send(res, status, body, type = "text/html; charset=utf-8") {
  res.writeHead(status, { "Content-Type": type, "Cache-Control": "no-store" });
  res.end(body);
}

function sendJson(res, value, status = 200) {
  send(res, status, JSON.stringify(value), "application/json; charset=utf-8");
}

function readBody(req) {
  return new Promise((resolve) => {
    let body = "";
    req.on("data", (d) => { body += d; if (body.length > 65536) req.destroy(); });
    req.on("end", () => {
      try { resolve(JSON.parse(body || "{}")); } catch { resolve({}); }
    });
  });
}

function page(res, file) {
  fs.readFile(path.join(WEB, file), (err, data) => {
    if (err) return send(res, 500, "missing " + file);
    send(res, 200, data);
  });
}

// The sign-in pages: one click from the splash into the page that asks, so the
// remote cannot be a dead end when a profile has no session yet.
const SIGNIN_URLS = {
  "Netflix": "https://www.netflix.com/login",
  "YouTube": "https://accounts.google.com/ServiceLogin?service=youtube",
  "Prime Video": "https://www.primevideo.com/auth/get-credentials",
  "Disney+": "https://www.disneyplus.com/login",
  "Spotify": "https://accounts.spotify.com/login",
  "Twitch": "https://www.twitch.tv/login",
  "HBO Max": "https://auth.max.com/login",
};

const stateKept = {};
const stateTurn = {};
const stateStats = { asked: 0, worked: 0, slowest: 0 };
// An action changed something a screen shows (a profile, the tab, Android): the
// next ask works the state out again, and an answer begun before the change is
// not kept. A page that acts and then asks sees what it did.
function stateChanged() {
  for (const role of Object.keys(ROOMS)) {
    stateTurn[role] = (stateTurn[role] || 0) + 1;
    delete stateKept[role];
  }
}
async function state(role) {
  stateStats.asked++;
  const known = stateKept[role];
  if (known && known.value && Date.now() - known.at < 2000) return known.value;
  if (known && known.running) return known.running;
  const turn = stateTurn[role] || 0;
  const began = Date.now();
  const running = stateNow(role).then((value) => {
    if ((stateTurn[role] || 0) === turn) stateKept[role] = { value, at: Date.now() };
    const took = Date.now() - began;
    stateStats.worked++;
    stateStats.slowest = Math.max(stateStats.slowest, took);
    if (took >= 2000) console.log(`[room] ${role}: the state took ${took} ms`);
    return value;
  }, (error) => {
    if ((stateTurn[role] || 0) === turn) stateKept[role] = known && known.value ? { value: known.value, at: known.at } : {};
    throw error;
  });
  stateKept[role] = { ...(known || {}), running };
  return running;
}

async function stateNow(role) {
  const active = activeProfile(role);
  const profiles = listProfiles(role);
  const up = await androidUp();
  const device = androidDevice();
  const android = up && device ? androidStreamUrl() : null;
  const mine = profiles.find((p) => p.name === active) || {};
  return {
    role,
    label: ROOMS[role].label,
    dir: ROOMS[role].dir,
    container: ROOMS[role].container,
    running: await ups.fresh(ROOMS[role].container, false),
    chromium: await chromes.fresh(role, "unknown"),
    profile: active,
    profiles,
    channels: CHANNELS.map((c) => ({ ...c, url: c.id === "android" ? (android || "/android") : c.url })),
    services: mine.services || [],
    signin: Object.entries(SIGNIN_URLS).map(([name, url]) => ({
      name, url, signedIn: (mine.services || []).indexOf(name) !== -1,
    })),
    androidUp: up,
    androidDevice: device,
    androidLabel: ANDROID_LABEL,
    androidPort: ANDROID_PORT,
    androidStarting: androidStarting !== null,
    mirror: mirrorUrl(role) !== null,
    roomUrl: await roomUrl(role),
    uptime: Math.round((Date.now() - started) / 1000),
    now: Date.now(),
  };
}

const served = { count: 0, slowest: 0, slowestPath: "", since: Date.now() };
const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, "http://room.invalid");
  const began = Date.now();
  res.on("finish", () => {
    const took = Date.now() - began;
    served.count++;
    if (took > served.slowest) { served.slowest = took; served.slowestPath = req.method + " " + url.pathname; }
    if (took >= 5000) console.log(`[room] slow answer: ${req.method} ${url.pathname} took ${took} ms`);
  });
  const role = ROOMS[url.searchParams.get("room")] ? url.searchParams.get("room") : "production";

  try {
    // The television's face is set in Inter; the room's Chromium has no such
    // family and falls back to DejaVu Sans, which reads nothing like a TV. One
    // small file, served from this directory, cached for a week.
    if (req.method === "GET" && url.pathname === "/inter.woff2") {
      res.writeHead(200, { "Content-Type": "font/woff2", "Cache-Control": "public, max-age=604800" });
      const stream = fs.createReadStream(path.join(WEB, "inter.woff2"));
      stream.on("error", () => res.end());
      return stream.pipe(res);
    }

    if (req.method === "GET" && (url.pathname === "/" || url.pathname === "/splash")) return page(res, "splash.html");
    if (req.method === "GET" && url.pathname === "/profiles") return page(res, "profiles.html");

    // The Mirror app: a redirect to the relay's viewer page for this room's
    // server. Nothing is cached and nothing is logged, because the location
    // carries the channel's token.
    if (req.method === "GET" && url.pathname === "/mirror") {
      const target = mirrorUrl(role);
      if (!target) return send(res, 404, "no screen mirror is configured for this room");
      res.writeHead(302, { Location: target, "Cache-Control": "no-store" });
      return res.end();
    }

    if (req.method === "GET" && url.pathname === "/android") {
      if (!androidDevice()) {
        // The tile is also how the emulator is started: asking for the screen
        // when it is down brings it up and then goes to it.
        startAndroid();
        return send(res, 202, "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=10>" +
          "<body style=\"background:#0a0d13;color:#e8eef7;font:16px system-ui;padding:40px\">" +
          "<h1 style=\"font-size:22px\">Starting Android</h1>" +
          "<p>This page reloads itself until the screen is up. A cold boot takes about a minute.</p>" +
          "<p><a style=\"color:#19dfeb\" href=\"/?room=" + role + "\">Back to the room</a></p></body>");
      }
      res.writeHead(302, { Location: androidStreamUrl(), "Cache-Control": "no-store" });
      return res.end();
    }

    if (req.method === "GET" && url.pathname === "/api/state") return sendJson(res, await state(role));

    if (req.method === "GET" && url.pathname === "/api/status") {
      // Asked by a person or a check, not by a screen on a timer: worked out fresh
      // (the rooms side by side), never from a cache that is still empty.
      const rooms = await Promise.all(Object.entries(ROOMS).map(async ([r, spec]) => [r, {
        container: spec.container, running: await ups.fresh(spec.container, false),
        profile: activeProfile(r), chromium: await chromes.fresh(r, "unknown"), polled: lastPoll[r] || null,
      }]));
      return sendJson(res, {
        ok: true,
        rooms: Object.fromEntries(rooms),
        androidUp: await androidUp(),
        androidDevice: androidDevice(),
        androidLabel: ANDROID_LABEL,
        androidPort: ANDROID_PORT,
      });
    }

    if (req.method === "POST" && url.pathname === "/api/action") {
      const body = await readBody(req);
      const target = ROOMS[body.room] ? body.room : role;
      const action = String(body.action || "");
      try {
        if (action === "use") {
          const name = safeName(body.profile);
          if (!name) throw new Error("bad profile name");
          await switchProfile(target, name);
          return sendJson(res, { ok: true, active: activeProfile(target) });
        }
        if (action === "create") {
          const name = safeName(body.profile);
          if (!name) throw new Error("a profile name is lower-case letters, digits, - and _ (max 32)");
          await createProfile(target, name);
          return sendJson(res, { ok: true, created: name });
        }
        if (action === "reset") {
          const name = safeName(body.profile);
          if (!name) throw new Error("bad profile name");
          return sendJson(res, { ok: true, ...(await resetProfile(target, name)) });
        }
        if (action === "delete") {
          const name = safeName(body.profile);
          if (!name) throw new Error("bad profile name");
          return sendJson(res, { ok: true, deleted: await deleteProfile(target, name) });
        }
        if (action === "rename") {
          const from = safeName(body.profile);
          const to = safeName(body.to);
          if (!from || !to) throw new Error("names are lower-case letters, digits, - and _ (max 32)");
          return sendJson(res, { ok: true, ...(await renameProfile(target, from, to)) });
        }
        if (action === "open") {
          const wanted = String(body.url || "");
          if (wanted === "android") {
            if (!(await androidDeviceNow())) {
              const started = await startAndroid();
              if (!started.ok) throw new Error(started.error || "the Android screen did not start");
            }
            return sendJson(res, { ok: true, url: await openInRoom(target, androidStreamUrl()) });
          }
          if (!/^https?:\/\//.test(wanted)) throw new Error("only http(s) URLs");
          return sendJson(res, { ok: true, url: await openInRoom(target, wanted) });
        }
        if (action === "android-start") {
          return sendJson(res, { ok: true, ...(await startAndroid()) });
        }
        if (action === "restart") {
          // The splash's own "reload": a wedged room comes back on the same tab
          // and the same profile, which is what the television is pointed at.
          await runChecked("docker", ["exec", ROOMS[target].container, "supervisorctl", "restart", "chromium"], 40000);
          chromes.forget(target);
          return sendJson(res, { ok: true, restarted: true });
        }
        throw new Error("unknown action");
      } catch (error) {
        return sendJson(res, { ok: false, error: error.message }, 400);
      } finally {
        stateChanged();
      }
    }

    send(res, 404, "not found");
  } catch (error) {
    console.error("[room]", error);
    send(res, 500, "room error: " + error.message);
  }
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`[room] listening on ${PORT}: splash, /profiles, /android, /api/*`);
  // What the screens will ask first, asked now, so the first answers are not empty.
  refreshAndroid();
  for (const [role, spec] of Object.entries(ROOMS)) {
    ups.get(spec.container, false);
    chromes.get(role, "unknown");
  }
});

// The game's pending queue: a browser cinema was spawned and asked for Android.
// Two seconds is a person waiting for a screen, not a hot loop.
setInterval(() => {
  for (const role of Object.keys(ROOMS)) {
    if (containerUp(ROOMS[role].container)) pollGame(role);
  }
}, 2000);

// Once a minute, when anything was asked: how many answers, the slowest, and the
// event loop's own lag (how late a 1 s timer fired) -- the line that says whether
// the room is keeping up.
let lagMax = 0;
let lagLast = Date.now();
setInterval(() => {
  const now = Date.now();
  lagMax = Math.max(lagMax, now - lagLast - 1000);
  lagLast = now;
}, 1000);
setInterval(() => {
  if (served.count === 0 && stateStats.asked === 0) { lagMax = 0; return; }
  console.log(`[room] last minute: ${served.count} answer(s), the slowest ${served.slowest} ms (${served.slowestPath || "-"}); ` +
    `state asked ${stateStats.asked}, worked out ${stateStats.worked}, slowest ${stateStats.slowest} ms; ` +
    `event loop late by at most ${lagMax} ms; Android ${androidDevice() ? "up" : "down"} (adb asked ${adbKnown.at ? Math.round((Date.now() - adbKnown.at) / 1000) + " s ago" : "not yet"})`);
  served.count = 0; served.slowest = 0; served.slowestPath = "";
  stateStats.asked = 0; stateStats.worked = 0; stateStats.slowest = 0;
  lagMax = 0;
}, 60000);
