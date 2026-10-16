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
// What the script has to do now is one of two things, and neither of them is the
// picture a second time (see the header of build/open77-fit.js for the four fills a
// television has rejected):
//
//   * COVER, where the box is within a third of the picture's shape: the picture is
//     scaled up, keeping its shape, until it reaches every edge of the box, and the
//     overhang is cut. Measured here as: the picture element IS the box, its computed
//     `object-fit` is `cover`, the box's own edges are the picture rather than the
//     page's dark grey, the synthetic stream's square white mark is still SQUARE in
//     the box (a stretched picture would show a rectangle) and still centred (so the
//     crop is symmetric), and no layer and no gap is left anywhere.
//   * A GLOW, where covering would cost more than a third of the picture: the picture
//     is left exactly as the client laid it out -- the same rect, the same object-fit,
//     the same inline style, as the same page without the script -- and each gap
//     carries the picture's own edge COLOURS as a wash. Measured here as an equality
//     the script cannot fake: the wash's own stops, read pixel for pixel out of the
//     bars, are the picture's edge strip (drawn small, the same way the script draws
//     it, and dimmed the same) to within rounding. A bar of the video again, or one
//     of a stretched column of the picture, cannot pass it.
//
// The layer's own geometry is unchanged from the version a television reported as
// "another part of the video" and from the one it reported as a stretched smear: put
// immediately before the picture inside the picture's own box, sized to the whole box,
// taking no clicks, one sharp canvas per gap, placed exactly on that gap, carrying no
// second stream and no filter -- and BELOW the picture.
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
// top and bottom edges and a white mark in the middle. The bands are how "the
// picture's own rows are still here" is read, and the mark is how a stretch is
// caught: it is square in the stream, so it has to be square in the box.
// The one moving thing -- a dot at y=200, outside the middle of the picture that
// the mark is measured in -- is what keeps the stream live: real frames, not a still.
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
// A client that lays the picture out in the document's flow instead: same box,
// same shape, not positioned -- the case that needs the script's one guard.
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
window.__styleBefore = null;
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

// Both scripts on the one page, which is how the room runs them: the fill's bars are
// canvases drawn from the picture, and open77-volume.js has to leave them alone --
// the sound is the picture element's. The stream carries an audio track as well as
// the picture, so the volume script has the choice it has in the room.
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
// The same box, framed page after framed page: the scriptless client is the
// control, and the flow client is the one that has to be lifted.
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

// One box: a page whose framed client is `box` pixels of viewport, holding the
// stream. `client` is which framed page to load (the neko-shaped one by default).
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
// The script's arithmetic, done again by the test: what a cover would cost on this
// box, as the fraction of the picture's own short side.
const lossFor = (width, height) => {
  const picture = STREAM_SIZE[0] / STREAM_SIZE[1];
  const screen = width / height;
  return screen > picture ? 1 - picture / screen : 1 - screen / picture;
};
const CROP_LIMIT = 1 / 3;
const modeFor = (width, height) => {
  const loss = lossFor(width, height);
  if (loss * (width / height > 16 / 9 ? width : height) <= 1) return "none";
  return loss <= CROP_LIMIT ? "cover" : "glow";
};

// The framed page's own pixels, decoded by the browser that drew them.
const look = async (page, rows = []) => {
  const shot = await page.locator("#tv").screenshot();
  return frame(page).evaluate(async ({ b64, rows }) => {
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
    // The stream's own square mark, measured where it can be: the middle of the
    // picture, away from the moving dot at its far left. A picture stretched into
    // the box turns this square into a rectangle.
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
      at: rows.map((y) => (y >= 0 && y < img.height ? at(img.width >> 1, y) : null)),
      lit: ring.filter((c) => c[0] + c[1] + c[2] > 90).length,
      dark: ring.filter((c) => c[0] + c[1] + c[2] < 30).length };
  }, { b64: shot.toString("base64"), rows });
};
const lit = (c) => c[0] + c[1] + c[2] > 90;
// Two renderings of the same colour, so anything but a near match is a different one.
const near = (a, b, tolerance = 8) => a.every((n, i) => Math.abs(n - b[i]) <= tolerance);

// What the script did to the page, as the page holds it: the picture's own facts
// (its class, its inline style attribute, its size, its object-fit) and, when there is
// one, the layer -- where it is, how big, and the bars it is made of: one per gap,
// each the whole box, its own gap exactly, taking no clicks, sharp, and with no second
// stream inside it. There is no layer in the cover case: the picture itself is the
// answer there, so this reads both and the checks below say which one they want.
const pageState = (page) => frame(page).evaluate(() => {
  const video = document.getElementById("stream");
  const element = document.getElementById("open77-fit");
  const pictureRect = video.getBoundingClientRect();
  const state = {
    layer: element, bars: [], streams: 0, before: false, position: null, pointer: null, covers: false,
    picture: getComputedStyle(video).position,
    pictureAttribute: video.getAttribute("style"),
    coverClass: video.classList.contains("open77-cover"),
    pictureFit: getComputedStyle(video).objectFit,
    pictureRect: [Math.round(pictureRect.width), Math.round(pictureRect.height)],
    pictureCovers: Math.abs(pictureRect.width - innerWidth) <= 1 &&
      Math.abs(pictureRect.height - innerHeight) <= 1 && Math.abs(pictureRect.left) <= 1 &&
      Math.abs(pictureRect.top) <= 1,
    styleRule: !!document.querySelector('style[data-open77-fit="cover"]'),
  };
  if (!element) return state;
  const rect = element.getBoundingClientRect();
  state.before = element.nextElementSibling === video && element.parentElement === video.parentElement;
  state.position = getComputedStyle(element).position;
  state.pointer = getComputedStyle(element).pointerEvents;
  state.covers = Math.abs(rect.width - innerWidth) <= 1 && Math.abs(rect.height - innerHeight) <= 1 &&
    Math.abs(rect.left) <= 1 && Math.abs(rect.top) <= 1;
  state.streams = element.querySelectorAll("video").length;
  state.bars = Array.from(element.querySelectorAll("canvas")).map((bar) => {
    const style = getComputedStyle(bar);
    const size = bar.getBoundingClientRect();
    return {
      side: bar.getAttribute("data-open77-fit"),
      rect: [Math.round(size.left), Math.round(size.top), Math.round(size.width), Math.round(size.height)],
      filter: style.filter, pointer: style.pointerEvents,
      backing: bar.width === Math.max(1, Math.round(size.width * Math.min(2, devicePixelRatio || 1))) &&
        bar.height === Math.max(1, Math.round(size.height * Math.min(2, devicePixelRatio || 1))),
    };
  });
  return state;
});
// A bar has to BE its gap: exactly that rectangle of the box, one canvas per gap, no
// second stream anywhere in the layer, and no filter anywhere (a blur is what one day
// of this script did).
const placeFor = (side, boxWidth, boxHeight, gaps) => {
  if (side === "left") return [0, 0, gaps.left, boxHeight];
  if (side === "right") return [boxWidth - gaps.right, 0, gaps.right, boxHeight];
  if (side === "top") return [gaps.left, 0, boxWidth - gaps.left - gaps.right, gaps.top];
  return [gaps.left, boxHeight - gaps.bottom, boxWidth - gaps.left - gaps.right, gaps.bottom];
};
const layerCheck = (shape, boxWidth, boxHeight, state, made) => {
  const sides = state.sides.slice().sort().join();
  const placed = (bar) => placeFor(bar.side, boxWidth, boxHeight, state.gaps)
    .every((n, i) => Math.abs(bar.rect[i] - n) <= 1);
  check(made && made.before && made.position === "fixed" && made.pointer === "none" && made.covers &&
    made.picture !== "static" && made.bars.map((b) => b.side).sort().join() === sides &&
    made.bars.every((b) => b.filter === "none" && b.pointer === "none" && b.backing && placed(b)) &&
    made.bars.length === state.bars && made.streams === 0 && !made.coverClass,
    shape + ": the layer is immediately before the picture inside its own box, covering the whole box, " +
    "taking no clicks, one SHARP canvas (`filter: none`) per gap placed exactly on that gap and no more " +
    "(`" + JSON.stringify(made && made.bars.map((b) => [b.side, b.rect])) + "` against the gaps " +
    JSON.stringify(state.gaps) + "), carrying NO second stream (`videos: " + (made && made.streams) +
    "`), and the picture is NOT wearing the cover: " +
    JSON.stringify({ before: made && made.before, position: made && made.position,
      pointer: made && made.pointer, covers: made && made.covers, bars: made && made.bars.length }));
};

// The picture's own edge, averaged into the wash's stops -- by the test, the same way
// the script does it: a strip of the picture's edge drawn small, dimmed. Read from the
// picture element itself, so the comparison is against the frames the bars were drawn
// from rather than against an opinion of them.
const expectedStops = (page, side) => frame(page).evaluate(async ({ side }) => {
  const video = document.getElementById("stream");
  const column = side === "left" || side === "right";
  const count = 6, strip = 12;
  const canvas = document.createElement("canvas");
  canvas.width = column ? 1 : count;
  canvas.height = column ? count : 1;
  const context = canvas.getContext("2d", { willReadFrequently: true });
  context.imageSmoothingEnabled = true;
  const near_ = side === "left" || side === "top";
  const width = video.videoWidth, height = video.videoHeight;
  if (column) {
    const x = near_ ? 0 : Math.max(0, width - strip);
    context.drawImage(video, x, 0, Math.min(strip, width), height, 0, 0, 1, count);
  } else {
    const y = near_ ? 0 : Math.max(0, height - strip);
    context.drawImage(video, 0, y, width, Math.min(strip, height), 0, 0, count, 1);
  }
  const data = context.getImageData(0, 0, canvas.width, canvas.height).data;
  const stops = [];
  for (let i = 0; i < count; i++) {
    stops.push([Math.round(data[i * 4] * 0.6), Math.round(data[i * 4 + 1] * 0.6), Math.round(data[i * 4 + 2] * 0.6)]);
  }
  return stops;
}, { side });

// The wash, read pixel for pixel out of the bars: for each gap, the bar's own pixel at
// each stop the gradient is built from, next to the expected stop. The bar's pixel at
// a stop's offset is exactly that stop, so this is an equality, not an impression --
// and a gap of the video again, or of the picture's edge column stretched sideways,
// cannot read the picture's edge strip at six levels into a smooth wash.
const wash = (page) => frame(page).evaluate(() => {
  const element = document.getElementById("open77-fit");
  const video = document.getElementById("stream");
  if (!element) return null;
  const inner = video.getBoundingClientRect();
  return Array.from(element.querySelectorAll("canvas")).map((bar) => {
    const box = bar.getBoundingClientRect();
    const column = bar.getAttribute("data-open77-fit") === "left" || bar.getAttribute("data-open77-fit") === "right";
    const ratio = box.width > 0 ? bar.width / box.width : 1;
    const span = column ? box.height : box.width;
    const start = Math.max(0, Math.round(column ? inner.top - box.top : inner.left - box.left));
    const end = Math.min(span, Math.round(column ? inner.top + inner.height - box.top : inner.left + inner.width - box.left));
    const context = bar.getContext("2d");
    const read = [];
    for (let i = 0; i < 6; i++) {
      const at = Math.round((start + (i / 5) * (end - start)) * ratio);
      const x = column ? Math.round(bar.width / 2) : Math.min(bar.width - 1, Math.max(0, at));
      const y = column ? Math.min(bar.height - 1, Math.max(0, at)) : Math.round(bar.height / 2);
      const d = context.getImageData(x, y, 1, 1).data;
      read.push([d[0], d[1], d[2]]);
    }
    return { side: bar.getAttribute("data-open77-fit"), start, end, read };
  });
});

// The box shapes a television's picture is put in: the 150 ft cinema panel's surface
// (1280x728, 1.76 from the quad), that panel with the control strip up (1280x675,
// 1.90), the 21:9 set (2560x1080), a 32:9 client's own viewport (5120x1440 -- the one
// whose height a cover would take half of), a portrait box, and a box the picture
// fills exactly (1280x720), where nothing is to be cropped and nothing to be filled.
const SHAPES = [
  [1280, 728, "cinema panel 1280x728 (1.76)", "cover"],
  [1280, 675, "cinema panel with the strip 1280x675 (1.90)", "cover"],
  [2560, 1080, "ultrawide 2560x1080 (2.37)", "cover"],
  [5120, 1440, "32:9 client 5120x1440 (3.56)", "glow"],
  [720, 1280, "portrait 720x1280 (0.56)", "glow"],
  [1280, 720, "16:9 1280x720 (1.78)", "none"],
];

for (const [width, height, shape, want] of SHAPES) {
  const page = await open(width, height, "/child");
  const laid = await before(page);
  const state = await fitState(page);
  const read = await look(page);
  const made = await pageState(page);
  const expected = modeFor(width, height);
  check(expected === want, shape + ": the test's own arithmetic says " + expected + " for this box");
  // The stream's own resolution is the peer connection's business and moves while it
  // settles; its SHAPE is the picture's, and is what every number below is about.
  const streamShape = state && state.stream ? Number(state.stream.split("x")[0]) / Number(state.stream.split("x")[1]) : 0;
  check(state && state.mode === want && Math.abs(state.loss - lossFor(width, height)) <= 0.002 &&
    Math.abs(streamShape - STREAM_SIZE[0] / STREAM_SIZE[1]) <= 0.01,
    shape + ": the script reads the box the same way the test does: " + JSON.stringify(state));

  if (want === "cover") {
    // The picture IS the box, scaled to reach every edge and cropped to it: the box's
    // own edges are the picture, the stream's square mark is still square and still
    // centred, and no layer and no gap is left anywhere.
    check(made.pictureCovers && made.pictureFit === "cover" && made.coverClass && made.styleRule &&
      made.layer === null && made.bars.length === 0 && made.streams === 0,
      shape + ": the picture is scaled up to cover the screen and nothing is drawn beside it: " +
      JSON.stringify({ pictureFit: made.pictureFit, class: made.coverClass, rule: made.styleRule,
        layer: made.layer, bars: made.bars.length, pictureCovers: made.pictureCovers }));
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
    // Nothing was written to the page's own picture element to do it: the class is
    // this script's, the style attribute is the page's and is byte for byte the one
    // the page wrote before a stream existed.
    check(made.pictureAttribute === laid.attribute && laid.objectFit === "contain",
      shape + ": and the page's own style attribute is untouched (`" + made.pictureAttribute +
      "`), the cover being a class rather than something written into the picture");
    const reports = await fitReports(page);
    check(reports.length >= 1 &&
      /^fit picture \d+x\d+ in a \d+x\d+ box \([\d.]+:1\): scaled up until it covers the screen, \d+% of the picture's (height|width) cut from the (top and bottom|left and right), \d+% each, nothing stretched and no second copy anywhere/.test(reports[reports.length - 1]),
      shape + ": and the television is told what was done: " + JSON.stringify(reports));
  } else if (want === "glow") {
    // The picture exactly as the client laid it out, and every gap a wash of its own
    // edge colours -- equal, stop for stop, to the strip read out of the picture.
    check(made.layer !== null && made.covers && !made.coverClass && made.pictureFit === "contain" &&
      made.pictureAttribute === laid.attribute &&
      made.pictureRect.join("x") === laid.rect.join("x"),
      shape + ": the picture is left exactly as the client laid it out, and the gap is filled beside it: " +
      JSON.stringify({ fit: made.pictureFit, rect: made.pictureRect, laid: laid.rect,
        covers: made.covers, class: made.coverClass }));
    layerCheck(shape, width, height, state, made);
    check(read.ring.every(lit),
      shape + ": and the box's own edges are the picture's colours rather than black: " + JSON.stringify(read.ring));
    const stops = await wash(page);
    const wrong = [];
    for (const bar of stops) {
      const want_ = await expectedStops(page, bar.side);
      bar.read.forEach((colour, i) => {
        if (!near(colour, want_[i])) wrong.push({ side: bar.side, stop: i, washed: colour, picture: want_[i] });
      });
    }
    check(stops !== null && stops.length === state.bars && wrong.length === 0,
      shape + ": and every bar is the picture's own edge colours, stop for stop, dimmed: " +
      (wrong.length ? JSON.stringify(wrong) : JSON.stringify(stops)));
    const reports = await fitReports(page);
    check(reports.length >= 1 &&
      /^fit picture \d+x\d+ in a \d+x\d+ box \([\d.]+\): the picture whole -- covering it would cost \d+% of the picture -- and its own edge colours, dimmed, in the gap: .+, nothing stretched, nothing cropped, no second copy/.test(reports[reports.length - 1]),
      shape + ": and the television is told what was done: " + JSON.stringify(reports));
  } else {
    check(state && state.fitted === false && made.layer === null && made.bars.length === 0 &&
      !made.coverClass,
      shape + ": nothing is cropped and nothing is filled where the picture is the box: " +
      JSON.stringify({ state: state, layer: made.layer, bars: made.bars.length, cover: made.coverClass }));
  }
  await page.close();
}

// The other client shape: a page that lays the picture out in the document's flow. It
// is not positioned, so the page's order alone would not keep it over the layer -- the
// script has to lift it, and lifting it must not move, resize, crop or blur it (what
// the same client, without the script, laid out).
for (const [width, height, shape] of [[5120, 1440, "32:9 client 5120x1440"], [720, 1280, "portrait 720x1280"]]) {
  const flow = await open(width, height, "/child-flow");
  const plain = await open(width, height, "/child-flow-plain");
  const laid = await before(flow);
  const same = await picture(plain);
  const after = await picture(flow);
  const state = await fitState(flow);
  const made = await pageState(flow);
  const read = await look(flow);
  check(modeFor(width, height) === "glow" && laid.position === "static" && same.position === "static" &&
    after.position === "relative",
    shape + " in flow: the picture is laid out in the document's flow, and the script lifts it -- only that: " +
    JSON.stringify({ laid: laid.position, lifted: after.position, without: same.position }));
  check(after.rect[0] === same.rect[0] && after.rect[1] === same.rect[1] &&
    after.width === same.width && after.height === same.height && after.objectFit === same.objectFit,
    shape + " in flow: the picture keeps the size, the shape and the object-fit the same client gives it " +
    "without the script: " + JSON.stringify({ with: after, without: same }));
  layerCheck(shape + " in flow", width, height, state, made);
  check(read.ring.every(lit) && made && made.before,
    shape + " in flow: the picture is over the layer and the box's own edges are the picture's colours: " +
    JSON.stringify({ ring: read.ring }));
  const stops = await wash(flow);
  const wrong = [];
  for (const bar of stops) {
    const want_ = await expectedStops(flow, bar.side);
    bar.read.forEach((colour, i) => {
      if (!near(colour, want_[i])) wrong.push({ side: bar.side, stop: i, washed: colour, picture: want_[i] });
    });
  }
  check(stops !== null && stops.length === state.bars && wrong.length === 0,
    shape + " in flow: and every bar is the picture's own edge colours, stop for stop: " +
    (wrong.length ? JSON.stringify(wrong) : JSON.stringify(stops)));
  await flow.close();
  await plain.close();
}

// The control. The same box, the same stream, the same page -- without the script.
// If this does not read black, the checks above are not measuring the gaps. Every
// shape here is one whose gap the ring lands inside: on a box the picture very nearly
// fills (the cinema panel, 14 px of gap) the ring lands on the picture itself, and no
// pixel can tell that shape's gap from its picture -- which is why the layer and the
// cover are also asserted directly, above.
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

// Both scripts on the one page, as the room runs them. The fill's bars are canvases
// drawn from the picture; the sound is the picture element's. What is held here is
// that a level the television posts never lands on a bar -- not a level, and above all
// not an unmute, which without Web Audio would play the stream a second time -- and
// that the sound stays on the picture, whether it goes through the gain or on the
// element.
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
  check(made.layer !== null && made.bars.length === 2 && made.streams === 0 &&
    made.bars.every((b) => b.filter === "none") && state.mode === "glow",
    "both scripts: the gap is filled with bars that are canvases, sharp, carrying no stream of their own: " +
    JSON.stringify({ bars: made && made.bars, streams: made && made.streams, state: state }));
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
  // The gesture that lets Web Audio run where a browser waits for one (the room's
  // page gets one the moment the player touches anything). The picture's own box, not
  // the body: this page has nothing in the flow, so its body has no size to click.
  await frame(page).click(".player");
  await wait(1800);
  const after = await frame(page).evaluate(() => window.__open77Volume());
  check(after.hooked && after.on === "stream" && after.audio === "running",
    "both scripts: with Web Audio running the picture carries the sound: " + JSON.stringify(after));
  check((await fitState(page)).streams === 0 && (await fitState(page)).bars === 2,
    "both scripts: and the bars still carry nothing but the wash: " + JSON.stringify(await fitState(page)));
  check(errors.length === 0, "both scripts: no page errors: " + JSON.stringify(errors));
  await page.close();
}

await browser.close();
server.close();
console.log(failed === 0 ? "ALL PASSED" : failed + " FAILED");
process.exit(failed === 0 ? 0 : 1);
