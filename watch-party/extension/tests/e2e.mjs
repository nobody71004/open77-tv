// End to end, in a real Chromium with this extension loaded:
//   node tests/e2e.mjs [path/to/opx_watchparty]
//
// www.netflix.com and xbuniverse.duckdns.org are both mapped to a local HTTPS
// server. The "Netflix" page is a stand-in player that exposes the same player
// API the real one does (no video at all); the watch party server is the
// resource's own Lua rules (party.lua + http.lua) behind tests/e2e_bridge.lua.
// The test moves the party from the "server" side and checks the browser
// follows; moves the browser as a viewer and checks the party follows; and
// switches the party's title and checks the tab goes with it.
import { spawn, execFileSync } from "node:child_process";
import https from "node:https";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const { chromium } = require(path.join(execFileSync("npm", ["root", "-g"]).toString().trim(), "playwright"));
const here = path.dirname(fileURLToPath(import.meta.url));
const extensionDir = path.resolve(here, "..");
const resourceDir = path.resolve(process.argv[2] || path.join(extensionDir, "..", "opx_watchparty"));

let checks = 0, failures = 0;
function check(label, ok, detail) {
  checks++;
  if (ok) console.log("  ok   " + label);
  else { failures++; console.log("  FAIL " + label + (detail !== undefined ? "  -- " + JSON.stringify(detail) : "")); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- the party server: the resource's own Lua, behind a pipe
const bridge = spawn("lua5.4", ["tests/e2e_bridge.lua"], { cwd: resourceDir, stdio: ["pipe", "pipe", "inherit"] });
let buffered = "";
const waiting = [];
bridge.stdout.on("data", (chunk) => {
  buffered += chunk.toString();
  let i;
  while ((i = buffered.indexOf("\n")) >= 0) {
    const line = buffered.slice(0, i);
    buffered = buffered.slice(i + 1);
    const resolve = waiting.shift();
    if (resolve) resolve(JSON.parse(line));
  }
});
function lua(msg) {
  return new Promise((resolve) => {
    waiting.push(resolve);
    bridge.stdin.write(JSON.stringify(Object.assign({ now: Date.now() }, msg)) + "\n");
  });
}

// ---- the stand-in Netflix page
const fakePage = (id) => `<!DOCTYPE html><html><head><title>Netflix</title></head><body>
<div data-uia="video-title">Stand-in title ${id}</div>
<video id="v"></video>
<script>
(function () {
  var v = document.getElementById("v");
  var st = { pos: 0, playing: false, at: performance.now(), dur: 3000000 };
  function now() { var ms = st.playing ? st.pos + (performance.now() - st.at) : st.pos; return Math.min(ms, st.dur); }
  function fire(type) { v.dispatchEvent(new Event(type)); }
  var log = [];
  var player = {
    getCurrentTime: function () { return now(); },
    isPaused: function () { return !st.playing; },
    getDuration: function () { return st.dur; },
    play: function () { st.pos = now(); st.at = performance.now(); st.playing = true; log.push("play"); fire("play"); },
    pause: function () { st.pos = now(); st.playing = false; log.push("pause"); fire("pause"); },
    seek: function (ms) { st.pos = ms; st.at = performance.now(); log.push("seek " + Math.round(ms)); setTimeout(function () { fire("seeked"); }, 30); }
  };
  window.netflix = { appContext: { state: { playerApp: { getAPI: function () {
    return { videoPlayer: { getAllPlayerSessionIds: function () { return ["watch-1"]; },
      getVideoPlayerBySessionId: function () { return player; } } }; } } } } };
  Object.defineProperty(v, "currentTime", { get: function () { return now() / 1000; }, set: function (s) { st.pos = s * 1000; st.at = performance.now(); } });
  Object.defineProperty(v, "paused", { get: function () { return !st.playing; } });
  Object.defineProperty(v, "duration", { get: function () { return st.dur / 1000; } });
  // Netflix starts the film by itself a moment after the page loads.
  setTimeout(function () { player.play(); }, 500);
  // What a viewer does with the player's own buttons.
  window.__user = {
    pause: function () { player.pause(); }, play: function () { player.play(); },
    seek: function (ms) { st.pos = ms; st.at = performance.now(); log.push("user seek"); fire("seeked"); }
  };
  window.__state = function () { return { pos: now(), playing: st.playing, log: log.slice(-12), href: location.href }; };
})();
</script></body></html>`;

// ---- one local HTTPS server for both hosts
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "opxwatch-e2e-"));
execFileSync("openssl", ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost",
  "-keyout", path.join(tmp, "key.pem"), "-out", path.join(tmp, "cert.pem")], { stdio: "ignore" });
const BASE_PATH = "/opx-watch-xb-staging";
const server = https.createServer({ key: fs.readFileSync(path.join(tmp, "key.pem")), cert: fs.readFileSync(path.join(tmp, "cert.pem")) },
  async (req, res) => {
    const host = (req.headers.host || "").split(":")[0];
    let body = "";
    req.on("data", (c) => (body += c));
    await new Promise((r) => req.on("end", r));
    if (host === "www.netflix.com") {
      const m = /^\/watch\/(\d+)/.exec(req.url);
      res.writeHead(200, { "Content-Type": "text/html" });
      res.end(fakePage(m ? m[1] : "none"));
      return;
    }
    if (host === "xbuniverse.duckdns.org" && req.url.startsWith(BASE_PATH + "/")) {
      const rest = req.url.slice(BASE_PATH.length);
      const out = await lua({ op: "http", req: { method: req.method, path: rest, body, remoteAddress: "203.0.113.5",
        headers: { "X-Real-IP": "203.0.113.5" } } });
      res.writeHead(out.status, out.headers || {});
      res.end(out.body || "");
      return;
    }
    res.writeHead(404);
    res.end("no");
  });
await new Promise((r) => server.listen(9443, "127.0.0.1", r));

// Each browser profile is one viewer (one party, one key): a second viewer is a
// second profile, as it would be a second PC.
function browser() {
  return chromium.launchPersistentContext(fs.mkdtempSync(path.join(os.tmpdir(), "opxwatch-profile-")), {
    channel: "chromium",
    headless: true,
    ignoreHTTPSErrors: true,
    args: [
      `--disable-extensions-except=${extensionDir}`,
      `--load-extension=${extensionDir}`,
      "--host-resolver-rules=MAP www.netflix.com 127.0.0.1:9443, MAP xbuniverse.duckdns.org 127.0.0.1:9443",
      "--ignore-certificate-errors",
      "--no-proxy-server",
    ],
  });
}
const context = await browser();
let context2 = null;

try {
  let [worker] = context.serviceWorkers();
  if (!worker) worker = await context.waitForEvent("serviceworker", { timeout: 10000 });
  check("the extension loads (its service worker is up)", !!worker, null);

  const host = await lua({ op: "start", player: 7, netflixId: "80057281", title: "Dark", name: "Matt" });
  const guest = await lua({ op: "join", player: 8, code: host.code, name: "Ana" });
  check("a party with two members", !!host.code && !!host.key && !!guest.key, { host, guest });
  await lua({ op: "control", code: host.code, action: "seek", positionMs: 60000 });
  await lua({ op: "control", code: host.code, action: "play" });

  const a = await context.newPage();
  await a.goto(`https://www.netflix.com/watch/80057281#opxwatch=${host.code}.${host.key}`);
  await sleep(4000);
  let sa = await a.evaluate(() => window.__state());
  let party = (await lua({ op: "state", code: host.code })).state;
  check("the browser joins from the link and goes to where the party is (60 s + time since), playing",
    sa.playing && Math.abs(sa.pos - party.positionMs) < 2500, { browser: sa, party: party.positionMs });
  check("and the key is wiped from the address bar", !sa.href.includes("opxwatch"), sa.href);

  await lua({ op: "control", code: host.code, action: "seek", positionMs: 1200000 });
  await sleep(2500);
  sa = await a.evaluate(() => window.__state());
  party = (await lua({ op: "state", code: host.code })).state;
  check("the party jumps to 20:00 in game: the browser follows within two seconds",
    Math.abs(sa.pos - party.positionMs) < 2500 && sa.pos > 1190000, { browser: sa.pos, party: party.positionMs, log: sa.log });

  await lua({ op: "control", code: host.code, action: "pause" });
  await sleep(2500);
  sa = await a.evaluate(() => window.__state());
  check("the party pauses in game: the browser pauses", sa.playing === false, sa);

  await a.evaluate(() => window.__user.seek(2400000));
  await sleep(1500);
  party = (await lua({ op: "state", code: host.code })).state;
  check("the viewer seeks in their browser: the party moves there (by the viewer's key)",
    Math.abs(party.positionMs - 2400000) < 3000 && party.lastBy === "Matt", party);
  await a.evaluate(() => window.__user.play());
  await sleep(1500);
  party = (await lua({ op: "state", code: host.code })).state;
  check("the viewer presses play: the party plays", party.playing === true, party);

  context2 = await browser();
  const b = await context2.newPage();
  await b.goto(`https://www.netflix.com/watch/80057281#opxwatch=${host.code}`);
  await sleep(4000);
  const sb = await b.evaluate(() => window.__state());
  party = (await lua({ op: "state", code: host.code })).state;
  check("a second tab that only follows (no key) is in step too",
    sb.playing && Math.abs(sb.pos - party.positionMs) < 2500, { tab: sb.pos, party: party.positionMs });
  await b.evaluate(() => window.__user.pause());
  await sleep(2500);
  const sb2 = await b.evaluate(() => window.__state());
  party = (await lua({ op: "state", code: host.code })).state;
  check("and a follower's own pause does not stop the party: it is put back in step",
    party.playing === true && sb2.playing === true, { tab: sb2, party: party.playing });
  check("the party counts both browsers in step", party.browsers === 2 && party.inSync === 2, party);

  await lua({ op: "title", code: host.code, netflixId: "81231974", title: "Wednesday" });
  await lua({ op: "control", code: host.code, action: "play" });
  await sleep(3500);
  const urlA = a.url();
  check("the party switches title: a tab that was following goes to the new title",
    /\/watch\/81231974/.test(urlA), urlA);
} catch (e) {
  failures++;
  console.log("  FAIL the test itself: " + (e && e.stack || e));
} finally {
  await context.close();
  if (context2) await context2.close();
  server.close();
  bridge.stdin.end();
}
console.log(`\n${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
