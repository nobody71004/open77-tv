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
};

// The game's watch-party API for each role, polled for "someone pressed
// Android". Empty means no game server for that room.
const GAME = {
  production: "https://xbuniverse.duckdns.org/opx-watch-xb",
  staging:    "https://xbuniverse.duckdns.org/opx-watch-xb-staging",
  f2077:      "",
};

// Where the Android screen is served (the emulator stack next to this one).
const ANDROID_PAGE = "http://172.17.0.1:8000/";

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
  return new Promise((resolve) => {
    execFile(cmd, args, { encoding: "utf8", timeout }, (err, stdout, stderr) => {
      resolve({ ok: !err, out: (stdout || "").trim(), err: (err && (stderr || err.message)) || "" });
    });
  });
}

function dirSize(dir) {
  try { return Number(run("du", ["-sm", dir]).split(/\s+/)[0]) * 1024 * 1024; }
  catch { return 0; }
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
function signedIn(profileDir) {
  const db = path.join(profileDir, "Default", "Cookies");
  if (!fs.existsSync(db)) return { services: [], cookies: 0 };
  const tmp = path.join(os.tmpdir(), "open77-cookies-" + process.pid + "-" + Math.random().toString(36).slice(2) + ".db");
  try {
    fs.copyFileSync(db, tmp);
    const rows = run("sqlite3", [tmp, "select host_key from cookies;"], 8000)
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
}

function containerIp(container) {
  try {
    return run("docker", ["inspect", "-f",
      "{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}", container], 8000);
  } catch { return null; }
}

function containerUp(container) {
  try {
    return run("docker", ["inspect", "-f", "{{.State.Running}}", container], 8000) === "true";
  } catch { return false; }
}

function chromiumState(role) {
  if (!containerUp(ROOMS[role].container)) return "room down";
  try {
    const line = run("docker", ["exec", ROOMS[role].container, "supervisorctl", "status", "chromium"], 8000);
    return line.replace(/\s+/g, " ").trim();
  } catch { return "unknown"; }
}

// ---------------------------------------------------------------------------
// the room's own browser, over CDP
// ---------------------------------------------------------------------------

// The room's DevTools socket speaks plain CDP, and only what this file needs is
// used: list targets, open one, activate it, close the others. (puppeteer's
// connect() against the socat-forwarded endpoint reports no page targets at
// all, so the small client below is also the one that works.)
function devtoolsBase(role) {
  const ip = containerIp(ROOMS[role].container);
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
  const list = await httpJson(devtoolsBase(role) + "/json/list");
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
  const version = await httpJson(devtoolsBase(role) + "/json/version");
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

function switchProfile(role, name) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  const dir = path.join(root, name);
  if (!fs.existsSync(dir)) throw new Error("no profile called " + name);
  const link = path.join(root, "active");
  const tmp = link + ".new";
  try { fs.unlinkSync(tmp); } catch {}
  fs.symlinkSync(name, tmp, "dir");
  fs.renameSync(tmp, link);                 // atomic: the browser never sees no profile
  try { run("chown", ["-h", "1000:1000", link], 8000); } catch {}
  // Chromium reads --user-data-dir once, at start: the restart is the switch.
  run("docker", ["exec", ROOMS[role].container, "supervisorctl", "restart", "chromium"], 30000);
  return activeProfile(role);
}

function createProfile(role, name) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  const dir = path.join(root, name);
  if (fs.existsSync(dir)) throw new Error("that profile already exists");
  fs.mkdirSync(dir, { recursive: true });
  run("chown", ["-R", "1000:1000", dir], 8000);
  return name;
}

// Renaming is a rename of the directory: the profile is the directory, and the
// active link follows it so the room keeps running on the same sign-ins.
function renameProfile(role, from, to) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  const src = path.join(root, from);
  const dst = path.join(root, to);
  if (!fs.existsSync(src)) throw new Error("no profile called " + from);
  if (fs.existsSync(dst)) throw new Error("there is already a profile called " + to);
  const wasActive = activeProfile(role) === from;
  if (wasActive) run("docker", ["exec", ROOMS[role].container, "supervisorctl", "stop", "chromium"], 40000);
  fs.renameSync(src, dst);
  run("chown", ["-R", "1000:1000", dst], 8000);
  if (wasActive) {
    const link = path.join(root, "active");
    const tmp = link + ".new";
    try { fs.unlinkSync(tmp); } catch {}
    fs.symlinkSync(to, tmp, "dir");
    fs.renameSync(tmp, link);
    try { run("chown", ["-h", "1000:1000", link], 8000); } catch {}
    run("docker", ["exec", ROOMS[role].container, "supervisorctl", "start", "chromium"], 40000);
  }
  return { renamed: to };
}

function resetProfile(role, name) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  if (activeProfile(role) === name) throw new Error("that profile is in use: switch to another first");
  const dir = path.join(root, name);
  if (!fs.existsSync(dir)) throw new Error("no profile called " + name);
  const stamp = new Date().toISOString().replace(/[-:T]/g, "").slice(0, 14);
  const bak = path.join(root, "." + name + ".reset-" + stamp);
  fs.renameSync(dir, bak);
  fs.mkdirSync(dir);
  run("chown", ["-R", "1000:1000", dir], 8000);
  return { kept: path.basename(bak) };
}

function deleteProfile(role, name) {
  const root = profileRoot(role);
  if (!root) throw new Error("no such room");
  if (activeProfile(role) === name) throw new Error("that profile is in use: switch to another first");
  const dir = path.join(root, name);
  if (!fs.existsSync(dir)) throw new Error("no profile called " + name);
  fs.rmSync(dir, { recursive: true, force: true });
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
  const udid = process.env.ANDROID_UDID || "emulator-5554";
  const proxy = "ws://172.17.0.1:8000/?action=proxy-adb&remote=tcp%3A8886&udid=" + udid;
  return "http://172.17.0.1:8000/#!action=stream&udid=" + udid +
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

// Whether the emulator is actually booted, not just whether the screen's web
// server answers: ws-scrcpy is up before the device is, and a stream opened at
// that moment is a black rectangle.
function androidDevice() {
  try {
    const line = run("adb", ["devices"], 6000).split("\n")
      .map((l) => l.trim()).find((l) => l && !l.startsWith("List of devices"));
    if (!line) return null;
    const [id, status] = line.split(/\s+/);
    return status === "device" ? id : null;
  } catch { return null; }
}

// "Run the Android emulator" is a control, not an assumption: the game can ask
// for it when it is stopped, and the splash can start it again. One start at a
// time; the answer is the same for everyone asking meanwhile.
let androidStarting = null;
function startAndroid(timeoutMs = 150000) {
  if (androidStarting) return androidStarting;
  androidStarting = (async () => {
    try {
      if (androidDevice() && await androidUp()) return { ok: true, already: true, device: androidDevice() };
      run("systemctl", ["start", "open77-android-web.service", "open77-android-emulator.service"], 60000);
      const until = Date.now() + timeoutMs;
      while (Date.now() < until) {
        if (androidDevice() && await androidUp()) return { ok: true, device: androidDevice(), waited: Math.round((timeoutMs - (until - Date.now())) / 1000) };
        await new Promise((r) => setTimeout(r, 3000));
      }
      return { ok: false, error: "the emulator did not come up in " + Math.round(timeoutMs / 1000) + " s" };
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
// ({ id, by, at }): id only ever goes up, so a room that restarts does not
// replay an old one, and the first answer after a restart is adopted as "seen"
// rather than acted on. Polling is two seconds: a person waiting for a screen.
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
    if (seenAndroid[role] === undefined) { seenAndroid[role] = id; return; }
    if (id <= seenAndroid[role] || androidBusy[role]) return;
    seenAndroid[role] = id;
    androidBusy[role] = true;
    try {
      if (!androidDevice()) {
        console.log(`[room] ${role}: Android asked for by ${parsed.by || "the game"}; starting the emulator`);
        await startAndroid();
      }
      if (androidDevice()) {
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

async function state(role) {
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
    running: containerUp(ROOMS[role].container),
    chromium: chromiumState(role),
    profile: active,
    profiles,
    channels: CHANNELS.map((c) => ({ ...c, url: c.id === "android" ? (android || "/android") : c.url })),
    services: mine.services || [],
    signin: Object.entries(SIGNIN_URLS).map(([name, url]) => ({
      name, url, signedIn: (mine.services || []).indexOf(name) !== -1,
    })),
    androidUp: up,
    androidDevice: device,
    androidStarting: androidStarting !== null,
    roomUrl: await roomUrl(role),
    uptime: Math.round((Date.now() - started) / 1000),
    now: Date.now(),
  };
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, "http://room.invalid");
  const role = ROOMS[url.searchParams.get("room")] ? url.searchParams.get("room") : "production";

  try {
    if (req.method === "GET" && (url.pathname === "/" || url.pathname === "/splash")) return page(res, "splash.html");
    if (req.method === "GET" && url.pathname === "/profiles") return page(res, "profiles.html");

    if (req.method === "GET" && url.pathname === "/android") {
      if (!androidDevice()) {
        // The tile is also how the emulator is started: asking for the screen
        // when it is down brings it up and then goes to it.
        startAndroid();
        return send(res, 202, "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content=10>" +
          "<body style=\"background:#0a0d13;color:#e8eef7;font:16px system-ui;padding:40px\">" +
          "<h1 style=\"font-size:22px\">Starting the Android emulator</h1>" +
          "<p>This page reloads itself until the screen is up. A cold boot takes about a minute.</p>" +
          "<p><a style=\"color:#19dfeb\" href=\"/?room=" + role + "\">Back to the room</a></p></body>");
      }
      res.writeHead(302, { Location: androidStreamUrl(), "Cache-Control": "no-store" });
      return res.end();
    }

    if (req.method === "GET" && url.pathname === "/api/state") return sendJson(res, await state(role));

    if (req.method === "GET" && url.pathname === "/api/status") {
      return sendJson(res, {
        ok: true,
        rooms: Object.fromEntries(Object.entries(ROOMS).map(([r, spec]) => [r, {
          container: spec.container, running: containerUp(spec.container),
          profile: activeProfile(r), chromium: chromiumState(r), polled: lastPoll[r] || null,
        }])),
        androidUp: await androidUp(),
        androidDevice: androidDevice(),
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
          switchProfile(target, name);
          return sendJson(res, { ok: true, active: activeProfile(target) });
        }
        if (action === "create") {
          const name = safeName(body.profile);
          if (!name) throw new Error("a profile name is lower-case letters, digits, - and _ (max 32)");
          createProfile(target, name);
          return sendJson(res, { ok: true, created: name });
        }
        if (action === "reset") {
          const name = safeName(body.profile);
          if (!name) throw new Error("bad profile name");
          return sendJson(res, { ok: true, ...resetProfile(target, name) });
        }
        if (action === "delete") {
          const name = safeName(body.profile);
          if (!name) throw new Error("bad profile name");
          return sendJson(res, { ok: true, deleted: deleteProfile(target, name) });
        }
        if (action === "rename") {
          const from = safeName(body.profile);
          const to = safeName(body.to);
          if (!from || !to) throw new Error("names are lower-case letters, digits, - and _ (max 32)");
          return sendJson(res, { ok: true, ...renameProfile(target, from, to) });
        }
        if (action === "open") {
          const wanted = String(body.url || "");
          if (wanted === "android") {
            if (!androidDevice()) {
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
          run("docker", ["exec", ROOMS[target].container, "supervisorctl", "restart", "chromium"], 40000);
          return sendJson(res, { ok: true, restarted: true });
        }
        throw new Error("unknown action");
      } catch (error) {
        return sendJson(res, { ok: false, error: error.message }, 400);
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
});

// The game's pending queue: a browser cinema was spawned and asked for Android.
// Two seconds is a person waiting for a screen, not a hot loop.
setInterval(() => {
  for (const role of Object.keys(ROOMS)) {
    if (containerUp(ROOMS[role].container)) pollGame(role);
  }
}, 2000);
