// Store images for the Open77 Watch Party extension, made from the real thing:
// the extension loaded in Chromium, the watch party server's own Lua rules behind
// it (as in tests/e2e.mjs), and a neutral stand-in player page -- not Netflix's
// interface, which is theirs -- exposing the same player API.
//
//   node shots.mjs <extension dir> <opx_watchparty dir> <out dir>
import { spawn, execFileSync } from "node:child_process";
import https from "node:https";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const { chromium } = require(path.join(execFileSync("npm", ["root", "-g"]).toString().trim(), "playwright"));
const [extensionDir, resourceDir, outDir] = process.argv.slice(2).map((p) => path.resolve(p));
fs.mkdirSync(outDir, { recursive: true });
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
const lua = (msg) => new Promise((resolve) => {
  waiting.push(resolve);
  bridge.stdin.write(JSON.stringify(Object.assign({ now: Date.now() }, msg)) + "\n");
});

// ---- the stand-in player: a dark frame, a title, a progress bar. No logos.
const TITLE = "Night City Nights";
const standIn = (id) => `<!DOCTYPE html><html><head><meta charset="utf-8"><title>${TITLE}</title>
<style>
  html, body { margin: 0; height: 100%; background: #000; overflow: hidden; font-family: "Segoe UI", Arial, sans-serif; }
  .frame { position: absolute; inset: 0;
    background:
      radial-gradient(ellipse 60% 45% at 70% 38%, rgba(25,223,235,.28), transparent 70%),
      radial-gradient(ellipse 50% 40% at 22% 30%, rgba(229,9,20,.30), transparent 70%),
      linear-gradient(180deg, #0b0f1c 0%, #141a33 46%, #07080d 100%); }
  .glow { position: absolute; left: 8%; right: 8%; top: 26%; height: 34%; border-radius: 50%;
    background: radial-gradient(ellipse at center, rgba(233,238,245,.10), transparent 70%); filter: blur(8px); }
  .city { position: absolute; left: 0; right: 0; bottom: 120px; height: 330px;
    background:
      linear-gradient(90deg, transparent 0 4%, #05060a 4% 9%, transparent 9% 11%, #05060a 11% 15%, transparent 15% 16%,
        #05060a 16% 24%, transparent 24% 27%, #05060a 27% 31%, transparent 31% 33%, #05060a 33% 41%, transparent 41% 44%,
        #05060a 44% 47%, transparent 47% 49%, #05060a 49% 58%, transparent 58% 61%, #05060a 61% 66%, transparent 66% 68%,
        #05060a 68% 77%, transparent 77% 80%, #05060a 80% 86%, transparent 86% 88%, #05060a 88% 96%, transparent 96%);
    -webkit-mask: linear-gradient(0deg, #000 0 35%, transparent 35%), repeating-linear-gradient(90deg, #000 0 3%, transparent 3% 4%);
    -webkit-mask-composite: source-over; opacity: .95; }
  .ground { position: absolute; left: 0; right: 0; bottom: 0; height: 128px; background: #05060a; }
  .bar { position: absolute; left: 40px; right: 40px; bottom: 44px; height: 4px; background: rgba(255,255,255,.18); }
  .bar i { position: absolute; left: 0; top: 0; bottom: 0; width: 39%; background: #e9eef5; }
  .bar b { position: absolute; left: 39%; top: -5px; width: 14px; height: 14px; margin-left: -7px; border-radius: 50%; background: #e9eef5; }
  .name { position: absolute; left: 40px; bottom: 68px; color: #e9eef5; font-size: 22px; font-weight: 600; letter-spacing: .3px; }
  .name span { color: #8d99ab; font-weight: 400; font-size: 17px; margin-left: 10px; }
</style></head><body>
<div class="frame"></div><div class="glow"></div>
<div class="name" data-uia="video-title">${TITLE}<span>Episode 1</span></div>
<div class="bar"><i></i><b></b></div>
<video id="v" style="display:none"></video>
<script>
(function () {
  var v = document.getElementById("v");
  var st = { pos: 0, playing: false, at: performance.now(), dur: 2820000 };
  function now() { var ms = st.playing ? st.pos + (performance.now() - st.at) : st.pos; return Math.min(ms, st.dur); }
  function fire(type) { v.dispatchEvent(new Event(type)); }
  var player = {
    getCurrentTime: function () { return now(); },
    isPaused: function () { return !st.playing; },
    getDuration: function () { return st.dur; },
    play: function () { st.pos = now(); st.at = performance.now(); st.playing = true; fire("play"); },
    pause: function () { st.pos = now(); st.playing = false; fire("pause"); },
    seek: function (ms) { st.pos = ms; st.at = performance.now(); setTimeout(function () { fire("seeked"); }, 30); }
  };
  window.netflix = { appContext: { state: { playerApp: { getAPI: function () {
    return { videoPlayer: { getAllPlayerSessionIds: function () { return ["watch-1"]; },
      getVideoPlayerBySessionId: function () { return player; } } }; } } } } };
  Object.defineProperty(v, "currentTime", { get: function () { return now() / 1000; }, set: function (s) { st.pos = s * 1000; st.at = performance.now(); } });
  Object.defineProperty(v, "paused", { get: function () { return !st.playing; } });
  Object.defineProperty(v, "duration", { get: function () { return st.dur / 1000; } });
  setTimeout(function () { player.play(); }, 300);
  window.__state = function () { return { pos: now(), playing: st.playing }; };
})();
</script></body></html>`;

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "opxwatch-shots-"));
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
      res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
      res.end(standIn(m ? m[1] : "none"));
      return;
    }
    if (host === "xbuniverse.duckdns.org" && req.url.startsWith(BASE_PATH + "/")) {
      const out = await lua({ op: "http", req: { method: req.method, path: req.url.slice(BASE_PATH.length), body,
        remoteAddress: "203.0.113.5", headers: { "X-Real-IP": "203.0.113.5" } } });
      res.writeHead(out.status, out.headers || {});
      res.end(out.body || "");
      return;
    }
    res.writeHead(404);
    res.end("no");
  });
await new Promise((r) => server.listen(9443, "127.0.0.1", r));

const context = await chromium.launchPersistentContext(fs.mkdtempSync(path.join(os.tmpdir(), "opxwatch-profile-")), {
  channel: "chromium",
  headless: true,
  ignoreHTTPSErrors: true,
  viewport: { width: 1280, height: 800 },
  deviceScaleFactor: 1,
  args: [
    `--disable-extensions-except=${extensionDir}`,
    `--load-extension=${extensionDir}`,
    "--host-resolver-rules=MAP www.netflix.com 127.0.0.1:9443, MAP xbuniverse.duckdns.org 127.0.0.1:9443",
    "--ignore-certificate-errors",
    "--no-proxy-server",
  ],
});

try {
  let [worker] = context.serviceWorkers();
  if (!worker) worker = await context.waitForEvent("serviceworker", { timeout: 10000 });
  const extensionId = new URL(worker.url()).host;

  const host = await lua({ op: "start", player: 7, netflixId: "80117401", title: TITLE, name: "Matt" });
  await lua({ op: "join", player: 8, code: host.code, name: "Ana" });
  await lua({ op: "join", player: 9, code: host.code, name: "Kenji" });
  await lua({ op: "control", code: host.code, action: "seek", positionMs: 1099000 });
  await lua({ op: "control", code: host.code, action: "play" });

  const page = await context.newPage();
  await page.setViewportSize({ width: 720, height: 560 });
  await page.goto(`https://www.netflix.com/watch/80117401#opxwatch=${host.code}.${host.key}`);
  await sleep(5000);
  // The badge is in a closed shadow root: click where it is drawn to open its card.
  await page.mouse.click(720 - 12 - 60, 12 + 12);
  await sleep(600);
  await page.screenshot({ path: path.join(outDir, "raw-badge.png") });

  const popup = await context.newPage();
  await popup.setViewportSize({ width: 352, height: 380 });
  await popup.goto(`chrome-extension://${extensionId}/popup.html`);
  await sleep(800);
  const box = await popup.evaluate(() => {
    const r = document.body.getBoundingClientRect();
    return { width: Math.ceil(r.width), height: Math.ceil(document.documentElement.scrollHeight) };
  });
  await popup.screenshot({ path: path.join(outDir, "raw-popup.png"), clip: { x: 0, y: 0, width: box.width, height: box.height } });

  // The in-game panel (the resource's own web/panel.html, as the game's browser
  // shows it), in the state of the party above.
  const panel = await context.newPage();
  await panel.setViewportSize({ width: 1920, height: 1080 });
  await panel.addInitScript(() => {
    window.__handlers = {};
    window.Open77 = { on: (n, f) => { window.__handlers[n] = f; }, emit: () => {}, ready: () => {} };
  });
  await panel.goto("file://" + path.join(resourceDir, "web", "panel.html"));
  await sleep(500);
  await panel.evaluate((code) => window.__handlers["watch:state"]({ open: true, version: "1.0.0", isHost: true, hasKey: true,
    party: { code, title: "Night City Nights: Episode 1", netflixId: "80117401", playing: true, positionMs: 1099000,
      durationMs: 2820000, hostName: "Matt", lastBy: "Ana", members: 3, browsers: 3, inSync: 3, tvId: 4 },
    result: { ok: true, text: "browser link copied: paste it into Edge or Chrome (with the Open77 Watch Party extension).", at: 1 } }),
    host.code);
  await sleep(600);
  const panelBox = await panel.evaluate(() => {
    const r = document.getElementById("panel").getBoundingClientRect();
    return { x: Math.floor(r.left), y: Math.floor(r.top), width: Math.ceil(r.width), height: Math.ceil(r.height) };
  });
  await panel.screenshot({ path: path.join(outDir, "raw-panel.png"), clip: panelBox });

  // The page a television in game shows for the party, served by the resource.
  const tv = await context.newPage();
  await tv.setViewportSize({ width: 960, height: 540 });
  await tv.goto(`https://xbuniverse.duckdns.org${BASE_PATH}/v1/tv/${host.code}`);
  await sleep(2500);
  await tv.screenshot({ path: path.join(outDir, "raw-tv.png") });

  // ---- the store images, composed from the raw ones
  const data = (file) => "data:image/png;base64," + fs.readFileSync(file).toString("base64");
  const here = path.dirname(new URL(import.meta.url).pathname);
  const iconFile = [path.join(here, "icon.svg"), path.join(here, "art", "icon.svg")].find((f) => fs.existsSync(f));
  const icon = "data:image/svg+xml;base64," + fs.readFileSync(iconFile).toString("base64");
  const stage = await context.newPage();
  const css = `
    html, body { margin: 0; background: #0a0d13; color: #e9eef5; font-family: "Segoe UI", "DejaVu Sans", Arial, sans-serif; overflow: hidden; }
    .cap { position: absolute; left: 56px; top: 48px; width: 420px; }
    .cap h1 { font-size: 34px; line-height: 1.15; margin: 0 0 16px; font-weight: 700; letter-spacing: .2px; }
    .cap p { font-size: 18px; line-height: 1.5; color: #a9b4c4; margin: 0 0 12px; }
    .cap .mark { display: flex; align-items: center; gap: 10px; margin-bottom: 28px; color: #8d99ab; font-size: 14px;
      letter-spacing: 3px; text-transform: uppercase; }
    .cap .mark img { width: 34px; height: 34px; }
    .shot { position: absolute; box-shadow: 0 18px 60px rgba(0,0,0,.65); border: 1px solid #1f2735; }
    .accent { position: absolute; left: 56px; bottom: 48px; width: 64px; height: 4px; background: #19dfeb; }`;
  const render = async (file, width, height, html) => {
    await stage.setViewportSize({ width, height });
    await stage.setContent(`<!DOCTYPE html><html><head><meta charset="utf-8"><style>${css}</style></head><body style="width:${width}px;height:${height}px">${html}</body></html>`);
    await sleep(300);
    await stage.screenshot({ path: path.join(outDir, file) });
  };
  const mark = `<div class="mark"><img src="${icon}">Open77 Watch Party</div>`;

  await render("screenshot-1-in-step-1280x800.png", 1280, 800, `
    <img class="shot" src="${data(path.join(outDir, "raw-badge.png"))}" style="left:520px;top:120px;width:720px;height:560px">
    <div class="cap">${mark}<h1>Your film, in step with the party</h1>
      <p>Play, pause and seek follow the watch party in Open77, and your own controls move the party.</p>
      <p>A small badge says you are in step. Click it for the party, or to leave.</p></div>
    <div class="accent"></div>`);

  await render("screenshot-2-popup-1280x800.png", 1280, 800, `
    <div class="cap">${mark}<h1>Join with one link</h1>
      <p>In game, press <b>Copy browser link</b> and paste it into the address bar: the party code and your own key fill in by themselves, and leave the address bar at once.</p>
      <p>Or type a party code here to follow along.</p></div>
    <img class="shot" src="${data(path.join(outDir, "raw-popup.png"))}" style="left:640px;top:${Math.round((800 - box.height * 1.45) / 2)}px;width:${Math.round(box.width * 1.45)}px">
    <div class="accent"></div>`);

  {
    const panelPng = path.join(outDir, "raw-panel.png");
    await render("screenshot-3-in-game-1280x800.png", 1280, 800, `
      <div class="cap">${mark}<h1>Start it from the TV in game</h1>
        <p>At a TV in Open77, <b>/watch</b> starts a party with a Netflix link or joins the one playing there.</p>
        <p>Everyone watches on their own Netflix account. Only the title and the time are shared.</p></div>
      <img class="shot" src="${data(panelPng)}" style="left:600px;top:50%;transform:translateY(-50%);max-height:740px">
      <div class="accent"></div>`);
  }

  await render("screenshot-4-tv-1280x800.png", 1280, 800, `
    <div class="cap">${mark}<h1>The TV shows the party</h1>
      <p>The television the party is on shows what is playing, where the film is, and how many browsers are in step.</p>
      <p>Nothing of the film itself is captured or streamed into the game.</p></div>
    <img class="shot" src="${data(path.join(outDir, "raw-tv.png"))}" style="left:520px;top:200px;width:720px;height:405px">
    <div class="accent"></div>`);

  await render("promo-small-440x280.png", 440, 280, `
    <div style="position:absolute;inset:0;background:radial-gradient(ellipse 70% 80% at 85% 20%,rgba(25,223,235,.22),transparent 70%),radial-gradient(ellipse 60% 70% at 10% 90%,rgba(229,9,20,.25),transparent 70%)"></div>
    <img src="${icon}" style="position:absolute;left:32px;top:58px;width:84px;height:84px">
    <div style="position:absolute;left:136px;top:62px;font-size:30px;font-weight:700;line-height:1.1">Open77<br>Watch Party</div>
    <div style="position:absolute;left:34px;top:172px;width:390px;font-size:16px;line-height:1.45;color:#a9b4c4">Your own Netflix, in step with<br>the watch party in game.</div>
    <div style="position:absolute;left:34px;bottom:30px;width:48px;height:4px;background:#19dfeb"></div>`);

  await render("promo-marquee-1400x560.png", 1400, 560, `
    <div style="position:absolute;inset:0;background:radial-gradient(ellipse 50% 70% at 80% 30%,rgba(25,223,235,.20),transparent 70%),radial-gradient(ellipse 45% 70% at 8% 95%,rgba(229,9,20,.24),transparent 70%)"></div>
    <img src="${icon}" style="position:absolute;left:96px;top:150px;width:150px;height:150px">
    <div style="position:absolute;left:286px;top:150px;font-size:58px;font-weight:700;line-height:1.05">Open77<br>Watch Party</div>
    <div style="position:absolute;left:100px;top:360px;width:640px;font-size:24px;line-height:1.45;color:#a9b4c4">Play, pause and seek with the party in game, on your own Netflix account. Only the time is shared.</div>
    <img class="shot" src="${data(path.join(outDir, "raw-popup.png"))}" style="left:930px;top:${Math.round((560 - box.height * 1.15) / 2)}px;width:${Math.round(box.width * 1.15)}px">`);

  await render("store-logo-300x300.png", 300, 300, `<img src="${icon}" style="position:absolute;left:0;top:0;width:300px;height:300px">`);
  await stage.evaluate(() => { document.body.style.background = "transparent"; document.documentElement.style.background = "transparent"; });
  await stage.setViewportSize({ width: 300, height: 300 });
  await stage.setContent(`<!DOCTYPE html><html style="background:transparent"><body style="margin:0;background:transparent"><img src="${icon}" style="width:300px;height:300px;display:block"></body></html>`);
  await sleep(200);
  await stage.screenshot({ path: path.join(outDir, "store-logo-300x300.png"), omitBackground: true });
  await stage.setViewportSize({ width: 128, height: 128 });
  await stage.setContent(`<!DOCTYPE html><html style="background:transparent"><body style="margin:0;background:transparent"><img src="${icon}" style="width:128px;height:128px;display:block"></body></html>`);
  await sleep(200);
  await stage.screenshot({ path: path.join(outDir, "icon-128-from-svg.png"), omitBackground: true });
  console.log("shots ok", extensionId, host.code);
} finally {
  await context.close();
  server.close();
  bridge.stdin.end();
}
