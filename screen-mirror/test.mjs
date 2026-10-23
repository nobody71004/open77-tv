// End-to-end proof of the screen-mirror loop over WebRTC: sender -> (signaling
// relay) -> receiver, with the picture (H.264) and sound (Opus) flowing over a
// real peer connection between two Chromium pages. The relay only carries the
// SDP/ICE handshake; no media passes through it. Runs on one machine, so the
// peers connect over host candidates (no TURN needed to prove the pipeline).
//
//   node screen-mirror/test.mjs
//
// Exit 0 = picture and sound flowed, and the capture stayed within the 60fps
// cap. Non-zero = something did not.

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));
const PORT = 8901;
const BASE = `http://127.0.0.1:${PORT}`;
const CHANNEL = "test";

function log(...a) { console.log("[test]", ...a); }
function fail(msg) { console.error("[test] FAIL:", msg); process.exitCode = 1; }

async function waitForHealth(timeoutMs = 8000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    try {
      const r = await fetch(`${BASE}/health`);
      if (r.ok) return true;
    } catch {}
    await new Promise((r) => setTimeout(r, 150));
  }
  return false;
}

async function main() {
  // Start the signaling relay. STUN/TURN are turned OFF so the two local peers
  // connect over host candidates -- this proves the WebRTC pipeline itself,
  // independent of any network or the VPS's TURN server.
  const relay = spawn(process.execPath, [join(HERE, "relay.mjs")], {
    env: { ...process.env, PORT: String(PORT), MIRROR_TOKEN: "", STUN_URL: "", TURN_URL: "" },
    stdio: ["ignore", "pipe", "pipe"],
  });
  relay.stdout.on("data", (d) => process.stdout.write("  relay: " + d));
  relay.stderr.on("data", (d) => process.stderr.write("  relay! " + d));

  const cleanup = () => { try { relay.kill(); } catch {} };
  process.on("exit", cleanup);

  if (!(await waitForHealth())) {
    fail("relay did not come up on " + BASE);
    cleanup();
    return;
  }
  log("relay (signaling) up on", BASE);

  const { chromium } = await import("playwright");
  const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
  const sender = await browser.newPage();
  const receiver = await browser.newPage();

  const errors = [];
  for (const [name, page] of [["sender", sender], ["receiver", receiver]]) {
    page.on("pageerror", (e) => errors.push(name + ": " + e.message));
    page.on("console", (m) => { if (m.type() === "error") errors.push(name + " console: " + m.text()); });
  }

  // Receiver first, then the sender (test-pattern source). The sender is asked
  // for 120fps to prove the capture is clamped to the 60 cap.
  await receiver.goto(`${BASE}/receiver?channel=${CHANNEL}`);
  await sender.goto(`${BASE}/?channel=${CHANNEL}&source=canvas&fps=120`);
  await sender.evaluate(() => window.__open77Sender && window.__open77Sender.resumeAudio());
  await receiver.evaluate(() => window.__open77Mirror.resumeAudio && window.__open77Mirror.resumeAudio());

  // 1. Wait for video to actually decode on the receiver (framesDecoded > 0).
  let stats = null;
  const t0 = Date.now();
  while (Date.now() - t0 < 15000) {
    stats = await receiver.evaluate(() => window.__open77Mirror.getStats());
    if (stats && stats.framesDecoded > 0) break;
    await sender.evaluate(() => window.__open77Sender && window.__open77Sender.resumeAudio());
    await receiver.evaluate(() => window.__open77Mirror.resumeAudio && window.__open77Mirror.resumeAudio());
    await new Promise((r) => setTimeout(r, 300));
  }
  log("receiver stats after wait:", JSON.stringify(stats));
  if (!stats || stats.framesDecoded < 1) {
    fail("no video decoded over WebRTC (framesDecoded " + (stats && stats.framesDecoded) + ")");
  } else {
    log("video decoded:", stats.framesDecoded, "frames, codec", stats.codec || "(none reported)");
  }

  // 2. The picture is live (advancing), not a single stalled frame.
  const s1 = await receiver.evaluate(() => window.__open77Mirror.getStats());
  await new Promise((r) => setTimeout(r, 1200));
  const s2 = await receiver.evaluate(() => window.__open77Mirror.getStats());
  if (s2.framesDecoded <= s1.framesDecoded) {
    fail("video not advancing (" + s1.framesDecoded + " -> " + s2.framesDecoded + ")");
  } else {
    log("video advancing:", s1.framesDecoded, "->", s2.framesDecoded);
  }

  // 3. Sound flows alongside the picture.
  if (!s2.audioPackets || s2.audioPackets < 1) {
    fail("no audio over WebRTC (audioPackets " + s2.audioPackets + ")");
  } else {
    log("audio flowing:", s2.audioPackets, "packets");
  }

  // 4. The 60fps CAP holds even though 120 was requested on the URL.
  const fps = await sender.evaluate(() => window.__open77Sender.fps());
  const track = await sender.evaluate(() => window.__open77Sender.trackSettings());
  log("sender fps setting:", fps, " track:", JSON.stringify(track));
  if (fps > 60) {
    fail("requested frame rate was not capped at 60 (fps=" + fps + ")");
  } else {
    log("fps capped at 60:", fps);
  }
  if (track && typeof track.frameRate === "number" && track.frameRate > 60.5) {
    fail("capture frame rate above the 60 cap (" + track.frameRate + ")");
  } else if (track && typeof track.frameRate === "number") {
    log("capture frameRate within cap:", track.frameRate);
  }

  // 5. Codec (informational). Playwright's bundled Chromium may lack the
  // proprietary H.264 codec, so this reports the negotiated codec rather than
  // asserting H.264 -- the sender PREFERS H.264, which production Chrome/CEF use.
  log("negotiated video codec:", s2.codec || "(none reported)");

  if (errors.length) {
    log("page errors:", JSON.stringify(errors, null, 2));
  }

  await browser.close();
  cleanup();

  if (process.exitCode) {
    console.error("\n[test] RESULT: FAIL");
  } else {
    console.log("\n[test] RESULT: PASS - WebRTC carried a live picture and sound, within the 60fps cap");
  }
}

main().catch((e) => { console.error("[test] threw:", e); process.exitCode = 1; });
