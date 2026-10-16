// Diagnose the VPS TURN path: gather ICE candidates with policy "all" and report
// their kinds. host = local; srflx = STUN reached the VPS (so UDP 3478 is open
// externally); relay = TURN allocated (the goal). This distinguishes an upstream
// firewall block (no srflx) from a TURN auth/alloc problem (srflx but no relay).
import { chromium } from "playwright";

const ICE_URL = "https://xbuniverse.duckdns.org/screen-mirror/ice";
const ice = await (await fetch(ICE_URL)).json();
console.log("ice urls:", (ice.iceServers || []).map((s) => s.urls).flat().join(", "));

const browser = await chromium.launch();
const page = await browser.newPage();
const probe = (iceServers, policy) => page.evaluate(async ({ iceServers, policy }) => {
  const pc = new RTCPeerConnection({ iceServers, iceTransportPolicy: policy });
  pc.createDataChannel("probe");
  const cand = [];
  pc.onicecandidate = (e) => { if (e.candidate) cand.push(e.candidate.candidate); };
  await pc.setLocalDescription(await pc.createOffer());
  await new Promise((res) => setTimeout(res, 6000));
  const kind = (c) => (c.match(/ typ ([a-z]+)/) || [])[1];
  return {
    total: cand.length,
    host: cand.filter((c) => kind(c) === "host").length,
    srflx: cand.filter((c) => kind(c) === "srflx").length,
    relay: cand.filter((c) => kind(c) === "relay").length,
  };
}, { iceServers, policy });

// Probe 1 (policy "all"): the diagnostic view. srflx > 0 means UDP 3478 is
// reachable from outside; relay == 0 alongside srflx > 0 isolates the problem
// to TURN auth/alloc/realm rather than an upstream firewall.
const r = await probe(ice.iceServers, "all");
console.log("ICE (policy all):", JSON.stringify(r));

// Probe 2 (policy "relay"): the forced-relay proof. Every candidate here MUST
// be a relay candidate through the VPS TURN; none means TURN is not usable.
const f = await probe(ice.iceServers, "relay");
console.log("ICE (policy relay, forced):", JSON.stringify(f));
await browser.close();

if (f.relay > 0 && r.relay > 0) {
  console.log("VERDICT: TURN RELAY OK - relay candidates were allocated through the VPS TURN");
} else if (r.srflx === 0) {
  console.log("VERDICT: TURN relay allocation FAILED - no srflx either; UDP 3478 blocked upstream");
  process.exitCode = 1;
} else {
  console.log("VERDICT: TURN relay allocation FAILED (auth/alloc/realm)");
  process.exitCode = 1;
}
