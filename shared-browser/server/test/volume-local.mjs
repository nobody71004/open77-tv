// open77-volume.js against a real WebRTC stream, in a real Chromium, framed the
// way a television frames the shared browser's client page.
//
// The framed page makes the stream the way neko's client receives its own: a
// tone sent over a peer connection to a second one in the same page, and the
// remote stream played by a video element. The framing page posts the levels
// tv.js posts. What is measured is the sound the script puts out (its own meter,
// after the gain and the limiter) -- in game the level has to be in the samples,
// because the game's browser takes them before an element's volume is applied.
//
//   node server/test/volume-local.mjs        (CHROMIUM=<path> to pick a browser)
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

const here = path.dirname(fileURLToPath(import.meta.url));
const script = fs.readFileSync(path.join(here, "..", "build", "open77-volume.js"));

const child = `<!doctype html><html><head><script src="/open77-volume.js"></script></head><body>
<video id="stream" playsinline autoplay></video>
<script>
window.__ready = false;
async function connect() {
  const tone = new AudioContext();
  const osc = tone.createOscillator(); osc.frequency.value = 440;
  const amp = tone.createGain(); amp.gain.value = 0.25;
  const out = tone.createMediaStreamDestination();
  osc.connect(amp).connect(out); osc.start();
  const a = new RTCPeerConnection(), b = new RTCPeerConnection();
  a.onicecandidate = (e) => e.candidate && b.addIceCandidate(e.candidate);
  b.onicecandidate = (e) => e.candidate && a.addIceCandidate(e.candidate);
  const arrived = new Promise((resolve) => { b.ontrack = (e) => resolve(e.track); });
  a.addTrack(out.stream.getAudioTracks()[0], out.stream);
  await a.setLocalDescription(await a.createOffer());
  await b.setRemoteDescription(a.localDescription);
  await b.setLocalDescription(await b.createAnswer());
  await a.setRemoteDescription(b.localDescription);
  const track = await arrived;
  return { tone, a, b, track };
}
window.__connect = async function () {
  const link = await connect();
  const video = document.getElementById("stream");
  video.srcObject = new MediaStream([link.track]);
  try { await video.play(); } catch (e) { window.__playError = String(e); }
  window.__links = (window.__links || []).concat([link]);
  window.__ready = true;
};
__connect();
</script></body></html>`;

const parent = `<!doctype html><body><iframe id="tv" src="/child" allow="autoplay" style="width:640px;height:360px"></iframe>
<script>window.__msgs=[];addEventListener("message",function(e){var d=e.data;if(d&&d.open77SharedBrowser===1)__msgs.push(d.kind+" "+d.text)});
window.__post=function(v,m){document.getElementById("tv").contentWindow.postMessage({open77SharedBrowserVolume:1,volume:v,muted:m},location.origin)};</script></body>`;

const server = http.createServer((req, res) => {
  if (req.url === "/open77-volume.js") { res.writeHead(200, { "content-type": "text/javascript" }); return res.end(script); }
  res.writeHead(200, { "content-type": "text/html" });
  res.end(req.url === "/child" ? child : parent);
}).listen(0, "127.0.0.1");
await new Promise((r) => server.once("listening", r));
const origin = "http://127.0.0.1:" + server.address().port;

let failed = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) failed++; };
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

async function open(args) {
  const browser = await chromium.launch(Object.assign({ args }, process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {}));
  const page = await browser.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  await page.goto(origin + "/");
  let frame = null;
  for (let i = 0; i < 50 && !frame; i++) { await wait(100); frame = page.frames().find((f) => f.url().endsWith("/child")); }
  for (let i = 0; i < 50; i++) { if (await frame.evaluate(() => window.__ready === true)) break; await wait(100); }
  return { browser, page, frame, errors };
}
const state = (frame) => frame.evaluate(() => window.__open77Volume());
// The meter, averaged over a few reads (a read is 2048 samples, about 43 ms).
async function loudness(frame) {
  let sum = 0;
  for (let i = 0; i < 8; i++) { sum += (await state(frame)).level; await wait(60); }
  return sum / 8;
}

// 1. Sound allowed to start by itself, as in the game.
{
  const { browser, page, frame, errors } = await open(["--autoplay-policy=no-user-gesture-required"]);
  check((await state(frame)).wanted === null, "nothing is done before the television posts a level");
  await page.evaluate(() => __post(1, false));
  await wait(1500);
  let s = await state(frame);
  check(s.hooked && s.audio === "running" && s.original === false, "at 100 the stream's sound goes through Web Audio and the element's own is off: " + JSON.stringify(s));
  check(Math.abs(s.gain - 2) < 0.01, "100 is twice the stream's level: gain " + s.gain);
  const loud = await loudness(frame);
  check(loud > 0.2, "and it is heard: output level " + loud.toFixed(3));
  await page.evaluate(() => __post(0.5, false));
  await wait(800);
  const half = await loudness(frame);
  check(Math.abs(half / loud - Math.pow(0.5, 1.5)) < 0.05, "50 is the curve's share of 100: " + (half / loud).toFixed(3) + " (" + Math.pow(0.5, 1.5).toFixed(3) + ")");
  await page.evaluate(() => __post(0.5, true));
  await wait(800);
  const muted = await loudness(frame);
  check(muted < 0.001, "muted is silence: " + muted.toFixed(5));
  // Something turns the element's own sound back on: it goes off again.
  await frame.evaluate(() => { document.getElementById("stream").srcObject.getAudioTracks()[0].enabled = true; });
  await wait(2600);
  s = await state(frame);
  check(s.original === false, "a track turned back on is turned off again: " + JSON.stringify(s));
  // A new stream (the client reconnecting): taken through the gain, the old track given back.
  const oldTrack = await frame.evaluateHandle(() => document.getElementById("stream").srcObject.getAudioTracks()[0]);
  await frame.evaluate(() => window.__connect());
  await page.evaluate(() => __post(0.75, false));
  await wait(1500);
  s = await state(frame);
  const swapped = await frame.evaluate((old) => {
    const now = document.getElementById("stream").srcObject.getAudioTracks()[0];
    return { newOff: now.enabled === false, different: now !== old, oldOn: old.enabled === true };
  }, oldTrack);
  check(s.hooked && swapped.newOff && swapped.different && swapped.oldOn, "a new stream is taken over, the old track given back: " + JSON.stringify(swapped));
  check((await loudness(frame)) > 0.1, "and heard at 75");
  // Only the framing television is listened to.
  await frame.evaluate(() => window.postMessage({ open77SharedBrowserVolume: 1, volume: 0, muted: true }, "*"));
  await wait(400);
  s = await state(frame);
  check(s.wanted.volume === 0.75 && !s.wanted.muted, "a level the page posts to itself is ignored: " + JSON.stringify(s.wanted));
  const msgs = await page.evaluate(() => __msgs);
  check(msgs.some((m) => /^volume 100 on the stream's sound \(gain 2\.00, Web Audio running\)/.test(m)), "the television is told: " + JSON.stringify(msgs));
  check(errors.length === 0, "no page errors: " + JSON.stringify(errors));
  await browser.close();
}

// 2. Sound not allowed to start without a click (the browser's autoplay policy,
// played here by the page: an AudioContext made now stays suspended, and resume()
// is refused, until the page is clicked). The element keeps its sound, with the
// level on it, until then.
{
  const { browser, page, frame, errors } = await open(["--autoplay-policy=no-user-gesture-required"]);
  await frame.evaluate(() => {
    const Real = window.AudioContext;
    let allowed = false;
    window.addEventListener("pointerdown", () => { allowed = true; }, true);
    window.AudioContext = function (options) {
      const context = new Real(options);
      context.suspend();
      const resume = context.resume.bind(context);
      context.resume = () => (allowed ? resume() : Promise.reject(new Error("autoplay policy: no click yet")));
      return context;
    };
    window.AudioContext.prototype = Real.prototype;
  });
  await page.evaluate(() => __post(0.6, false));
  await wait(1200);
  let s = await state(frame);
  const element = await frame.evaluate(() => { const v = document.getElementById("stream"); return { volume: v.volume, track: v.srcObject ? v.srcObject.getAudioTracks()[0].enabled : null }; });
  check(!s.hooked && s.audio === "suspended", "without a click Web Audio waits: " + JSON.stringify(s));
  check(Math.abs(element.volume - 0.6) < 1e-6 && element.track === true, "and the element keeps its sound, at the level: " + JSON.stringify(element));
  await frame.click("body", { position: { x: 20, y: 20 } });
  await wait(1500);
  s = await state(frame);
  check(s.hooked && s.audio === "running" && s.original === false, "a click starts it, and the sound goes through the gain: " + JSON.stringify(s));
  check((await loudness(frame)) > 0.1, "and it is heard");
  check(errors.length === 0, "no page errors: " + JSON.stringify(errors));
  await browser.close();
}

server.close();
console.log(failed === 0 ? "ALL PASSED" : failed + " FAILED");
process.exit(failed === 0 ? 0 : 1);
