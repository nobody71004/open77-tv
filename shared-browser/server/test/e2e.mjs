// End-to-end check of the shared browser, run ON the VPS in a Playwright
// container with host networking: the client page through nginx, the login
// from the TV link, and a WebRTC picture arriving over the public IP's 59100.
// The link (with the viewer password) is read from /secret and never printed.
import fs from "node:fs";
import { chromium } from "playwright";
const url = fs.readFileSync("/secret", "utf8").trim();
const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
const notes = [];
page.on("websocket", (ws) => { notes.push("websocket opened"); ws.on("close", () => notes.push("websocket closed")); });
page.on("console", (m) => { const t = m.text(); if (/error|fail|timeout|disconnect|connected|ice|webrtc/i.test(t)) notes.push("console: " + t.replace(/password=[^&\s]*/g, "password=REDACTED").replace(/%c/g, "").slice(0, 150)); });
await page.goto(url, { waitUntil: "load" });
let sample = null;
for (let i = 0; i < 40; i++) {
  await page.waitForTimeout(1000);
  sample = await page.evaluate(() => {
    const v = document.querySelector("video");
    if (!v) return { video: false };
    return { video: true, readyState: v.readyState, t: v.currentTime, w: v.videoWidth, h: v.videoHeight, paused: v.paused, muted: v.muted };
  });
  if (sample.video && sample.readyState >= 2 && sample.w > 0) break;
}
const first = sample;
await page.waitForTimeout(3000);
const later = await page.evaluate(() => { const v = document.querySelector("video"); return v ? { t: v.currentTime, readyState: v.readyState } : null; });
await page.screenshot({ path: "/out/e2e.png" });
console.log(JSON.stringify({ first, later, advancing: !!(later && first && later.t > first.t), notes: notes.slice(0, 20) }, null, 1));
await browser.close();
