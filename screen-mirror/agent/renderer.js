// open77 headless mirror agent - WebRTC sender core (runs in the hidden window).
//
// Same transport as sender.html: capture the screen with getDisplayMedia
// (auto-approved by the main process), prefer H.264, cap the capture at 60fps,
// stream over WebRTC per viewer. Signaling rides the relay's WebSocket. Adaptive
// bitrate (AIMD) keeps the picture from stalling on a weak link, and ICE restart
// recovers from Wi-Fi drops. View-only: nothing is ever received back.

// ---- Config (injected by main via renderer-config) -------------------------
const CFG = window.__mirrorConfig || {};
const CHANNEL = CFG.channel || "WORK";
const TOKEN = CFG.token || "";
const WS_URL = CFG.relay || "wss://xbuniverse.duckdns.org/screen-mirror/ws";
const ICE_URL = CFG.iceUrl || "https://xbuniverse.duckdns.org/screen-mirror/ice";
const FPS_CAP = 60;
const FPS = Math.max(1, Math.min(FPS_CAP, Number(CFG.fps) || FPS_CAP));

function status(text) {
  if (window.__agent && window.__agent.status) window.__agent.status(text);
}

// ---- WebRTC ---------------------------------------------------------------
let displayStream = null;
let iceServers = [];
const peers = new Map();           // receiverId -> RTCPeerConnection
const pendingReceivers = new Set();

async function fetchIce() {
  try {
    const r = await fetch(ICE_URL);
    iceServers = (await r.json()).iceServers || [];
  } catch { iceServers = []; }
}

function signal(to, payload) {
  if (ws && ws.readyState === 1) ws.send(JSON.stringify({ type: "signal", to, payload }));
}

// H.264 first on the video transceiver: sharp text, hardware encode. Other
// codecs stay as fallbacks so a browser without H.264 still works.
function preferH264(transceiver) {
  if (!transceiver || !transceiver.setCodecPreferences) return;
  const caps = (window.RTCRtpReceiver.getCapabilities && window.RTCRtpReceiver.getCapabilities("video")) || { codecs: [] };
  const codecs = caps.codecs || [];
  const h264 = codecs.filter((c) => c.mimeType.toLowerCase() === "video/h264");
  const rest = codecs.filter((c) => c.mimeType.toLowerCase() !== "video/h264");
  if (h264.length) { try { transceiver.setCodecPreferences([...h264, ...rest]); } catch {} }
}

// ---- Adaptive bitrate ------------------------------------------------------
// Watch the outgoing video and back the bitrate off when the link shows loss,
// rising RTT, or a bandwidth limitation, then ease back up while clean (AIMD).
const adaptiveTimers = new Map();
const adaptiveState = new Map();
function stopAdaptive(pc) {
  const t = adaptiveTimers.get(pc);
  if (t) { clearInterval(t); adaptiveTimers.delete(pc); }
  adaptiveState.delete(pc);
}
function startAdaptive(pc, sender) {
  if (!sender || adaptiveTimers.has(pc)) return;
  const MIN = 250000, MAX = 8000000;
  adaptiveState.set(pc, { cur: 3000000, lastSent: 0, lastLost: 0 });
  const apply = () => {
    const st = adaptiveState.get(pc);
    if (!st) return;
    try {
      const p = sender.getParameters();
      if (!p.encodings || !p.encodings.length) p.encodings = [{}];
      p.encodings[0].maxBitrate = Math.round(st.cur);
      p.degradationPreference = "maintain-resolution"; // keep text sharp
      sender.setParameters(p).catch(() => {});
    } catch {}
  };
  apply();
  const timer = setInterval(async () => {
    const st = adaptiveState.get(pc);
    if (!st) { clearInterval(timer); return; }
    if (pc.connectionState === "closed") { stopAdaptive(pc); return; }
    let lossRate = 0, rttMs = 0, limited = "";
    try {
      const stats = await pc.getStats();
      let sent = 0, lost = 0;
      stats.forEach((r) => {
        if (r.type === "outbound-rtp" && r.kind === "video") {
          sent = r.packetsSent || 0;
          limited = r.qualityLimitationReason || "";
        }
        if (r.type === "remote-inbound-rtp" && r.kind === "video") {
          lost = r.packetsLost || 0;
          rttMs = (r.roundTripTime || 0) * 1000;
        }
      });
      const dSent = sent - st.lastSent, dLost = lost - st.lastLost;
      st.lastSent = sent; st.lastLost = lost;
      if (dSent > 0) lossRate = Math.max(0, dLost) / (dSent + Math.max(0, dLost));
    } catch {}
    const congested = limited === "bandwidth" || lossRate > 0.02 || rttMs > 250;
    if (congested) st.cur = Math.max(MIN, st.cur * 0.8);
    else if (limited === "" && lossRate < 0.005 && rttMs < 120) st.cur = Math.min(MAX, st.cur * 1.1 + 50000);
    apply();
  }, 2000);
  adaptiveTimers.set(pc, timer);
}

function createPeerFor(rid) {
  if (peers.has(rid)) return peers.get(rid);
  const pc = new RTCPeerConnection({ iceServers });
  peers.set(rid, pc);
  const vTrack = displayStream.getVideoTracks()[0];
  let vSender = null;
  if (vTrack) {
    try { vTrack.contentHint = "detail"; } catch {} // sharp text over smooth motion
    const vTr = pc.addTransceiver(vTrack, { direction: "sendonly" });
    preferH264(vTr);
    vSender = vTr.sender;
    try { const p = vSender.getParameters(); p.degradationPreference = "maintain-resolution"; vSender.setParameters(p); } catch {}
  }
  const aTrack = displayStream.getAudioTracks()[0];
  if (aTrack) pc.addTransceiver(aTrack, { direction: "sendonly" });
  pc.onicecandidate = (e) => {
    if (e.candidate) signal(rid, { kind: "ice", candidate: e.candidate.toJSON() });
  };
  // One offer path for first offers and ICE-restart re-offers alike.
  pc.onnegotiationneeded = async () => {
    try {
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);
      signal(rid, { kind: "offer", sdp: { type: pc.localDescription.type, sdp: pc.localDescription.sdp } });
    } catch {}
  };
  let discTimer = null;
  pc.onconnectionstatechange = () => {
    reportState();
    const s = pc.connectionState;
    if (s === "connected") {
      if (discTimer) { clearTimeout(discTimer); discTimer = null; }
      startAdaptive(pc, vSender);
    } else if (s === "disconnected") {
      if (!discTimer) discTimer = setTimeout(() => { discTimer = null; try { pc.restartIce(); } catch {} }, 4000);
    } else if (s === "failed") {
      if (discTimer) { clearTimeout(discTimer); discTimer = null; }
      try { pc.restartIce(); } catch {}
    } else if (s === "closed") {
      if (discTimer) { clearTimeout(discTimer); discTimer = null; }
      stopAdaptive(pc);
    }
  };
  return pc;
}

function reportState() {
  const live = [...peers.values()].filter((p) => p.connectionState === "connected").length;
  status(live > 0
    ? "streaming " + CHANNEL + " to " + live + " viewer(s)"
    : "up on " + CHANNEL + " - waiting for viewers");
}

function flushPending() {
  for (const rid of pendingReceivers) createPeerFor(rid);
  pendingReceivers.clear();
  reportState();
}

// ---- Signaling WebSocket lifecycle ----------------------------------------
let ws = null;
let reconnectTimer = null;
let backoffMs = 1000;
const BACKOFF_MAX = 30000;

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => { reconnectTimer = null; connect(); }, backoffMs);
  backoffMs = Math.min(backoffMs * 2, BACKOFF_MAX);
}

function connect() {
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
  const old = ws; ws = null;
  if (old) { try { old.close(); } catch {} }
  let sock;
  try { sock = new WebSocket(WS_URL); }
  catch { status("relay unreachable - retrying"); scheduleReconnect(); return; }
  ws = sock;

  sock.addEventListener("open", () => {
    if (sock !== ws) return;
    backoffMs = 1000;
    sock.send(JSON.stringify({ type: "join", channel: CHANNEL, role: "sender", token: TOKEN }));
    status("joined " + CHANNEL + " - starting capture");
  });

  sock.addEventListener("message", (e) => {
    if (sock !== ws) return;
    let m; try { m = JSON.parse(e.data); } catch { return; }
    if (m.type === "superseded") {
      status("another sender took " + CHANNEL + " - this agent is standing by");
    } else if (m.type === "auth_failed") {
      status("REJECTED - token is wrong or expired (fix config.json, then Reconnect)");
      scheduleReconnect();
    } else if (m.type === "peer" && m.role === "receiver") {
      if (displayStream) createPeerFor(m.id);
      else pendingReceivers.add(m.id);
    } else if (m.type === "signal") {
      const pc = peers.get(m.from);
      const p = m.payload || {};
      if (!pc) return;
      if (p.kind === "answer") pc.setRemoteDescription(p.sdp).catch(() => {});
      else if (p.kind === "ice") pc.addIceCandidate(p.candidate).catch(() => {});
    } else if (m.type === "peer_left") {
      const pc = peers.get(m.id);
      if (pc) { try { pc.close(); } catch {} peers.delete(m.id); reportState(); }
    }
  });

  sock.addEventListener("close", () => {
    if (sock !== ws) return;
    ws = null;
    status("relay connection lost - retrying");
    scheduleReconnect();
  });
  sock.addEventListener("error", () => {});
}

if (window.__agent && window.__agent.onReconnect) {
  window.__agent.onReconnect(() => { backoffMs = 1000; connect(); });
}

// ---- Capture ---------------------------------------------------------------
async function startCapture() {
  try {
    // 60fps CAP: the screen may refresh faster; the mirror never exceeds 60.
    const stream = await navigator.mediaDevices.getDisplayMedia({
      video: { frameRate: { ideal: FPS, max: FPS } },
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
    });
    displayStream = stream;
    stream.getVideoTracks()[0].addEventListener("ended", () => {
      status("capture stopped by the system - restarting");
      setTimeout(startCapture, 2000);
    });
    flushPending();
    return true;
  } catch (err) {
    status("screen capture refused: " + err.name);
    setTimeout(startCapture, 5000);
    return false;
  }
}

(async () => {
  await fetchIce();
  connect();
  await startCapture();
})();
