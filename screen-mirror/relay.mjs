// open77 screen-mirror relay. Runs on a VPS (or any host both ends can reach).
//
// With the WebRTC transport the media does NOT flow through here any more: the
// picture (H.264) and sound (Opus) travel directly sender -> receiver over
// WebRTC, relayed by the VPS's TURN server only when the two ends cannot reach
// each other directly. This process is the SIGNALING channel and the static
// host: it serves the sender/receiver pages, hands out the ICE (STUN/TURN)
// config, and forwards the small SDP/ICE messages the two ends need to find and
// connect to each other. It never sees a frame or an audio sample.
//
//   node relay.mjs                 # http on PORT (default 8787)
//   WS  /ws?channel=NAME&role=sender|receiver
//   GET /            -> sender page      GET /receiver -> receiver page
//   GET /ice         -> { iceServers }   GET /health   -> { ok, channels }
//
// Env for the ICE config (what the browser needs to reach the TURN server):
//   STUN_URL   default stun:xbuniverse.duckdns.org:3478  ("" to disable)
//   TURN_URL   e.g. turn:xbuniverse.duckdns.org:3478?transport=udp  ("" to disable)
//   TURN_USER / TURN_PASS   the TURN long-term credential
//
// This is a view-only mirror. Picture and sound move sender -> receiver only.
// No input, keystrokes or mouse events travel the other way.

import http from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join, normalize } from "node:path";
import { WebSocketServer } from "ws";
import { createHmac, timingSafeEqual } from "node:crypto";

const HERE = dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT || 8787);
// Optional shared secret. If MIRROR_TOKEN is set, a client must present the same
// ?token= to join a channel. On a public VPS, set it: the channel name alone is
// then not enough to watch or publish.
const TOKEN = process.env.MIRROR_TOKEN || "";
// Bind to loopback by default so the relay is reachable only through the
// fronting proxy (nginx) on HTTPS/WSS, not directly on the public host.
const HOST = process.env.HOST || "127.0.0.1";

function log(...a) { console.log("[screen-mirror]", new Date().toISOString(), ...a); }

// ---- Token auth -----------------------------------------------------------
// Two ways to authenticate a join, both keyed off MIRROR_TOKEN (the secret):
//   1. plain shared secret:  token === TOKEN                          (simple)
//   2. signed scoped token:  <channel>.<role>.<expiry>.<b64url-hmac>
//      hmac = HMAC-SHA256(TOKEN, "<channel>.<role>.<expiry>")
// A signed token is bound to one channel and role and can expire, so a leaked
// link stops working and senders are told apart. Mint one with `node token.mjs
// mint`. The plain shared-secret match still works. If MIRROR_TOKEN is unset,
// auth is disabled entirely (open relay) -- set it on a public host.
function signToken(secret, ch, role, exp) {
  return createHmac("sha256", secret).update(`${ch}.${role}.${exp}`).digest("base64url");
}
function validSignedToken(token, ch, role) {
  if (!TOKEN) return true;
  const parts = String(token).split(".");
  if (parts.length !== 4) return false;
  const [tch, trole, exp, sig] = parts;
  if (tch !== ch) return false;
  if (trole !== "*" && trole !== role) return false;
  const expiry = Number(exp) || 0;
  if (expiry && Date.now() / 1000 > expiry) return false;
  const expect = signToken(TOKEN, tch, trole, exp);
  const a = Buffer.from(expect), b = Buffer.from(String(sig));
  return a.length === b.length && timingSafeEqual(a, b);
}
function authOk(token, ch, role) {
  if (!TOKEN) return true;
  return token === TOKEN || validSignedToken(token, ch, role);
}

// ---- Watchdog -------------------------------------------------------------
// A sender that keeps failing auth (bad or expired token) instead of streaming
// is a silent failure: the TV shows nothing and no one knows why. Track auth
// failures per channel+role and raise an alert when they flap past a threshold
// within a window, so the operator hears about it instead of guessing.
const authFails = new Map(); // "channel:role" -> {count, windowStart, last}
const watchdogAlerts = [];   // recent alerts, surfaced on /health
const WATCHDOG_FAILS = Number(process.env.WATCHDOG_FAILS || 5);
const WATCHDOG_WINDOW_MS = Number(process.env.WATCHDOG_WINDOW_MS || 60000);
function recordAuthFail(role, ch) {
  const key = ch + ":" + role;
  const now = Date.now();
  let e = authFails.get(key);
  if (!e || now - e.windowStart > WATCHDOG_WINDOW_MS) e = { count: 0, windowStart: now };
  e.count += 1; e.last = now;
  authFails.set(key, e);
  if (e.count === WATCHDOG_FAILS) {
    const msg = role + " on channel " + ch + " is flapping auth_failed (" + e.count + " in " +
      (WATCHDOG_WINDOW_MS / 1000) + "s) instead of streaming - check its token";
    log("WATCHDOG", msg);
    watchdogAlerts.push({ ts: new Date().toISOString(), role, channel: ch, count: e.count, msg });
    if (watchdogAlerts.length > 50) watchdogAlerts.shift();
    const hook = process.env.ALERT_WEBHOOK;
    if (hook) fetch(hook, { method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ text: msg }) }).catch(() => {});
  }
}

// The ICE config the browser needs to gather candidates: STUN to learn its own
// reflexive address, and TURN (on the VPS) to relay the media when a direct path
// is blocked. Built from env so the TURN secret never lands in the served pages.
function iceServers() {
  const out = [];
  const stun = "stun" in process.env ? process.env.STUN_URL : "stun:xbuniverse.duckdns.org:3478";
  if (stun) out.push({ urls: stun });
  const turn = process.env.TURN_URL || "";
  if (turn) {
    const e = { urls: turn };
    if (process.env.TURN_USER) e.username = process.env.TURN_USER;
    if (process.env.TURN_PASS) e.credential = process.env.TURN_PASS;
    out.push(e);
  }
  return out;
}

// channel name -> { sender: ws|null, peers: Map<id, ws> }
const channels = new Map();
const clients = new Map(); // peer id -> ws (for routing signal{to})
let nextId = 1;

function channel(name) {
  let c = channels.get(name);
  if (!c) { c = { sender: null, peers: new Map() }; channels.set(name, c); }
  return c;
}
function dropEmpty(name) {
  const c = channels.get(name);
  if (c && c.peers.size === 0) channels.delete(name);
}
function send(ws, obj) {
  if (ws && ws.readyState === 1) { try { ws.send(JSON.stringify(obj)); } catch {} }
}

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
};

async function serveFile(res, rel, status = 200) {
  // normalize + strip leading separators so a path can't walk out of HERE.
  const safe = normalize(rel).replace(/^([/\\])+/, "");
  const path = join(HERE, safe);
  if (!path.startsWith(HERE)) { res.writeHead(403).end("forbidden"); return; }
  try {
    const body = await readFile(path);
    res.writeHead(status, {
      "content-type": MIME[path.slice(path.lastIndexOf("."))] || "application/octet-stream",
      "cache-control": "no-store",
    });
    res.end(body);
  } catch {
    res.writeHead(404, { "content-type": "text/plain" }).end("not found");
  }
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  const p = url.pathname;
  if (p === "/health") {
    res.writeHead(200, { "content-type": "application/json", "cache-control": "no-store" });
    res.end(JSON.stringify({
      ok: true,
      channels: [...channels.entries()].map(([name, c]) => ({
        name, sender: !!c.sender, peers: c.peers.size,
      })),
      watchdog: {
        threshold: WATCHDOG_FAILS, windowMs: WATCHDOG_WINDOW_MS,
        alerts: watchdogAlerts.slice(-10),
      },
    }));
    return;
  }
  if (p === "/ice") {
    // ACAO * so the Electron agent (a file:// origin) can fetch this too. The
    // endpoint only publishes the STUN/TURN routing config, nothing per-user.
    res.writeHead(200, {
      "content-type": "application/json",
      "cache-control": "no-store",
      "access-control-allow-origin": "*",
    });
    res.end(JSON.stringify({ iceServers: iceServers() }));
    return;
  }
  if (p === "/" ) { await serveFile(res, "sender.html"); return; }
  if (p === "/receiver" || p === "/receiver.html") { await serveFile(res, "receiver.html"); return; }
  if (p === "/sender" || p === "/sender.html") { await serveFile(res, "sender.html"); return; }
  if (p === "/relay.mjs") { await serveFile(res, "relay.mjs"); return; }
  res.writeHead(404, { "content-type": "text/plain" }).end("not found");
});

const wss = new WebSocketServer({ server, path: "/ws" });

wss.on("connection", (ws) => {
  ws.id = "p" + nextId++;
  ws.channelName = null;
  ws.role = null;
  ws.isAlive = true;
  ws.on("pong", () => { ws.isAlive = true; });

  ws.on("message", (raw) => {
    let msg;
    try { msg = JSON.parse(String(raw)); } catch { return; }

    if (msg.type === "join") {
      const name = String(msg.channel || "default").replace(/[^a-z0-9_-]/gi, "").slice(0, 64) || "default";
      const role = msg.role === "sender" ? "sender" : "receiver";
      if (!authOk(msg.token, name, role)) {
        log("auth_failed", "role=" + role, "channel=" + name,
            "token=" + (msg.token ? "present" : "MISSING"));
        recordAuthFail(role, name);
        send(ws, { type: "auth_failed" });
        try { ws.close(4001, "bad token"); } catch {}
        return;
      }
      ws.channelName = name;
      ws.role = role;
      const c = channel(name);
      c.peers.set(ws.id, ws);
      clients.set(ws.id, ws);
      log("join", role, name, ws.id);
      send(ws, { type: "joined", id: ws.id, role });

      if (role === "sender") {
        // One sender per channel; a reconnecting sender supersedes the old one.
        if (c.sender && c.sender !== ws && c.sender.readyState === 1) {
          log("supersede", name);
          send(c.sender, { type: "superseded" });
          try { c.sender.close(); } catch {}
        }
        c.sender = ws;
        // Tell the sender about every receiver already waiting, and each of them
        // who the sender is, so the sender can open a peer connection per viewer.
        for (const [rid, r] of c.peers) {
          if (r === ws) continue;
          if (r.role === "receiver") {
            send(ws, { type: "peer", id: rid, role: "receiver" });
            send(r, { type: "peer", id: ws.id, role: "sender" });
            send(r, { type: "sender", online: true });
          }
        }
      } else {
        // A new viewer: tell the sender (if any) so it offers this viewer, and
        // tell the viewer whether a sender is up.
        if (c.sender && c.sender.readyState === 1) {
          send(c.sender, { type: "peer", id: ws.id, role: "receiver" });
          send(ws, { type: "peer", id: c.sender.id, role: "sender" });
          send(ws, { type: "sender", online: true });
        } else {
          send(ws, { type: "sender", online: false });
        }
      }
      return;
    }

    if (msg.type === "signal") {
      // Route an SDP offer/answer or ICE candidate to one peer in the channel.
      const target = clients.get(String(msg.to));
      if (target && target.readyState === 1 && target.channelName === ws.channelName) {
        send(target, { type: "signal", from: ws.id, payload: msg.payload });
      }
      return;
    }

    if (msg.type === "ping") { send(ws, { type: "pong" }); return; }
  });

  ws.on("close", () => {
    const name = ws.channelName;
    log("close", ws.role, name, ws.id);
    clients.delete(ws.id);
    const c = name && channels.get(name);
    if (!c) return;
    c.peers.delete(ws.id);
    if (ws.role === "sender" && c.sender === ws) {
      c.sender = null;
      for (const r of c.peers.values()) send(r, { type: "sender", online: false });
    }
    // Tell everyone left who dropped, so the other end can tear its connection down.
    for (const r of c.peers.values()) send(r, { type: "peer_left", id: ws.id });
    dropEmpty(name);
  });
});

// Liveness sweep so dead sockets don't hold channels open.
const sweep = setInterval(() => {
  for (const ws of wss.clients) {
    if (!ws.isAlive) { try { ws.terminate(); } catch {} continue; }
    ws.isAlive = false;
    try { ws.ping(); } catch {}
  }
}, 15000);
wss.on("close", () => clearInterval(sweep));

server.listen(PORT, HOST, () => {
  console.log(`[screen-mirror relay] http://${HOST}:${PORT}  (signaling on /ws, ice on /ice)`);
});
