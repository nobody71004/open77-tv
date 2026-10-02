// The television's volume and mute on the shared browser's sound, end to end:
// the client page framed the way a television frames it, the level posted the way
// open77_media's tv.js posts it, and the stream's own media element read back.
// Afterwards the level is put back to the televisions' default (75, not muted).
// Run ON the VPS in Playwright's image; the link (viewer password) is read from
// /secret and never printed.
import fs from "node:fs";
import http from "node:http";
import { chromium } from "playwright";
const link = fs.readFileSync("/secret", "utf8").trim().replace(/#open77-shared-browser$/, "");
const server = http.createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/html" });
  res.end('<!doctype html><body style="margin:0;background:#000"><iframe id="tv" style="border:0;width:1280px;height:720px" ' +
    'allow="autoplay; fullscreen; clipboard-read; clipboard-write" referrerpolicy="no-referrer"></iframe><script>window.__msgs=[];' +
    'addEventListener("message",function(e){var d=e.data;if(d&&d.open77SharedBrowser===1)__msgs.push(d.kind+" ["+d.mode+"] "+d.text)});' +
    'window.__post=function(v,m,o){document.getElementById("tv").contentWindow.postMessage({open77SharedBrowserVolume:1,volume:v,muted:m},o)};' +
    '</script></body>');
}).listen(0, "127.0.0.1");
await new Promise((r) => server.once("listening", r));
const origin = "http://127.0.0.1:" + server.address().port;
const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const page = await (await browser.newContext({ viewport: { width: 1280, height: 720 } })).newPage();
await page.goto(origin + "/");
const u = new URL(link);
u.searchParams.set("open77", "2");
const frameOrigin = u.origin;
await page.evaluate((src) => { document.getElementById("tv").src = src; }, u.toString());
for (let i = 0; i < 30; i++) {
  await page.waitForTimeout(1000);
  if ((await page.evaluate(() => __msgs.slice())).some((m) => /^connected/.test(m))) break;
}
await page.waitForTimeout(2000);
const frame = page.frames().find((f) => f.url().startsWith(frameOrigin));
const read = () => frame.evaluate(() => Array.from(document.querySelectorAll("video, audio")).map((v) => ({
  tag: v.tagName.toLowerCase(), volume: Math.round(v.volume * 100), muted: v.muted, paused: v.paused,
  audioTracks: v.srcObject && v.srcObject.getAudioTracks ? v.srcObject.getAudioTracks().length : -1 })));
const post = async (volume, muted) => {
  await page.evaluate(([v, m, o]) => __post(v, m, o), [volume, muted, frameOrigin]);
  await page.waitForTimeout(800);
};
const before = await read();
await post(0.35, false);
const at35 = await read();
await post(0.35, true);
const muted = await read();
// The client copies the element's level into its own saved one.
const saved = await frame.evaluate(() => { try { return localStorage.getItem("volume"); } catch (e) { return "unreadable"; } });
await post(0.75, false);
const back = await read();
const msgs = await page.evaluate(() => __msgs);
console.log(JSON.stringify({ connected: msgs.some((m) => /^connected/.test(m)),
  volumeLines: msgs.filter((m) => /^volume/.test(m)), before, at35, muted, saved, back }, null, 1));
await browser.close();
process.exit(0);
