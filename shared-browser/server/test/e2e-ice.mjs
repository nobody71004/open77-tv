// Which ICE candidates the shared browser offers, which pair a Chromium picks,
// and whether the picture also arrives over TCP alone (UDP candidates dropped on
// the client side, as if UDP were blocked at a player's PC). Run ON the VPS in
// Playwright's image. The link (viewer password) is read from /secret and never
// printed; addresses in the output are reduced to public/private/mdns.
import fs from "node:fs";
import { chromium } from "playwright";
const url = fs.readFileSync("/secret", "utf8").trim();
const mode = process.argv[2] === "tcp" ? "tcp" : "all";
// "frame": the client page inside an iframe of a page on 127.0.0.1, the way a
// television frames it (a local page, the shared browser cross-origin in it).
const framed = process.argv[3] === "frame";

const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
await page.addInitScript((mode) => {
  const Native = window.RTCPeerConnection;
  const ice = window.__ice = { remote: [], local: [], states: [], dropped: 0, t0: performance.now() };
  const keep = (line) => mode !== "tcp" || / tcp /i.test(line);
  const filterSdp = (sdp) => mode !== "tcp" ? sdp :
    sdp.split("\r\n").filter((l) => !(l.startsWith("a=candidate:") && !/ tcp /i.test(l))).join("\r\n");
  class Watched extends Native {
    constructor(...args) {
      super(...args);
      ice.pc = this;
      this.addEventListener("iceconnectionstatechange", () => ice.states.push(this.iceConnectionState + "@" + Math.round(performance.now() - ice.t0)));
      this.addEventListener("icecandidate", (e) => { if (e.candidate && e.candidate.candidate) ice.local.push(e.candidate.candidate); });
    }
    addIceCandidate(c, ...rest) {
      const line = c && typeof c === "object" ? String(c.candidate || "") : "";
      if (line) ice.remote.push(line);
      if (line && !keep(line)) { ice.dropped++; return Promise.resolve(); }
      return super.addIceCandidate(c, ...rest);
    }
    setRemoteDescription(d) {
      if (d && d.sdp) {
        d.sdp.split("\r\n").filter((l) => l.startsWith("a=candidate:")).forEach((l) => ice.remote.push(l.slice(2)));
        d = { type: d.type, sdp: filterSdp(d.sdp) };
      }
      return super.setRemoteDescription(d);
    }
  }
  window.RTCPeerConnection = Watched;
}, mode);
let target = page;
if (framed) {
  const http = await import("node:http");
  const server = http.createServer((req, res) => {
    res.writeHead(200, { "content-type": "text/html" });
    res.end('<!doctype html><body style="margin:0;background:#000"><iframe id="tv" style="border:0;width:1280px;height:720px" ' +
      'allow="autoplay; fullscreen; clipboard-read; clipboard-write"></iframe></body>');
  }).listen(0, "127.0.0.1");
  await new Promise((r) => server.once("listening", r));
  await page.goto("http://127.0.0.1:" + server.address().port + "/", { waitUntil: "load" });
  await page.evaluate((u) => { document.getElementById("tv").src = u; }, url);
  for (let i = 0; i < 50 && page.frames().length < 2; i++) await page.waitForTimeout(100);
  target = page.frames().find((f) => f !== page.mainFrame());
  await target.waitForLoadState("load");
} else {
  await page.goto(url, { waitUntil: "load" });
}

let sample = null;
for (let i = 0; i < 45; i++) {
  await page.waitForTimeout(1000);
  sample = await target.evaluate(() => {
    const v = document.querySelector("video");
    return v ? { readyState: v.readyState, t: v.currentTime, w: v.videoWidth, h: v.videoHeight } : null;
  });
  if (sample && sample.readyState >= 2 && sample.w > 0) break;
}
await page.waitForTimeout(3000);
const result = await target.evaluate(async () => {
  const ice = window.__ice;
  const shape = (line) => {
    const f = String(line).replace(/^candidate:/, "").split(" ");
    const addr = f[4] || "", type = (/ typ (\w+)/.exec(line) || [])[1], tcptype = (/ tcptype (\w+)/.exec(line) || [])[1];
    const where = /\.local$/.test(addr) ? "mdns" : /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.|127\.|::1|fe80)/.test(addr) ? "private" : "public";
    return [String(f[2] || "").toLowerCase(), type, tcptype, where, f[5]].filter(Boolean).join("/");
  };
  let pair = null;
  const pairs = [];
  if (ice.pc) {
    const stats = await ice.pc.getStats();
    const byId = {};
    stats.forEach((s) => { byId[s.id] = s; });
    stats.forEach((s) => {
      if (s.type !== "candidate-pair") return;
      const l = byId[s.localCandidateId] || {}, r = byId[s.remoteCandidateId] || {};
      pairs.push([l.protocol, l.candidateType].join("/") + ">" + [r.protocol, r.candidateType, r.port].join("/") + ":" + s.state +
        " req=" + (s.requestsSent || 0) + " resp=" + (s.responsesReceived || 0));
    });
    stats.forEach((s) => {
      if (s.type === "transport" && s.selectedCandidatePairId && byId[s.selectedCandidatePairId]) {
        const p = byId[s.selectedCandidatePairId];
        const l = byId[p.localCandidateId] || {}, r = byId[p.remoteCandidateId] || {};
        pair = { state: p.state, local: [l.protocol, l.candidateType].join("/"), remote: [r.protocol, r.candidateType, r.port].join("/"), rx: p.bytesReceived };
      }
    });
  }
  const v = document.querySelector("video");
  return { remote: [...new Set(ice.remote.map(shape))], local: [...new Set(ice.local.map(shape))], dropped: ice.dropped,
    states: ice.states, pair, pairs, video: v ? { readyState: v.readyState, t: v.currentTime, w: v.videoWidth, h: v.videoHeight } : null };
});
result.mode = mode + (framed ? "+frame" : "");
result.advancing = !!(sample && result.video && result.video.t > sample.t);
console.log(JSON.stringify(result));
await browser.close();
process.exit(0);
