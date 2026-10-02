// open77-ice.js against two real RTCPeerConnections in headless Chromium: the
// client page framed cross-origin by a "television" page that collects what the
// script posts. Scenarios: a connection that works, one that never connects (the
// retry over TCP, then giving up, with the login kept although the page took it
// off the address bar, as neko's client does), and ice=tcp from the start (UDP
// candidates left out). No address may appear in any message.
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
let chromium;
try {
  ({ chromium } = await import("playwright"));
} catch (error) {
  console.error("playwright is not installed: npm i -D playwright && npx playwright install chromium");
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const SCRIPT = "open77-ice.js";
// usage: node <this file> [path to the script]   (default: ../build/<script>)
const script = fs.readFileSync(process.argv[2] || path.join(here, "..", "build", SCRIPT), "utf8");

const clientHtml = `<!doctype html><head><script>window.__NativePC = window.RTCPeerConnection; window.__open77IceConnectWithinMs = 2500;</script>
<script src="open77-ice.js"></script><script src="app.js"></script></head><body>client</body>`;
const appJs = `
(function () {
  const q = new URL(location.href).searchParams;
  const scenario = q.get("scenario");
  // As neko's client does: the login is read, then taken off the address bar.
  const u = new URL(location.href); u.searchParams.delete("usr"); u.searchParams.delete("pwd");
  history.replaceState(null, "", u.toString());
  const client = new RTCPeerConnection({ iceServers: [] });
  const server = new window.__NativePC({ iceServers: [] });
  window.__peers = { client, server };
  server.createDataChannel("neko");
  if (scenario === "ok") {
    server.onicecandidate = (e) => { if (e.candidate) client.addIceCandidate(e.candidate).catch(() => {}); };
    client.onicecandidate = (e) => { if (e.candidate) server.addIceCandidate(e.candidate).catch(() => {}); };
  }
  server.createOffer().then((offer) => server.setLocalDescription(offer))
    .then(() => client.setRemoteDescription(server.localDescription))
    .then(() => client.createAnswer()).then((answer) => client.setLocalDescription(answer))
    .then(() => server.setRemoteDescription(client.localDescription))
    .catch((error) => console.log("negotiation: " + error));
})();`;

const clientServer = http.createServer((req, res) => {
  const path = new URL(req.url, "http://x").pathname;
  if (path === "/open77-ice.js") { res.writeHead(200, { "content-type": "text/javascript" }); return res.end(script); }
  if (path === "/app.js") { res.writeHead(200, { "content-type": "text/javascript" }); return res.end(appJs); }
  res.writeHead(200, { "content-type": "text/html", "cache-control": "no-store" }); res.end(clientHtml);
}).listen(0, "127.0.0.1");
await new Promise((r) => clientServer.once("listening", r));
const clientBase = "http://127.0.0.1:" + clientServer.address().port + "/";

const tvServer = http.createServer((req, res) => {
  res.writeHead(200, { "content-type": "text/html" });
  res.end(`<!doctype html><body><iframe id="tv" allow="autoplay"></iframe><script>
    window.__msgs = [];
    addEventListener("message", (e) => { const d = e.data; if (d && d.open77SharedBrowser === 1) __msgs.push(d.kind + " [" + d.mode + "] " + d.text); });
  </script></body>`);
}).listen(0, "localhost");
await new Promise((r) => tvServer.once("listening", r));
const tvUrl = "http://localhost:" + tvServer.address().port + "/";

const browser = await chromium.launch();
let failures = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) failures++; };
const noAddress = (msgs) => !msgs.some((m) => /\b\d{1,3}(\.\d{1,3}){3}\b|\.local\b|[0-9a-f]{8}-[0-9a-f]{4}/i.test(m));

async function run(query, waitMs) {
  const page = await browser.newPage();
  await page.goto(tvUrl);
  await page.evaluate((src) => { document.getElementById("tv").src = src; }, clientBase + "?" + query);
  await page.waitForTimeout(waitMs);
  const msgs = await page.evaluate(() => __msgs);
  const frame = page.frames().find((f) => f !== page.mainFrame());
  const frameUrl = frame ? new URL(frame.url()) : null;
  await page.close();
  return { msgs, frameUrl };
}

// 1. A stream that connects.
let r = await run("usr=open77&pwd=secretpw&embed=1&scenario=ok", 4000);
console.log("   " + r.msgs.join("\n   "));
check(r.msgs.some((m) => /^start \[auto\] open77-ice 1/.test(m)), "says the connection started");
check(r.msgs.some((m) => /^state \[auto\] (checking|connected)/.test(m)), "reports the ICE states");
check(r.msgs.some((m) => /^connected \[auto\] gathered .*udp\/host.*; offered .*udp\/host.*; pairs .*succeeded sent=\d+ answered=[1-9].*; using udp\/host>udp\/host/.test(m)),
  "the summary names what was gathered, offered, tried and used");
check(!r.msgs.some((m) => /^(retry_tcp|gave_up)/.test(m)), "and does not retry");
check(noAddress(r.msgs), "no address in any message");
check(!r.msgs.some((m) => /secretpw/.test(m)), "and not the password");

// 2. A stream that never connects: one retry over TCP, the login kept, then given up.
r = await run("usr=open77&pwd=secretpw&embed=1&scenario=silent", 7500);
console.log("   " + r.msgs.join("\n   "));
check(r.msgs.some((m) => /^retry_tcp \[auto\] not connected after 2500 ms/.test(m)), "auto: not connected in time, so it retries over TCP");
check(r.frameUrl && r.frameUrl.searchParams.get("ice") === "tcp", "the page reopened with ice=tcp");
check(r.frameUrl && r.frameUrl.searchParams.get("scenario") === "silent" && r.frameUrl.searchParams.get("embed") === "1",
  "with the rest of the link");
check(r.msgs.some((m) => /^start \[tcp\] .*UDP candidates are left out/.test(m)), "the second page runs in tcp mode");
check(r.msgs.some((m) => /^gave_up \[tcp\] not connected after 2500 ms .* over TCP too/.test(m)), "and gives up once, saying so");
check(r.msgs.filter((m) => /^retry_tcp/.test(m)).length === 1, "exactly one retry");
check(noAddress(r.msgs), "no address in any message");

// The login survives the retry although the page took it off the address bar.
const page = await browser.newPage();
await page.goto(tvUrl);
await page.evaluate((src) => { document.getElementById("tv").src = src; }, clientBase + "?usr=open77&pwd=secretpw&scenario=silent");
let reopened = null;
const seen = [];
page.on("framenavigated", (f) => {
  if (f === page.mainFrame()) return;
  seen.push(f.url().replace(/pwd=[^&]*/, "pwd=*"));
  if (!reopened && /ice=tcp/.test(f.url())) reopened = new URL(f.url());
});
await page.waitForTimeout(5000);
console.log("   frame navigations: " + seen.join("  |  "));
check(reopened && reopened.searchParams.get("usr") === "open77" && reopened.searchParams.get("pwd") === "secretpw",
  "the reopened link still carries the login the television gave");
await page.close();

// 3. ice=tcp from the start: UDP candidates are left out (a loopback pair has only UDP ones).
r = await run("usr=open77&pwd=secretpw&ice=tcp&scenario=ok", 4000);
console.log("   " + r.msgs.join("\n   "));
check(r.msgs.some((m) => /^gave_up \[tcp\] .*offered none/.test(m)), "tcp mode: the server's UDP candidates are not used");
check(!r.msgs.some((m) => /^connected/.test(m)), "so nothing connects over UDP");

await browser.close();
clientServer.close(); tvServer.close();
console.log(failures === 0 ? "ALL PASSED" : failures + " FAILED");
process.exit(failures === 0 ? 0 : 1);
