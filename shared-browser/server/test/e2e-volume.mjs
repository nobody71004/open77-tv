// The television's volume and mute on the shared browser's sound, end to end:
// the client page framed the way a television frames it, the level posted the way
// open77_media's tv.js posts it, and what open77-volume.js did read back -- the
// stream's element, and the loudness of the sound it puts out (its own meter,
// after the gain: in game the level has to be in the samples). The loudness is
// whatever the shared browser is playing at the moment; silence is reported as
// such.
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
const meter = async () => {
  let sum = 0;
  for (let i = 0; i < 10; i++) { sum += (await frame.evaluate(() => window.__open77Volume().level)); await page.waitForTimeout(80); }
  return Math.round((sum / 10) * 10000) / 10000;
};
const before = await read();
await post(1, false);
await page.waitForTimeout(1500);
const at100 = { elements: await read(), state: await frame.evaluate(() => window.__open77Volume()), loudness: await meter() };
await post(0.35, false);
const at35 = { loudness: await meter() };
await post(0.35, true);
const muted = { loudness: await meter() };
await post(0.75, false);
const back = { elements: await read(), state: await frame.evaluate(() => window.__open77Volume()), loudness: await meter() };
const msgs = await page.evaluate(() => __msgs);
console.log(JSON.stringify({ connected: msgs.some((m) => /^connected/.test(m)),
  volumeLines: msgs.filter((m) => /^volume/.test(m)), before, at100, at35, muted, back }, null, 1));
await browser.close();
process.exit(0);
