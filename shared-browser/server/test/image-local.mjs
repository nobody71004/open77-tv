// image-local.sh's fourth part: a real Chromium as the player, on the real client page of a throwaway room, typing
// through WebRTC into the room's own Chromium.  The room's page is a stand-in served from this machine on the Docker
// bridge (where the room service serves the real one), opened the way the room service opens pages: browser-level
// DevTools through the image's forwarder (the policy's DeveloperToolsAvailability=2 refuses attaching to a page).
//
//   O77_CLIENT=<client link> node image-local.mjs <container ip> <bridge gateway ip>
//
// Prints no link.  Exits with the number of failed checks.
import { createRequire } from "module";
import http from "http";
const { chromium } = createRequire(import.meta.url)("playwright");
const [cip, gw] = process.argv.slice(2);
const client = process.env.O77_CLIENT;
let fail = 0;
const check = (what, cond, detail) => {
  console.log(`  ${cond ? "ok  " : "FAIL"}  ${what}${detail === undefined ? "" : ": " + JSON.stringify(detail).slice(0, 300)}`);
  if (!cond) fail++;
};

// the room's page: records every key it gets and tells this machine; a reload would be a second load
let loads = 0, report = { k: [], value: "" };
const KEYPAGE = `<!doctype html><title>keys</title><style>body{background:#123;color:#fff;font:40px sans-serif}</style>
<input id=i autofocus style="font-size:40px"><script>const k=[];const i=document.getElementById('i');
addEventListener('keydown',e=>k.push((e.ctrlKey?'C-':'')+(e.altKey?'A-':'')+e.key),true);
setInterval(()=>{i.focus();fetch('/report',{method:'POST',body:JSON.stringify({k,value:i.value})})},300);</script>`;
const srv = http.createServer((req, res) => {
  if (req.url.startsWith("/keys.html")) { loads++; res.writeHead(200, { "content-type": "text/html" }); return res.end(KEYPAGE); }
  if (req.url === "/report") {
    let b = "";
    req.on("data", (c) => (b += c));
    req.on("end", () => { try { report = JSON.parse(b); } catch { /* a torn report: the next one comes */ } res.end("ok"); });
    return;
  }
  res.writeHead(404); res.end();
});
await new Promise((r) => srv.listen(0, gw, r));

const ver = await (await fetch(`http://${cip}:9222/json/version`)).json();
const ws = new WebSocket(ver.webSocketDebuggerUrl);
let id = 0; const wait = {};
ws.onmessage = (m) => { const d = JSON.parse(m.data); if (wait[d.id]) { wait[d.id](d); delete wait[d.id]; } };
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
const send = (method, params = {}) => new Promise((r) => { const i = ++id; wait[i] = r; ws.send(JSON.stringify({ id: i, method, params })); });
const pages = async () => (await send("Target.getTargets")).result.targetInfos.filter((t) => t.type === "page");
const before = await pages();
const made = await send("Target.createTarget", { url: `http://${gw}:${srv.address().port}/keys.html` });
await send("Target.activateTarget", { targetId: made.result.targetId });
for (const t of before) await send("Target.closeTarget", { targetId: t.targetId });
await new Promise((r) => setTimeout(r, 2500));
check("the room shows the key page", loads === 1, { loads });
const pages0 = (await pages()).length;

const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
try {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  await page.goto(client);
  const pic = await page.waitForFunction(() => {
    const v = document.querySelector("video");
    return v && v.videoWidth > 0 && v.currentTime > 0.5 ? `${v.videoWidth}x${v.videoHeight}` : false;
  }, null, { timeout: 60000 }).then((h) => h.jsonValue(), () => null);
  check("the picture, over WebRTC", pic === "1280x720", pic);
  const scripts = await page.evaluate(() => [...document.scripts].map((s) => s.getAttribute("src")).filter(Boolean).slice(0, 5));
  check("the guard first in the page, then the image's four", scripts.join(" ") ===
    "open77-input.js?v=3 open77-ice.js open77-paste.js open77-volume.js open77-fit.js?v=3", scripts);
  check("the guard running", (await page.evaluate(() => typeof window.__open77Input === "function" && window.__open77Input().version)) === "open77-input 3");
  const vb = await (await page.$("video")).boundingBox();
  await page.mouse.click(vb.x + vb.width / 2, vb.y + vb.height / 2);
  await page.waitForTimeout(1500);
  await page.keyboard.type("hello", { delay: 80 });
  await page.waitForTimeout(800);
  const SHORTCUTS = ["F5", "Control+t", "Control+w", "Control+n", "Alt+F4", "F11", "Control+Tab", "Control+1"];
  for (const k of SHORTCUTS) { await page.keyboard.press(k); await page.waitForTimeout(300); }
  await page.keyboard.type(" ok", { delay: 80 });
  await page.waitForTimeout(2000);
  check("typed into the room's page: \"hello ok\"", report.value === "hello ok", report.value);
  const reached = report.k.filter((k) => !/^(C-|A-|C-A-)?(Control|Alt|Shift)$/.test(k) && k.length > 1 && k !== " ");
  check("no shortcut reached the room's browser (only the modifier keys themselves)", reached.length === 0, report.k);
  check("the room's page never reloaded (F5), no tab opened or closed", loads === 1 && (await pages()).length === pages0,
    { loads, pages: (await pages()).length, was: pages0 });
  const g = await page.evaluate(() => (typeof window.__open77Input === "function" ? window.__open77Input() : {}));
  const kept = (g.keyboard && g.keyboard.keptByName) || {};
  check("the guard says what it kept", ["F5", "Ctrl+T", "Ctrl+W", "Ctrl+N", "F4", "F11", "Ctrl+Tab", "Ctrl+1"].every((k) => kept[k] >= 1), kept);
  const s = g.stream && g.stream.last;
  check("the guard's stream numbers (frame rate, round trip, path)", !!(s && s.fps > 0 && s.path), s);
} finally {
  ws.close(); srv.close();
  await browser.close();
}
process.exit(fail);
