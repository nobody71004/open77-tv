// open77-fit.js against a real WebRTC stream, in a real Chromium, framed in a box
// the way a television frames the shared browser's client page.
//
// The framed page is neko's client page as far as the picture goes, structure and
// CSS taken from the room's own image (measured 2026-10-16 with
// `docker exec open77-tvbrowser-xbs cat /var/www/css/app.*.css`): `div.player`, an
// absolutely positioned flex box with a black background; inside it
// `div.player-container`, `position: relative; width: 100%; max-width: 177.7778vh`
// -- a 16:9 box, centred, which is what leaves the bars; and inside that the
// picture, `position: absolute; width: 100%; height: 100%; object-fit: contain`.
// The stream is real WebRTC video: a canvas carrying a cyan field with magenta
// bands along its top and bottom and a white mark in the middle, sent over a peer
// connection to a second one in the page.
//
// The answer the script has to give is one answer, on every shape: THE PICTURE FILLS
// THE BOX. Five fills were measured on a television on 2026-10-16 and all five were
// rejected -- black stripes, a blur, the video again, the picture's edge column
// stretched, and the same edge colours dimmed as a wash ("the blur side bars are
// back") -- so what is held here is:
//
//   * a box that is NOT the picture's shape: the picture element IS the box, wearing
//     the cover class, with a computed `object-fit: cover` (the browser's own uniform
//     scale, so the picture is cropped and never stretched), the box's own edges are
//     the picture rather than the page's dark grey, the synthetic stream's square
//     white mark is still SQUARE and still the middle of the screen (a stretched
//     picture shows a rectangle off-centre), and NOTHING is added to the page -- no
//     layer, no canvas, no second stream, and the page's own inline style attribute
//     byte for byte what it was before a stream existed (the cover is a class).
//   * a box that IS the picture's shape: nothing at all is done, and the page carries
//     no trace of this script except its (unused) stylesheet.
//   * and the control, every time: the same box, the same stream, the same page
//     WITHOUT the script must read the client page's own black exactly where the cover
//     fills -- a check that cannot see a gap is worth nothing.
//
//   node server/test/fit-local.mjs          (CHROMIUM=<path> to pick a browser)
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

const here = path.dirname(fileURLToPath(import.meta.url));
const script = fs.readFileSync(path.join(here, "..", "build", "open77-fit.js"));
// The volume script too, for the section at the end that runs both on one page.
const volume = fs.readFileSync(path.join(here, "..", "build", "open77-volume.js"));

// The stream every shape is shown: a 16:9 canvas, cyan, with magenta bands at its
// top and bottom edges and a white mark in the middle. The mark is how a stretch is
// caught -- it is square in the stream, so it has to be square in the box -- and the
// bands are the picture's own outermost rows. The one moving thing -- a dot at y=200,
// outside the middle of the picture the mark is measured in -- keeps the stream live:
// real frames, not a still.
const STREAM = `
const canvas = document.createElement("canvas");
canvas.width = 1280; canvas.height = 720;
const paint = canvas.getContext("2d");
paint.fillStyle = "rgb(0, 192, 255)"; paint.fillRect(0, 0, 1280, 720);
paint.fillStyle = "rgb(255, 0, 200)"; paint.fillRect(0, 0, 1280, 86); paint.fillRect(0, 634, 1280, 86);
paint.fillStyle = "rgb(255, 255, 255)"; paint.fillRect(600, 320, 80, 80);
setInterval(function () {
  paint.fillStyle = "rgb(255, 0, 200)"; paint.fillRect(0, 0, 1280, 86);paint.fillStyle = "rgb(255, 255, 255)";
  paint.fillRect(10 + (Date.now() / 50) % 40, 200, 4, 4);
}, 50);
`;

// neko's own picture element: absolutely positioned, filling its 16:9 container.
const PICTURE = "position:absolute;top:0;bottom:0;width:100%;height:100%;object-fit:contain";
// A client that lays the picture out in the document's flow instead: same box, same
// shape, not positioned.
const PICTURE_FLOW = "display:block;width:100%;height:auto;object-fit:contain";

const child = (withScript, pictureStyle, aspect) => `<!doctype html><html><head>
${withScript ? '<script src="/open77-fit.js"></script>' : ""}
</head><body style="margin:0;background:rgb(32,34,37)">
<div id="neko" style="position:absolute;inset:0">
 <main class="neko-main" style="height:100%">
  <div class="video" style="height:100%">
   <div class="video-container" style="height:100%;background:rgba(0,0,0,0.4)">
    <div class="player" style="position:absolute;inset:0;display:flex;justify-content:center;align-items:center;background:#000">
     <div class="player-container" style="position:relative;width:100%;max-width:177.7778vh">
      ${aspect ? '<div class="player-aspect" style="display:block;padding-bottom:56.25%"></div>' : ""}
      <video id="stream" playsinline autoplay muted style="${pictureStyle}"></video>
     </div>
    </div>
   </div>
  </div>
 </main>
</div>
<script>
window.__ready = false;
window.__before = null;
${STREAM}
// What the page laid out BEFORE the script saw a stream, taken from the same
// computed styles the script would change if it changed anything -- and the style
// ATTRIBUTE, which is the page's own and which nothing here should ever write to.
function snapshot() {
  const video = document.getElementById("stream");
  const style = getComputedStyle(video);
  const rect = video.getBoundingClientRect();
  return { rect: [Math.round(rect.width), Math.round(rect.height)], objectFit: style.objectFit,
    position: style.position, width: style.width, height: style.height,
    attribute: video.getAttribute("style") };
}
window.__snapshot = snapshot;
async function connect() {
  const sent = canvas.captureStream(30);
  const a = new RTCPeerConnection(), b = new RTCPeerConnection();
  a.onicecandidate = (e) => e.candidate && b.addIceCandidate(e.candidate);
  b.onicecandidate = (e) => e.candidate && a.addIceCandidate(e.candidate);
  const arrived = new Promise((resolve) => { b.ontrack = (e) => resolve(e.streams[0]); });
  a.addTrack(sent.getVideoTracks()[0], sent);
  await a.setLocalDescription(await a.createOffer());
  await b.setRemoteDescription(a.localDescription);
  await b.setLocalDescription(await b.createAnswer());
  await a.setRemoteDescription(b.localDescription);
  const stream = await arrived;
  const video = document.getElementById("stream");
  window.__before = snapshot();
  video.srcObject = stream;
  try { await video.play(); } catch (e) { window.__playError = String(e); }
  window.__links = [a, b, sent];
  window.__ready = true;
}
window.__connect = connect;
connect();
</script></body></html>`;

// Both scripts on the one page, which is how the room runs them: the cover adds no
// media element at all, so open77-volume.js has only the picture to work on. The
// stream carries an audio track as well as the picture, so the volume script has the
// choice it has in the room.
const childBoth = () => `<!doctype html><html><head>
<script src="/open77-fit.js"></script>
<script src="/open77-volume.js"></script>
</head><body style="margin:0;background:rgb(32,34,37)">
<div class="player" style="position:absolute;inset:0;display:flex;justify-content:center;align-items:center;background:#000">
 <div class="player-container" style="position:relative;width:100%;max-width:177.7778vh">
  <div class="player-aspect" style="display:block;padding-bottom:56.25%"></div>
  <video id="stream" playsinline autoplay style="${PICTURE}"></video>
 </div>
</div>
<script>
window.__ready = false;
${STREAM}
async function connect() {
  const sent = canvas.captureStream(30);
  const tone = new AudioContext();
  const osc = tone.createOscillator();
  const amp = tone.createGain();
  amp.gain.value = 0.05;
  const out = tone.createMediaStreamDestination();
  osc.connect(amp).connect(out);
  osc.start();
  sent.addTrack(out.stream.getAudioTracks()[0]);
  const a = new RTCPeerConnection(), b = new RTCPeerConnection();
  a.onicecandidate = (e) => e.candidate && b.addIceCandidate(e.candidate);
  b.onicecandidate = (e) => e.candidate && a.addIceCandidate(e.candidate);
  const arrived = new Promise((resolve) => { b.ontrack = (e) => resolve(e.streams[0]); });
  a.addTrack(sent.getVideoTracks()[0], sent);
  a.addTrack(sent.getAudioTracks()[0], sent);
  await a.setLocalDescription(await a.createOffer());
  await b.setRemoteDescription(a.localDescription);
  await b.setLocalDescription(await b.createAnswer());
  await a.setRemoteDescription(b.localDescription);
  const stream = await arrived;
  const video = document.getElementById("stream");
  video.srcObject = stream;
  try { await video.play(); } catch (e) { window.__playError = String(e); }
  window.__links = [a, b, sent, tone];
  window.__ready = true;
}
window.__connect = connect;
connect();
</script></body></html>`;

const parent = (width, height) => `<!doctype html><body style="margin:0">
<iframe id="tv" src="/child" allow="autoplay" style="border:0;width:${width}px;height:${height}px"></iframe>
<script>
window.__msgs = [];
addEventListener("message", function (e) {
  const d = e.data;
  if (d && d.open77SharedBrowser === 1) window.__msgs.push(d.kind + " " + d.text);
});
// The same box, framed page after framed page: the scriptless client is the control.
window.__to = async function (src) {
  const frame = document.getElementById("tv");
  frame.src = src;
  while (!frame.contentWindow.__ready) await new Promise((r) => setTimeout(r, 100));
};
// The levels tv.js posts.
window.__postVolume = function (volume, muted) {
  document.getElementById("tv").contentWindow.postMessage(
    { open77SharedBrowserVolume: 1, volume: volume, muted: muted }, location.origin);
};
</script></body>`;

const document_ = (path, query) => {
  if (path.startsWith("/child-both")) return childBoth();
  if (path.startsWith("/child-flow-plain")) return child(false, PICTURE_FLOW, false);
  if (path.startsWith("/child-flow")) return child(true, PICTURE_FLOW, false);
  if (path.startsWith("/child-plain")) return child(false, PICTURE, true);
  if (path.startsWith("/child")) return child(true, PICTURE, true);
  return parent(Number(query.get("w") || 1280), Number(query.get("h") || 728));
};
const server = http.createServer((req, res) => {
  if (req.url.startsWith("/open77-fit.js")) { res.writeHead(200, { "content-type": "text/javascript" }); return res.end(script); }
  if (req.url.startsWith("/open77-volume.js")) { res.writeHead(200, { "content-type": "text/javascript" }); return res.end(volume); }
  const url = new URL(req.url, "http://x");
  res.writeHead(200, { "content-type": "text/html" });
  res.end(document_(url.pathname, url.searchParams));
}).listen(0, "127.0.0.1");
await new Promise((r) => server.once("listening", r));
const origin = "http://127.0.0.1:" + server.address().port;

let failed = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) failed++; };
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

const browser = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});

// One box: a page whose framed client is `box` pixels of viewport, holding the stream.
async function open(width, height, client) {
  const page = await browser.newPage({ viewport: { width, height: Math.max(height, 400) } });
  page.on("pageerror", (e) => console.log("     pageerror " + String(e).slice(0, 140)));
  await page.goto(origin + "/?w=" + width + "&h=" + height, { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => document.getElementById("tv").contentWindow.__ready === true, null, { timeout: 30000 });
  if (client !== "/child") await page.evaluate((src) => window.__to(src), client);
  await wait(400);
  return page;
}

const frame = (page) => page.frames().find((f) => f.url().includes("child"));
const picture = (page) => frame(page).evaluate(() => window.__snapshot());
const before = (page) => frame(page).evaluate(() => window.__before);
const fitState = (page) => frame(page).evaluate(() => (window.__open77Fit ? window.__open77Fit() : null));
const fitReports = (page) => page.evaluate(() => window.__msgs.filter((m) => m.startsWith("fit ")));
const STREAM_SIZE = [1280, 720];
// The script's own arithmetic, done again by the test: what the cover costs on this
// box, as the fraction of the picture's own short side.
const lossFor = (width, height) => {
  const picture = STREAM_SIZE[0] / STREAM_SIZE[1];
  const screen = width / height;
  return screen > picture ? 1 - picture / screen : 1 - screen / picture;
};
const modeFor = (width, height) => {
  const loss = lossFor(width, height);
  const gap = loss * (width / height > STREAM_SIZE[0] / STREAM_SIZE[1] ? width : height);
  return gap <= 1 ? "none" : "cover";
};

// The page as the script left it: the picture's own facts (its class, its inline
// style attribute, its rect, its object-fit) and whether ANYTHING was added to the
// page besides this script's own (unused) stylesheet.
const pageState = (page) => frame(page).evaluate(() => {
  const video = document.getElementById("stream");
  const rect = video.getBoundingClientRect();
  const style = getComputedStyle(video);
  return {
    picture: [Math.round(rect.width), Math.round(rect.height), Math.round(rect.left), Math.round(rect.top)],
    pictureFit: style.objectFit, picturePosition: style.position,
    pictureAttribute: video.getAttribute("style"),
    coverClass: video.classList.contains("open77-cover"),
    covers: Math.abs(rect.width - innerWidth) <= 1 && Math.abs(rect.height - innerHeight) <= 1 &&
      Math.abs(rect.left) <= 1 && Math.abs(rect.top) <= 1,
    layer: document.getElementById("open77-fit"),
    canvases: document.querySelectorAll("canvas[data-open77-fit]").length,
    videos: document.querySelectorAll("video").length,
    styleRule: !!document.querySelector('style[data-open77-fit="cover"]'),
  };
});

// The framed page's own pixels, decoded by the browser that drew them: the box's own
// outer ring (15 % inside every edge) and the stream's own square mark, measured in
// the middle of the picture where the moving dot at its far left cannot reach.
const look = async (page) => {
  const shot = await page.locator("#tv").screenshot();
  return frame(page).evaluate(async ({ b64 }) => {
    const img = new Image();
    img.src = "data:image/png;base64," + b64;
    await img.decode();
    const canvas = document.createElement("canvas");
    canvas.width = img.width; canvas.height = img.height;
    const context = canvas.getContext("2d");
    context.drawImage(img, 0, 0);
    const at = (x, y) => { const d = context.getImageData(x, y, 1, 1).data; return [d[0], d[1], d[2]]; };
    const inset = Math.max(4, Math.round(0.15 * Math.min(img.width, img.height)));
    const ring = [at(inset, img.height >> 1), at(img.width - 1 - inset, img.height >> 1),
      at(img.width >> 1, inset), at(img.width >> 1, img.height - 1 - inset)];
    const white = (c) => c[0] > 200 && c[1] > 200 && c[2] > 200;
    let left = img.width, right = -1, top = img.height, bottom = -1;
    const from = [Math.round(img.width * 0.25), Math.round(img.height * 0.2)];
    const to = [Math.round(img.width * 0.75), Math.round(img.height * 0.8)];
    for (let y = from[1]; y < to[1]; y++) {
      for (let x = from[0]; x < to[0]; x++) {
        if (!white(at(x, y))) continue;
        if (x < left) left = x; if (x > right) right = x;
        if (y < top) top = y; if (y > bottom) bottom = y;
      }
    }
    return { shot: img.width + "x" + img.height, width: img.width, height: img.height, ring,
      mark: right < 0 ? null : { width: right - left + 1, height: bottom - top + 1,
        centre: [Math.round((left + right) / 2), Math.round((top + bottom) / 2)] },
      lit: ring.filter((c) => c[0] + c[1] + c[2] > 90).length,
      dark: ring.filter((c) => c[0] + c[1] + c[2] < 30).length };
  }, { b64: shot.toString("base64") });
};
const lit = (c) => c[0] + c[1] + c[2] > 90;

// The box shapes a television's picture is put in: the 150 ft cinema panel's surface
// (1280x728, 1.76 from the quad), that panel with the control strip up (1.90), the
// 21:9 set (2.37), a 32:9 client's own viewport (3.56), a portrait box (0.56), and a
// box the picture fills exactly (16:9), where nothing is to be cropped at all.
const SHAPES = [
  [1280, 728, "cinema panel 1280x728 (1.76)", "cover"],
  [1280, 675, "cinema panel with the strip 1280x675 (1.90)", "cover"],
  [2560, 1080, "ultrawide 2560x1080 (2.37)", "cover"],
  [5120, 1440, "32:9 client 5120x1440 (3.56)", "cover"],
  [720, 1280, "portrait 720x1280 (0.56)", "cover"],
  [1280, 720, "16:9 1280x720 (1.78)", "none"],
];

// One shape, through everything the answer has to hold, for a given framed client.
async function shapeCheck(shape, width, height, want, page, laid, plain) {
  const state = await fitState(page);
  const made = await pageState(page);
  const read = await look(page);
  const streamShape = state && state.stream ? Number(state.stream.split("x")[0]) / Number(state.stream.split("x")[1]) : 0;
  check(state && state.mode === want && Math.abs(state.loss - lossFor(width, height)) <= 0.002 &&
    Math.abs(streamShape - STREAM_SIZE[0] / STREAM_SIZE[1]) <= 0.01,
    shape + ": the script reads the box the same way the test does: " + JSON.stringify(state));
  if (want === "none") {
    // The box IS the picture's own shape: the page is left exactly as its client laid
    // it out, and nothing is added to it.
    check(state.fitted === false && made.layer === null && made.canvases === 0 && !made.coverClass &&
      made.picture.slice(0, 2).join("x") === laid.rect.join("x") && made.pictureAttribute === laid.attribute,
      shape + ": nothing is cropped and nothing is added where the picture is the box: " +
      JSON.stringify({ state: state, made: made }));
    check(plain !== null && made.picture.slice(0, 2).join(",") === plain.rect.join(",") &&
      made.pictureFit === plain.objectFit,
      shape + ": and the picture is exactly what the same page gives without the script: " +
      JSON.stringify({ with: made.picture, without: plain && plain.rect, fit: made.pictureFit }));
    return;
  }
  // The picture IS the box, scaled to reach every edge and cropped to it: nothing
  // beside it, no layer, no canvas, no second stream, and the page's own inline style
  // attribute untouched (the cover is a class).
  check(state.fitted === true && made.covers && made.pictureFit === "cover" && made.coverClass &&
    made.styleRule && made.layer === null && made.canvases === 0 && made.videos === 1,
    shape + ": the picture is scaled up to cover the screen, and nothing is drawn beside it: " +
    JSON.stringify({ state: state, made: made }));
  check(read.ring.every(lit),
    shape + ": the edges of the box are the picture, not the page's dark grey: " + JSON.stringify(read.ring));
  const mark = read.mark;
  check(mark !== null && Math.abs(mark.width - mark.height) <= 3,
    shape + ": the stream's own square mark is still SQUARE -- the picture is cropped, never stretched: " +
    JSON.stringify(mark));
  check(mark !== null && Math.abs(mark.centre[0] - read.width / 2) <= 4 &&
    Math.abs(mark.centre[1] - read.height / 2) <= 4,
    shape + ": and the crop is symmetric -- the mark is still the middle of the screen: " +
    JSON.stringify({ mark: mark && mark.centre, box: [read.width, read.height] }));
  check(made.pictureAttribute === laid.attribute && laid.objectFit === "contain",
    shape + ": and the page's own style attribute is untouched (`" + made.pictureAttribute +
    "`), the cover being a class rather than something written into the picture");
  const reports = await fitReports(page);
  check(reports.length >= 1 &&
    /^fit picture \d+x\d+ in a \d+x\d+ box \([\d.]+:1\): scaled up until it covers the screen, \d+% of the picture's (height|width) cut from the (top and bottom|left and right), \d+% each, nothing stretched and nothing beside it/.test(reports[reports.length - 1]),
    shape + ": and the television is told what was done: " + JSON.stringify(reports));
}

for (const [width, height, shape, want] of SHAPES) {
  const page = await open(width, height, "/child");
  const plain = await open(width, height, "/child-plain");
  check(modeFor(width, height) === want, shape + ": the test's own arithmetic says " + modeFor(width, height) + " for this box");
  await shapeCheck(shape + " (" + (await look(page)).shot + ")", width, height, want, page,
    await before(page), await picture(plain));
  await page.close();
  await plain.close();
}

// The other client shape: a page that lays the picture out in the document's flow. The
// answer is the same one, and the page's own style attribute is the thing to watch --
// lifting a static picture is the one thing this script writes to it, and it must be
// the ONLY thing, with the cover still a class.
for (const [width, height, shape] of [[5120, 1440, "32:9 client 5120x1440"], [720, 1280, "portrait 720x1280"]]) {
  const flow = await open(width, height, "/child-flow");
  const plain = await open(width, height, "/child-flow-plain");
  const laid = await before(flow);
  const made = await pageState(flow);
  check(laid.position === "static" && made.picturePosition !== "static",
    shape + " in flow: the picture is laid out in the document's flow, and the cover lifts it: " +
    JSON.stringify({ laid: laid.position, lifted: made.picturePosition }));
  await shapeCheck(shape + " in flow", width, height, "cover", flow, laid, await picture(plain));
  await flow.close();
  await plain.close();
}

// The control. The same box, the same stream, the same page -- without the script.
// If this does not read black where the cover fills, the checks above are not
// measuring the gaps the script exists for.
for (const [width, height, shape] of [[5120, 1440, "32:9 client 5120x1440"], [2560, 1080, "ultrawide 2560x1080"], [720, 1280, "portrait 720x1280"]]) {
  const page = await open(width, height, "/child-plain");
  const state = await fitState(page);
  const read = await look(page);
  check(state === null, shape + ": without the script there is no fit to report: " + JSON.stringify(state));
  check(read.dark >= 1 && read.lit >= 1,
    shape + ": and the box's own edges read black -- the bars this script exists for -- with the picture " +
    "itself still lit: " + JSON.stringify({ ring: read.ring, dark: read.dark, lit: read.lit }));
  await page.close();
}

// Both scripts on the one page, as the room runs them. The cover adds no media element
// at all; the sound is the picture element's, and it has to stay there whether it goes
// through the gain or on the element.
{
  const page = await browser.newPage({ viewport: { width: 5120, height: 1440 } });
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  await page.goto(origin + "/?w=5120&h=1440", { waitUntil: "domcontentloaded" });
  await page.waitForFunction(() => document.getElementById("tv").contentWindow.__ready === true, null, { timeout: 30000 });
  await page.evaluate(() => window.__to("/child-both?w=5120&h=1440"));
  await wait(600);
  const made = await pageState(page);
  const state = await fitState(page);
  check(made.coverClass && made.layer === null && made.canvases === 0 && made.videos === 1 &&
    state.mode === "cover",
    "both scripts: the picture covers the box and nothing else is in the page: " +
    JSON.stringify({ made: made, state: state }));
  await page.evaluate(() => __postVolume(0.5, false));
  await wait(1500);
  const sound = await frame(page).evaluate(() => window.__open77Volume());
  const pictureLevel = await frame(page).evaluate(() => {
    const v = document.getElementById("stream");
    return { volume: v.volume, muted: v.muted };
  });
  check(sound.hooked ? sound.on === "stream" : (Math.abs(pictureLevel.volume - 0.5) < 1e-6 && !pictureLevel.muted),
    "both scripts: the sound is the picture element's -- through the gain, or on the element: " +
    JSON.stringify({ sound: sound, picture: pictureLevel }));
  // The gesture that lets Web Audio run where a browser waits for one (the room's page
  // gets one the moment the player touches anything). The picture's own box, not the
  // body: this page has nothing in the flow, so its body has no size to click.
  await frame(page).click(".player");
  await wait(1800);
  const after = await frame(page).evaluate(() => window.__open77Volume());
  check(after.hooked && after.on === "stream" && after.audio === "running",
    "both scripts: with Web Audio running the picture carries the sound: " + JSON.stringify(after));
  check(errors.length === 0, "both scripts: no page errors: " + JSON.stringify(errors));
  await page.close();
}

await browser.close();
server.close();
console.log(failed === 0 ? "ALL PASSED" : failed + " FAILED");
process.exit(failed === 0 ? 0 : 1);
