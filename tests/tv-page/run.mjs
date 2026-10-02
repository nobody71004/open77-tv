// The television page (open77_media/web/tv.js) in a real Chromium, against a
// stub of the game host: the bridge is shimmed and /op77/* answers the way the
// shipped web host does -- including its malformed "disabled" answer, which is
// what put probe_failed (SyntaxError ... position 33) on every pasted link.
//
// What it pins: a DRM service is named on the screen and never probed; the
// malformed answer is repaired and a website is framed with the decoder off; a
// video file says it needs the decoder; a stream found inside a site is not
// played when the host cannot decode it; the shared browser is framed as it is,
// kept across state updates and closed when the TV leaves it.
//
//   npm i -D playwright && npx playwright install chromium
//   node tests/tv-page/run.mjs [web dir]          (default: open77_media/web)
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
let chromium;
try {
  ({ chromium } = await import("playwright"));
} catch (error) {
  console.error("playwright is not installed: npm i -D playwright && npx playwright install chromium");
  process.exit(2);
}
const webDir = process.argv[2] || path.join(here, "..", "..", "open77_media", "web");
// The web host's 'disabled' answer exactly as SurfaceClient builds it: the detail
// is JsonQuote(...) -- already quoted -- inside another pair of quotes.
const jsonQuote = (s) => JSON.stringify(s);
const buggyDisabled = (reason) => '{"verdict":"disabled","detail":"' + jsonQuote(reason) + '"}';
const probeAnswers = {
  disabledBuggy: buggyDisabled("no decoder tools were staged: ffprobe.exe, ffmpeg.exe missing from \"C:\\Open77\\decoder\""),
  disabledEmpty: buggyDisabled(""),
  nothingGood: JSON.stringify({ verdict: "nothing", detail: "not media" }),
  garbage: "<html>oops</html>",
};
let probeMode = "disabledBuggy";
let foundAnswer = JSON.stringify({ ok: false });
const hits = [];

const server = http.createServer((req, res) => {
  const url = new URL(req.url, "http://x");
  hits.push(url.pathname + url.search);
  if (url.pathname === "/op77/media/probe") {
    res.writeHead(200, { "Content-Type": "application/json" });
    return res.end(probeAnswers[probeMode]);
  }
  if (url.pathname === "/op77/web/frame") {
    res.writeHead(200, { "Content-Type": "application/json" });
    return res.end(JSON.stringify({ ok: true, frameable: true }));
  }
  if (url.pathname === "/op77/media/found") {
    res.writeHead(200, { "Content-Type": "application/json" });
    return res.end(foundAnswer);
  }
  if (url.pathname === "/site") {
    res.writeHead(200, { "Content-Type": "text/html" });
    return res.end("<html><body>a site</body></html>");
  }
  let file = path.join(webDir, url.pathname === "/" ? "tv.html" : url.pathname.slice(1));
  if (!fs.existsSync(file)) { res.writeHead(404); return res.end(); }
  let body = fs.readFileSync(file);
  if (file.endsWith("tv.html")) {
    body = body.toString().replace('<script src="tv.js"></script>',
      '<script>window.__reports=[];window.__handlers={};window.Open77={on:function(n,f){window.__handlers[n]=f},' +
      'emit:function(n,p){window.__reports.push([n,p])},ready:function(){}};</script><script src="tv.js"></script>');
  }
  res.writeHead(200, { "Content-Type": file.endsWith(".html") ? "text/html" : file.endsWith(".js") ? "text/javascript" : "text/css" });
  res.end(body);
});
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const base = "http://127.0.0.1:" + server.address().port;

const browser = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
const page = await browser.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));
await page.goto(base + "/");

let failed = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) failed++; };
const setState = (s) => page.evaluate((s) => { window.__reports = []; window.__handlers["media:state"](s); }, s);
const reports = () => page.evaluate(() => window.__reports.map((r) => r[1].status + " | " + r[1].detail));
const text = (id) => page.evaluate((id) => document.getElementById(id).textContent, id);
const noticeShown = () => page.evaluate(() => !document.getElementById("notice").hidden);
const settle = () => page.waitForTimeout(400);
const base_state = { volume: 75, muted: false, paused: false, label: "Cinema screen, 150 ft", border: "#c8102e", curtain: "open", key: "F5" };

// 1. Netflix, with watch parties on this server.
await setState({ ...base_state, url: "https://www.netflix.com/watch/81234567?trackId=1", parties: true });
await settle();
let r = await reports();
check(r.some((x) => x.startsWith("drm_protected | Netflix https://www.netflix.com/watch/81234567")), "a Netflix title link is reported drm_protected: " + JSON.stringify(r));
check(!hits.some((h) => h.startsWith("/op77/media/probe")), "and never goes to the probe");
check((await text("idle-detail")) === "Netflix cannot play on a television in the game", "the screen says Netflix cannot play: " + await text("idle-detail"));
let n = await text("notice");
check(await noticeShown() && n.includes("DRM-protected") && n.includes("/watch"), "the notice names DRM and /watch: " + n);

// 2. The bare home page (what was pasted on 2026-10-01), no parties on this server.
await setState({ ...base_state, url: "https://www.netflix.com", parties: false });
await settle();
r = await reports();
check(r.some((x) => x.startsWith("drm_protected | Netflix https://www.netflix.com")), "the bare netflix.com is drm_protected too");
n = await text("notice");
check(!n.includes("/watch") && n.includes("YouTube"), "without parties the notice names what does play: " + n);

// 3. A state update for the same link does not re-report.
await setState({ ...base_state, url: "https://www.netflix.com", parties: false, volume: 40 });
await settle();
r = await reports();
check(!r.some((x) => x.startsWith("drm_protected")), "a volume change on the same link is not a new drm_protected line: " + JSON.stringify(r));

// 4. Other services, and a host that only looks like one.
for (const [u, svc] of [["https://www.disneyplus.com/video/abc", "Disney+"], ["https://play.max.com/movie/x", "Max"], ["https://tv.apple.com/us/movie/x", "Apple TV+"]]) {
  await setState({ ...base_state, url: u });
  await settle();
  r = await reports();
  check(r.some((x) => x.startsWith("drm_protected | " + svc)), svc + " is recognised");
}
hits.length = 0;
await setState({ ...base_state, url: base + "/site?netflix.com" });
await settle();
check(hits.some((h) => h.startsWith("/op77/media/probe")), "a link that only mentions netflix.com in its query still goes to the probe");

// 5. A site, with the shipped host's malformed 'disabled' answer: repaired, framed.
probeMode = "disabledBuggy"; hits.length = 0;
await setState({ ...base_state, url: base + "/site" });
await page.waitForTimeout(1200);
r = await reports();
check(r.some((x) => x.startsWith("probe_answer_repaired | disabled: no decoder tools were staged")), "the double-quoted detail is repaired: " + JSON.stringify(r));
check(r.some((x) => x.startsWith("not_stream_site | decoder off:")), "a site with the decoder off is framed (not_stream_site)");
check(!r.some((x) => x.startsWith("probe_failed")), "no probe_failed");
const frameSrc = await page.evaluate(() => { const f = document.querySelector("#embed iframe"); return f ? f.src : null; });
check(frameSrc === base + "/site", "the site's frame is built: " + frameSrc);
check(r.some((x) => x.startsWith("embed_framed")), "and a document arrived in it (embed_framed)");

// 6. A video file with the decoder off: says it needs the decoder, names the reason.
probeMode = "disabledBuggy";
await setState({ ...base_state, url: "https://cdn.example.invalid/film.mp4" });
await settle();
r = await reports();
check(r.some((x) => x.startsWith("transcode_disabled | no decoder tools were staged")), "an .mp4 with the decoder off reports transcode_disabled: " + JSON.stringify(r));
check((await text("idle-detail")) === "this video needs the game host's decoder", "the screen says it needs the decoder");
check((await text("notice")).includes("C:\\Open77\\decoder"), "the notice carries the host's reason, escapes intact: " + await text("notice"));

// 7. An empty detail in the malformed shape.
probeMode = "disabledEmpty";
await setState({ ...base_state, url: "https://cdn.example.invalid/film2.mp4" });
await settle();
r = await reports();
check(r.some((x) => x.startsWith("probe_answer_repaired | disabled: ")) && r.some((x) => x.startsWith("transcode_disabled")), "an empty double-quoted detail is repaired too");

// 8. Well-formed answers are untouched: 'nothing' on a site frames it, no repair line.
probeMode = "nothingGood";
await setState({ ...base_state, url: base + "/site#2" });
await page.waitForTimeout(800);
r = await reports();
check(r.some((x) => x.startsWith("not_stream_site | not media")) && !r.some((x) => x.startsWith("probe_answer_")), "a well-formed nothing frames the site without a repair line");

// 9. An answer that is not JSON at all: reported verbatim, taken as nothing, site framed.
probeMode = "garbage";
await setState({ ...base_state, url: base + "/site#3" });
await page.waitForTimeout(800);
r = await reports();
check(r.some((x) => x.startsWith("probe_answer_unreadable | <html>oops")) && r.some((x) => x.startsWith("not_stream_site")), "an unreadable answer is named and the site still framed");

// 10. With the decoder off, a stream the host found does not replace the framed site.
probeMode = "disabledBuggy";
foundAnswer = JSON.stringify({ ok: true, stream: "https://cdn.example.invalid/x.m3u8", kind: "hls" });
await setState({ ...base_state, url: base + "/site#4" });
await page.waitForTimeout(2600);
r = await reports();
check(r.some((x) => x.startsWith("site_stream_found_decoder_off | hls https://cdn.example.invalid/x.m3u8")), "a found stream is not played with the decoder off: " + JSON.stringify(r));
check(!r.some((x) => x.startsWith("site_stream_found |")), "and the site stays framed");
foundAnswer = JSON.stringify({ ok: false });

// 11. A YouTube link still goes to the embed path.
await setState({ ...base_state, url: "https://www.youtube.com/watch?v=dQw4w9WgXcQ" });
await settle();
r = await reports();
check(r.some((x) => x.includes("youtube")), "YouTube still takes its embed path: " + JSON.stringify(r));

// 12. Netflix after a playing video stops the video.
await page.evaluate(() => { const m = document.getElementById("media"); m.setAttribute("src", "data:video/webm;base64,AAAA"); });
await setState({ ...base_state, url: "https://www.netflix.com/title/80057281", parties: true });
await settle();
const mediaSrc = await page.evaluate(() => document.getElementById("media").getAttribute("src"));
check(mediaSrc === null, "a protected link stops the previous video: src=" + mediaSrc);


// 13. The shared browser: framed as it is, marker stripped, no probe, kept across state updates.
hits.length = 0;
await setState({ ...base_state, url: base + "/site?b=1#open77-shared-browser" });
await settle();
r = await reports();
const bframe = await page.evaluate(() => { const f = document.querySelector("#embed iframe"); return f ? { src: f.src, allow: f.getAttribute("allow") } : null; });
check(bframe && bframe.src === base + "/site?b=1", "the shared browser is framed without its mark: " + JSON.stringify(bframe));
check(bframe && /autoplay/.test(bframe.allow) && /clipboard-write/.test(bframe.allow), "with autoplay and clipboard allowed");
check(r.some((x) => x.startsWith("browser | shared browser")), "reported as browser: " + JSON.stringify(r));
check(!hits.some((h) => h.startsWith("/op77/media/probe") || h.startsWith("/op77/web/frame")), "no probe and no frame check for it");
check((await text("notice")).includes("Press F8"), "the notice says how to use it");
await page.evaluate(() => { window.__frameRef = document.querySelector("#embed iframe"); });
await setState({ ...base_state, url: base + "/site?b=1#open77-shared-browser", volume: 30 });
await settle();
check(await page.evaluate(() => document.querySelector("#embed iframe") === window.__frameRef), "a volume change keeps the same frame (no reconnect)");

// 14. Leaving it for a protected link closes it.
await setState({ ...base_state, url: "https://www.netflix.com/watch/1" , parties: true});
await settle();
check(await page.evaluate(() => document.querySelectorAll("#embed iframe").length) === 0, "a Netflix link after the browser leaves no frame running");

// 15. Leaving it for a link that goes to the probe closes it too.
await setState({ ...base_state, url: base + "/site?b=2#open77-shared-browser" });
await settle();
probeMode = "disabledBuggy";
await setState({ ...base_state, url: "https://cdn.example.invalid/after-browser.mp4" });
await settle();
check(await page.evaluate(() => document.querySelectorAll("#embed iframe").length) === 0, "an .mp4 after the browser leaves no hidden browser frame");

check(errors.length === 0, "no page errors: " + JSON.stringify(errors));
await browser.close();
server.close();
console.log(failed === 0 ? "ALL PASSED" : failed + " FAILED");
process.exit(failed === 0 ? 0 : 1);
