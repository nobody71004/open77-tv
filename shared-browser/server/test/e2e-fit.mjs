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
// Two answers are checked, and which one is right is decided here from the box and the
// picture's own shape (the rect the client laid the picture out in), not from what the
// script says it did:
//
//   * COVER (the box is within a third of the picture's shape): the picture element IS
//     the box, wearing the cover class, with `object-fit: cover` -- the browser's own
//     uniform scale, so the picture is cropped and never stretched -- no layer exists,
//     and every point that was the page's black is now the picture.
//   * GLOW (covering would cost more than a third of the picture): the picture is
//     exactly the rect, the object-fit and the inline style it has without the script,
//     the layer is a layer of the whole box under the picture (one sharp canvas per
//     gap, no second stream), no bar is black, and each bar's own pixels are the
//     picture's own edge strip, drawn small and dimmed the way the script draws it.
//
// Exits non-zero when a box the picture does not fill is left black, when the picture
// was cropped where it should have been left whole (or the other way round), when the
// layer is not a layer of the whole box under the picture, or when the bars are not the
// picture's own edge colours.
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

// The bars against the picture's own edge, read the way the script builds them: a strip
// of the picture's edge drawn small into the same number of stops, dimmed the same. Both
// reads happen in one turn of the page's own JavaScript, so the frames behind them are as
// close as two reads can be -- and the bars are the picture's own frames by construction,
// so anything that differs is a bar that is not the picture's edge.
const stops = (side) => frameOf().evaluate((side) => {
  const count = 6, strip = 12;
  const video = [...document.querySelectorAll("video")].find((v) => !v.closest("#open77-fit"));
  const element = document.getElementById("open77-fit");
  const bar = [...element.querySelectorAll("canvas")].find((c) => c.getAttribute("data-open77-fit") === side);
  if (!bar) return null;
  const column = side === "left" || side === "right";
  const near_ = side === "left" || side === "top";
  const width = video.videoWidth, height = video.videoHeight;
  const scratch = document.createElement("canvas");
  scratch.width = column ? 1 : count;
  scratch.height = column ? count : 1;
  const context = scratch.getContext("2d", { willReadFrequently: true });
  context.imageSmoothingEnabled = true;
  if (column) {
    const x = near_ ? 0 : Math.max(0, width - strip);
    context.drawImage(video, x, 0, Math.min(strip, width), height, 0, 0, 1, count);
  } else {
    const y = near_ ? 0 : Math.max(0, height - strip);
    context.drawImage(video, 0, y, width, Math.min(strip, height), 0, 0, count, 1);
  }
  const data = context.getImageData(0, 0, scratch.width, scratch.height).data;
  const want = [];
  for (let i = 0; i < count; i++) {
    want.push([Math.round(data[i * 4] * 0.6), Math.round(data[i * 4 + 1] * 0.6), Math.round(data[i * 4 + 2] * 0.6)]);
  }
  const style = getComputedStyle(video);
  const box = bar.getBoundingClientRect();
  const picture = video.getBoundingClientRect();
  const scale = style.objectFit === "cover"
    ? Math.max(picture.width / width, picture.height / height)
    : Math.min(picture.width / width, picture.height / height);
  const shown = [width * scale, height * scale];
  const inner = [picture.left + (picture.width - shown[0]) / 2, picture.top + (picture.height - shown[1]) / 2];
  const ratio = box.width > 0 ? bar.width / box.width : 1;
  const span = column ? box.height : box.width;
  const start = Math.max(0, Math.round(column ? inner[1] - box.top : inner[0] - box.left));
  const end = Math.min(span, Math.round(column ? inner[1] + shown[1] - box.top : inner[0] + shown[0] - box.left));
  const canvas = bar.getContext("2d");
  const read = [];
  for (let i = 0; i < count; i++) {
    const point = Math.round((start + (i / 5) * (end - start)) * ratio);
    const x = column ? Math.round(bar.width / 2) : Math.min(bar.width - 1, Math.max(0, point));
    const y = column ? Math.min(bar.height - 1, Math.max(0, point)) : Math.round(bar.height / 2);
    const d = canvas.getImageData(x, y, 1, 1).data;
    read.push([d[0], d[1], d[2]]);
  }
  return { side: side, read: read, want: want };
}, side);

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
await page.route("**/open77-fit.js", (route) => route.abort());
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
const answer = !needsFill ? "none" : loss <= 1 / 3 ? "cover" : "glow";

// Then the same box with the script, at the same points.
await page.unroute("**/open77-fit.js");
await enter();
const withScript = await settle();
ok(!!withScript && !!withScript.stream[0], "no picture: the stream never arrived in " + seconds + "s (with the script)");
const pixels = await wash(bare ? bare.spots : []);
const fit = withScript && withScript.fit;
const agreed = !!fit && fit.mode === answer && Math.abs((fit.loss || 0) - loss) <= 0.005;
ok(agreed, "the script's own reading of the box is not the box: " +
  JSON.stringify({ fit: fit, loss: Math.round(loss * 1000) / 1000, answer: answer }));

let covered = null;
if (needsFill && answer === "cover") {
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
}

let glowing = null;
if (needsFill && answer === "glow") {
  glowing = {
    rect: withScript.element.join("x") === bare.element.join("x"),
    fit: withScript.pictureFit === bare.pictureFit && withScript.pictureFit === "contain",
    attribute: withScript.pictureAttribute === bare.pictureAttribute,
    coverClass: withScript.coverClass,
    layer: withScript.layer && withScript.layer.covers && withScript.layer.under &&
      withScript.layer.fixed === "fixed" && withScript.layer.pointer === "none" &&
      withScript.layer.filter === "none" && withScript.layer.streams === 0 &&
      withScript.layer.bars === withScript.sides.length,
  };
  ok(glowing.rect && glowing.fit && !glowing.coverClass && glowing.attribute,
    "glow: the picture was not left exactly as the client laid it out: " + JSON.stringify(glowing));
  ok(glowing.layer,
    "glow: the layer is not a layer of the whole box under the picture, sharp, one canvas per bar, with no " +
    "second stream: " + JSON.stringify(withScript.layer));
  ok(pixels.length > 0 && pixels.every((spot) => !black(spot)),
    "glow: a bar is still black: " + JSON.stringify(withoutScript.map((spot, i) => [spot, pixels[i]])));
  // And the bars are the picture's OWN edge, stop for stop, against the picture itself.
  const read = (await Promise.all(withScript.sides.map((side) => stops(side)))).filter(Boolean);
  const wrong = [];
  for (const bar of read) {
    bar.read.forEach((colour, i) => {
      const want = bar.want[i];
      if (!colour.every((n, c) => Math.abs(n - want[c]) <= 48)) wrong.push({ side: bar.side, stop: i, washed: colour, picture: want });
    });
  }
  ok(read.length === withScript.sides.length && wrong.length <= 2,
    "glow: the bars are not the picture's own edge colours, stop for stop: " + JSON.stringify(wrong));
  ok(fit && /the picture whole/.test(fit.line || ""),
    "glow: and the television is told so: " + JSON.stringify(fit && fit.line));
}

console.log(JSON.stringify({ box: [width, height], answer: answer, loss: Math.round(loss * 1000) / 1000,
  picture: bare && bare.picture, cover: covered, glow: glowing,
  edgeWithoutScript: withoutScript, withScript: pixels,
  fit: fit && { mode: fit.mode, bars: fit.bars, streams: fit.streams, line: fit.line },
  failures: failures }, null, 1));
console.log((failures.length === 0 ? "ok   " : "FAIL ") + width + "x" + height + ": " + answer +
  " (" + (needsFill ? Math.round(loss * 100) + "% of the picture" : "the box is the picture's own shape") +
  ")" + (failures.length ? " -- " + failures.join(" | ") : ", and every check passed"));

await browser.close();
server.close();
process.exit(failures.length === 0 ? 0 : 1);
