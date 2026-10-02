// The shared browser's client page as a television frames it -- inside a page
// on 127.0.0.1, the link without its mark, open77=2 -- and what the image's
// open77-ice.js tells that television. Run ON the VPS in Playwright's image. The
// link (viewer password) is read from /secret and never printed.
// usage: node e2e-page.mjs <seconds>
import fs from "node:fs";
import http from "node:http";
import { chromium } from "playwright";
const link = fs.readFileSync("/secret", "utf8").trim().replace(/#open77-shared-browser$/, "");
const seconds = Number(process.argv[2] || 30);
const server = http.createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/html" });
  res.end('<!doctype html><body style="margin:0;background:#000"><iframe id="tv" style="border:0;width:1280px;height:720px" ' +
    'allow="autoplay; fullscreen; clipboard-read; clipboard-write" referrerpolicy="no-referrer"></iframe><script>window.__msgs=[];' +
    'addEventListener("message",function(e){var d=e.data;if(d&&d.open77SharedBrowser===1)__msgs.push(d.kind+" ["+d.mode+"] "+d.text)});</script></body>');
}).listen(0, "127.0.0.1");
await new Promise((r) => server.once("listening", r));
const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
await page.goto("http://127.0.0.1:" + server.address().port + "/");
const u = new URL(link); u.searchParams.set("open77", "2");
await page.evaluate((src) => { document.getElementById("tv").src = src; }, u.toString());
let video = null;
const until = Date.now() + seconds * 1000;
while (Date.now() < until) {
  await page.waitForTimeout(1000);
  const msgs = await page.evaluate(() => __msgs.slice());
  if (msgs.some((m) => /^(connected|gave_up)/.test(m))) { await page.waitForTimeout(2500); break; }
}
const frame = page.frames().find((f) => f !== page.mainFrame());
if (frame) video = await frame.evaluate(() => { const v = document.querySelector("video"); return v ? { readyState: v.readyState, t: Math.round(v.currentTime * 10) / 10, w: v.videoWidth, h: v.videoHeight } : null; }).catch(() => null);
const msgs = await page.evaluate(() => __msgs);
console.log(JSON.stringify({ video, ice: frame ? new URL(frame.url()).searchParams.get("ice") : null,
  messages: msgs.map((m) => m.replace(/pwd=[^&\s]*/g, "pwd=REDACTED")) }, null, 1));
await browser.close();
process.exit(0);
