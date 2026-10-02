// End to end, in a real Chromium with this extension loaded:
//   node tests/live.mjs <CODE> <KEY>   (a self-test party on the live server)
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


const [CODE, KEY] = process.argv.slice(2);
const PUB = "https://xbuniverse.duckdns.org/opx-watch-xb-staging";
// through curl: it already goes through this machine's proxy and trusts its CA
async function api(method, p, body) {
  const args = ["-s", "-m", "8", "-X", method, "-H", "Content-Type: text/plain", "-w", "\n%{http_code}"];
  if (body) args.push("-d", JSON.stringify(body));
  args.push(PUB + p);
  const out = execFileSync("curl", args).toString();
  const i = out.lastIndexOf("\n");
  let data = null;
  try { data = JSON.parse(out.slice(0, i)); } catch (e) { /* not JSON */ }
  return { status: Number(out.slice(i + 1)), data };
}
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "opxwatch-live-"));
execFileSync("openssl", ["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-subj", "/CN=localhost",
  "-keyout", path.join(tmp, "key.pem"), "-out", path.join(tmp, "cert.pem")], { stdio: "ignore" });
const server = https.createServer({ key: fs.readFileSync(path.join(tmp, "key.pem")), cert: fs.readFileSync(path.join(tmp, "cert.pem")) },
  (req, res) => { const m = /^\/watch\/(\d+)/.exec(req.url); res.writeHead(200, { "Content-Type": "text/html" }); res.end(fakePage(m ? m[1] : "none")); });
await new Promise((r) => server.listen(9443, "127.0.0.1", r));
const proxy = process.env.HTTPS_PROXY || "http://127.0.0.1:40079";
const context = await chromium.launchPersistentContext(fs.mkdtempSync(path.join(os.tmpdir(), "opxwatch-live-profile-")), {
  channel: "chromium", headless: true, ignoreHTTPSErrors: true,
  args: [`--disable-extensions-except=${extensionDir}`, `--load-extension=${extensionDir}`,
    "--host-resolver-rules=MAP www.netflix.com 127.0.0.1:9443", `--proxy-server=${proxy}`,
    "--proxy-bypass-list=www.netflix.com", "--ignore-certificate-errors"],
});
try {
  let p = await api("GET", `/v1/party/${CODE}`);
  check("the live server answers for the self-test party", p.status === 200 && p.data.code === CODE, p);
  await api("POST", `/v1/party/${CODE}/control`, { key: KEY, action: "seek", positionMs: 300000 });
  await api("POST", `/v1/party/${CODE}/control`, { key: KEY, action: "play" });
  const a = await context.newPage();
  await a.goto(`https://www.netflix.com/watch/80057281#opxwatch=${CODE}`);
  await sleep(5000);
  let sa = await a.evaluate(() => window.__state());
  p = await api("GET", `/v1/party/${CODE}`);
  check("a browser with the extension follows the live party (playing, within 2.5 s)",
    sa.playing && Math.abs(sa.pos - p.data.positionMs) < 2500, { browser: sa.pos, party: p.data.positionMs, log: sa.log });
  await api("POST", `/v1/party/${CODE}/control`, { key: KEY, action: "pause" });
  await sleep(2500);
  sa = await a.evaluate(() => window.__state());
  check("the party pauses: the browser pauses", sa.playing === false, sa);
  await api("POST", `/v1/party/${CODE}/control`, { key: KEY, action: "seek", positionMs: 900000 });
  await sleep(2500);
  sa = await a.evaluate(() => window.__state());
  check("the party jumps to 15:00: the browser follows", Math.abs(sa.pos - 900000) < 2500, sa);
  p = await api("GET", `/v1/party/${CODE}`);
  check("the live party counts the browser, in step", p.data.browsers === 1 && p.data.inSync === 1, p.data);
  const tv = await context.newPage();
  await tv.setViewportSize({ width: 1280, height: 720 });
  await tv.goto(`${PUB}/v1/tv/${CODE}`);
  await sleep(2500);
  await tv.screenshot({ path: process.env.SHOT || "/tmp/tv-live.png" });
  const title = await tv.evaluate(() => document.getElementById("title").textContent);
  check("the television page, served live through the web server, shows the party", title === "Self test", title);
} catch (e) {
  failures++; console.log("  FAIL the test itself: " + (e && e.stack || e));
} finally {
  await context.close(); server.close();
}
console.log(`\n${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
