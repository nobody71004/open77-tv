// The real client page, in the real container, framed in a box the way a
// television frames it -- and what its own image's open77-fit.js did to the box,
// measured in the page and in the pixels the browser drew. Run ON the VPS in
// Playwright's image (host networking, as a player's PC reaches it). The link
// (viewer password) is read from /secret and never printed.
//
// The measurement is the same box twice, and the control comes FIRST: the page as an
// image with no open77-fit.js leaves it, whose gaps are the client page's own black
// (0,0,0) -- that black is the stripe a player reported -- and which also fixes the
// points this test samples. Then the same box with the script, at those same points.
//
// The answer is one answer, on both boxes, and it is decided here from the box and the
// picture's own shape (the rect the client laid the picture out in), not from what the
// script says it did: THE PICTURE FILLS THE BOX. The picture element IS the box, it is
// wearing the cover class with `object-fit: cover` (the browser's own uniform scale, so
// the picture is cropped and never stretched), no layer exists at all -- five fills with
// something beside the picture were measured on a television on 2026-10-16 and all five
// were rejected (the last one, the picture's own edge colours as a dimmed wash, as "the
// blur side bars are back") -- and every point that was the page's own black is now the
// picture.
//
// Exits non-zero when a box the picture does not fill is left black, when the picture is
// not the box, or when anything at all has been added beside it.
//
// usage: node e2e-fit.mjs <seconds> <box width> <box height>
import fs from "node:fs";
import http from "node:http";
import { chromium } from "playwright";

const link = fs.readFileSync("/secret", "utf8").trim().replace(/#open77-shared-browser$/, "");
const seconds = Number(process.argv[2] || 30);
const width = Number(process.argv[3] || 1280);
const height = Number(process.argv[4] || 728);

const server = http.createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/html" });
  res.end('<!doctype html><body style="margin:0;background:#111"><iframe id="tv" style="border:0;width:' + width +
    "px;height:" + height + 'px" allow="autoplay, fullscreen; clipboard-read; clipboard-write" ' +
    "referrerpolicy=\"no-referrer\"></iframe></body>");
}).listen(0, "127.0.0.1");
await new Promise((r) => server.once("listening", r));

const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage({ viewport: { width, height: Math.max(height, 400) } });
await page.goto("http://127.0.0.1:" + server.address().port + "/");
const url = new URL(link);
url.searchParams.set("open77", "2");
const enter = () => page.evaluate((src) => { document.getElementById("tv").src = src; }, url.toString());

const frameOf = () => page.frames().find((f) => f !== page.mainFrame());
// The picture: the client's own video. The bars add none of their own -- they are
// canvases drawn from the picture's frames -- so the first video in the page is it. The
// rect it is PAINTED at is computed here from the element, its object-fit and the
// stream's own size, because the element is not always the picture: neko's client gives
// it a 16:9 box with `object-fit: contain` inside the screen box, which is exactly where
// the bars live.
const measuring = async () => {
  const frame = frameOf();
  if (!frame) return null;
  return frame.evaluate(() => {
    const picture = [...document.querySelectorAll("video")].find((v) => !v.closest("#open77-fit"));
    if (!picture || !picture.videoWidth) return null;
    const layer = document.getElementById("open77-fit");
    const rect = (element) => {
      const box = element.getBoundingClientRect();
      return [Math.round(box.width), Math.round(box.height), Math.round(box.left), Math.round(box.top)];
    };
    const element = rect(picture);
    const style = getComputedStyle(picture);
    const scale = style.objectFit === "cover"
      ? Math.max(element[0] / picture.videoWidth, element[1] / picture.videoHeight)
      : style.objectFit === "none" ? 1
        : Math.min(element[0] / picture.videoWidth, element[1] / picture.videoHeight);
    const shown = [picture.videoWidth * scale, picture.videoHeight * scale];
    const at = (style.objectPosition || "50% 50%").split(/\s+/).map((value, i) => {
      const room = (i === 0 ? element[0] : element[1]) - (i === 0 ? shown[0] : shown[1]);
      if (value === undefined || value === "center") return room / 2;
      if (value === "left" || value === "top") return 0;
      if (value === "right" || value === "bottom") return room;
      if (value.slice(-1) === "%") return parseFloat(value) / 100 * room;
      return room / 2;
    });
    const inner = [element[2] + at[0], element[3] + at[1], shown[0], shown[1]];
    // Five samples inside each gap: along its length, which for a bar down the side is
    // the vertical axis and for one across the top is the horizontal one.
    const left = inner[0], right = inner[0] + inner[2], top = inner[1], bottom = inner[1] + inner[3];
    const spreadY = [0.2, 0.35, 0.5, 0.65, 0.8].map((f) => Math.round(f * (innerHeight - 1)));
    const spreadX = [0.2, 0.35, 0.5, 0.65, 0.8].map((f) => Math.round(f * (innerWidth - 1)));
    const spots = [];
    const sides = [];
    if (left >= 2) { sides.push("left"); for (const y of spreadY) spots.push([Math.floor(left / 2), y]); }
    if (innerWidth - right >= 2) { sides.push("right"); for (const y of spreadY) spots.push([innerWidth - 1 - Math.floor((innerWidth - right) / 2), y]); }
    if (top >= 2) { sides.push("top"); for (const x of spreadX) spots.push([x, Math.floor(top / 2)]); }
    if (innerHeight - bottom >= 2) { sides.push("bottom"); for (const x of spreadX) spots.push([x, innerHeight - 1 - Math.floor((innerHeight - bottom) / 2)]); }
    const layerRect = layer ? rect(layer) : null;
    const bars = layer ? layer.querySelectorAll("canvas") : [];
    return {
      box: [innerWidth, innerHeight], element: element, picture: inner, sides: sides, spots: spots,
      pictureFit: style.objectFit, picturePosition: style.position,
      pictureAttribute: picture.getAttribute("style"),
      coverClass: picture.classList.contains("open77-cover"),
      stream: [picture.videoWidth, picture.videoHeight],
      layer: layer ? { rect: layerRect, fixed: getComputedStyle(layer).position,
        pointer: getComputedStyle(layer).pointerEvents,
        covers: Math.abs(layerRect[0] - innerWidth) <= 1 && Math.abs(layerRect[1] - innerHeight) <= 1 &&
          Math.abs(layerRect[2]) <= 1 && Math.abs(layerRect[3]) <= 1,
        under: layer.nextElementSibling === picture && layer.parentElement === picture.parentElement,
        // One canvas per bar, sharp, and -- the whole point of the bars being drawn
        // from the picture's own frames -- NO second stream anywhere in the layer.
        bars: bars.length, streams: layer.querySelectorAll("video").length,
        filter: bars.length ? getComputedStyle(bars[0]).filter : null } : null,
      fit: typeof window.__open77Fit === "function" ? window.__open77Fit() : null,
    };
  }).catch(() => null);
};

// What those points read, from the pixels the browser drew for the framed page.
const wash = async (points) => {
  if (!points.length) return [];
  const shot = await page.locator("#tv").screenshot();
  const frame = frameOf();
  return frame.evaluate(async ({ b64, spots }) => {
    const img = new Image();
    img.src = "data:image/png;base64," + b64;
    await img.decode();
    const canvas = document.createElement("canvas");
    canvas.width = img.width; canvas.height = img.height;
    const context = canvas.getContext("2d");
    context.drawImage(img, 0, 0);
    return spots.map(([x, y]) => {
      const d = context.getImageData(x, y, 1, 1).data;
      return [d[0], d[1], d[2]];
    });
  }, { b64: shot.toString("base64"), spots: points });
};

// The picture over WebRTC: wait for it the way a player waits, then let the room's own
// desktop paint (a fresh stream shows a dark frame while it decodes).
const settle = async () => {
  const until = Date.now() + seconds * 1000;
  let state = null;
  while (Date.now() < until) {
    await page.waitForTimeout(1000);
    state = await measuring();
    if (state && state.stream && state.stream[0]) break;
    state = null;
  }
  await page.waitForTimeout(6000);
  return (await measuring()) || state;
};

const failures = [];
const ok = (condition, what) => { if (!condition) failures.push(what); };

// The control first: the page as an image with no open77-fit.js leaves it. Its gaps are
// the client page's own black -- the stripe a player reported -- and its picture's rect
// is the picture's own shape, which is what says whether this box needs an answer at all.
// The image's own tag is versioned (`open77-fit.js?v=3`, the cache-buster), so the
// pattern has to match the query too, or the "control" run is a run WITH the script.
await page.route("**/open77-fit.js*", (route) => route.abort());
await enter();
const bare = await settle();
ok(!!bare && !!bare.stream[0], "no picture: the stream never arrived in " + seconds + "s (control)");
const withoutScript = await wash(bare ? bare.spots : []);
// The client page's own black reads exactly (0,0,0) wherever the picture does not
// reach -- which is what makes it a control: a frame of decoded video, even a very dark
// one, is not three zeroes, and every colour this script paints is at least one.
const black = (spot) => spot[0] === 0 && spot[1] === 0 && spot[2] === 0;
const needsFill = !!bare && (bare.picture[2] < bare.box[0] - 2 || bare.picture[3] < bare.box[1] - 2);
ok(!needsFill || withoutScript.every(black),
  "the control is not a control: this box is not black without the script: " + JSON.stringify(withoutScript));

// What the box and the stream's own size say the answer has to be -- the script's rule,
// done again here, so the checks below cannot be satisfied by the script agreeing with
// itself.
const pictureShape = bare ? bare.stream[0] / bare.stream[1] : 0;
const screenShape = bare ? bare.box[0] / bare.box[1] : 0;
const loss = screenShape > pictureShape ? 1 - pictureShape / screenShape : 1 - screenShape / pictureShape;
const answer = !needsFill ? "none" : "cover";

// Then the same box with the script, at the same points.
await page.unroute("**/open77-fit.js*");
await enter();
const withScript = await settle();
ok(!!withScript && !!withScript.stream[0], "no picture: the stream never arrived in " + seconds + "s (with the script)");
const pixels = await wash(bare ? bare.spots : []);
const fit = withScript && withScript.fit;
const agreed = !!fit && fit.mode === answer && Math.abs((fit.loss || 0) - loss) <= 0.005;
ok(agreed, "the script's own reading of the box is not the box: " +
  JSON.stringify({ fit: fit, loss: Math.round(loss * 1000) / 1000, answer: answer }));

let covered = null;
if (needsFill) {
  covered = {
    element: withScript.element[0] === withScript.box[0] && withScript.element[1] === withScript.box[1] &&
      withScript.element[2] <= 1 && withScript.element[3] <= 1,
    fit: withScript.pictureFit, coverClass: withScript.coverClass, layer: withScript.layer === null,
  };
  ok(covered.element && covered.fit === "cover" && covered.coverClass && covered.layer,
    "cover: the picture is not the box, wearing the cover class, with no layer beside it: " + JSON.stringify(covered));
  // Every point that was the client page's own black without the script is the picture
  // with it: the stripe is gone, which is the whole complaint this answers.
  const stillBlack = pixels.filter((spot, i) => black(spot) && black(withoutScript[i] || [0, 0, 0]));
  ok(pixels.length > 0 && stillBlack.length === 0,
    "cover: a point that was black without the script is still black with it: " + JSON.stringify(stillBlack));
  ok(fit && /scaled up until it covers the screen/.test(fit.line || ""),
    "cover: and the television is told so: " + JSON.stringify(fit && fit.line));
  // And nothing at all beside the picture: no layer, no bars, no second stream.
  ok(withScript.layer === null && fit.bars === 0 && fit.streams === 0,
    "cover: something is still in the page beside the picture: " +
    JSON.stringify({ layer: withScript.layer, bars: fit.bars, streams: fit.streams }));
}

console.log(JSON.stringify({ box: [width, height], answer: answer, loss: Math.round(loss * 1000) / 1000,
  picture: bare && bare.picture, cover: covered,
  edgeWithoutScript: withoutScript, withScript: pixels,
  fit: fit && { mode: fit.mode, bars: fit.bars, streams: fit.streams, line: fit.line },
  failures: failures }, null, 1));
console.log((failures.length === 0 ? "ok   " : "FAIL ") + width + "x" + height + ": " + answer +
  " (" + (needsFill ? Math.round(loss * 100) + "% of the picture" : "the box is the picture's own shape") +
  ")" + (failures.length ? " -- " + failures.join(" | ") : ", and every check passed"));

await browser.close();
server.close();
process.exit(failures.length === 0 ? 0 : 1);
