// open77-ice.js -- put into the shared browser's client page (neko's index.html)
// by the Open77 image, ahead of the page's own scripts.
//
// The picture on a television is this page's WebRTC stream. When it never
// connects, the television stays black and nothing says why, so this script
// watches the connection the page makes and tells the television framing it
// (window.parent, open77_media's tv.js, which logs it as `browser_ice`):
//
//   * which kinds of candidates this PC gathered, and which the server offered;
//   * every pair that was tried, with the checks sent and the answers received;
//   * which pair carried the picture, or that none did.
//
// Kinds, ports of the server's own candidates and counts only: no address of
// this PC is ever put in a message.
//
// If the stream has not connected within CONNECT_WITHIN_MS (or failed sooner),
// the page is opened once more with `ice=tcp`: only the server's TCP candidate
// is used then, on the same port, which a firewall that drops UDP answers still
// lets through. A second failure is reported and left alone.
(function () {
  "use strict";
  var Native = window.RTCPeerConnection;
  if (typeof Native !== "function") return;

  // Overridable by a test page only (nothing in the client sets it).
  var CONNECT_WITHIN_MS = typeof window.__open77IceConnectWithinMs === "number" ? window.__open77IceConnectWithinMs : 20000;
  // Read now: the client takes `usr` and `pwd` off the address bar once it has
  // read them, and the retry needs the link the television gave.
  var original = location.href;
  var mode = new URL(original).searchParams.get("ice") === "tcp" ? "tcp" : "auto";
  var started = Date.now();
  var settled = false;

  function post(kind, text) {
    try {
      if (window.parent && window.parent !== window) {
        window.parent.postMessage({ open77SharedBrowser: 1, kind: String(kind), mode: mode,
          text: String(text || "").slice(0, 600) }, "*");
      }
    } catch (error) { /* telling the television is best effort */ }
  }

  function isTcpCandidate(line) { return / tcp /i.test(String(line || "")); }
  function keepCandidate(line) { return mode !== "tcp" || isTcpCandidate(line); }
  function filterSdp(sdp) {
    if (mode !== "tcp" || typeof sdp !== "string") return sdp;
    return sdp.split("\r\n").filter(function (line) {
      return line.indexOf("a=candidate:") !== 0 || isTcpCandidate(line);
    }).join("\r\n");
  }

  function kindOf(candidate) {
    return [candidate.protocol, candidate.candidateType, candidate.tcpType].filter(Boolean).join("/");
  }

  // One line: what was gathered, what was offered, every pair and the one used.
  function summary(pc) {
    return pc.getStats().then(function (stats) {
      var byId = {};
      var local = {};
      var remote = {};
      var pairs = [];
      var chosen = "none";
      stats.forEach(function (s) { byId[s.id] = s; });
      stats.forEach(function (s) {
        if (s.type === "local-candidate") local[kindOf(s)] = (local[kindOf(s)] || 0) + 1;
        if (s.type === "remote-candidate") remote[kindOf(s) + ":" + s.port] = true;
        if (s.type === "candidate-pair") {
          var l = byId[s.localCandidateId] || {};
          var r = byId[s.remoteCandidateId] || {};
          pairs.push(kindOf(l) + ">" + kindOf(r) + ":" + r.port + " " + s.state +
            " sent=" + (s.requestsSent || 0) + " answered=" + (s.responsesReceived || 0));
        }
        if (s.type === "transport" && s.selectedCandidatePairId && byId[s.selectedCandidatePairId]) {
          var p = byId[s.selectedCandidatePairId];
          chosen = kindOf(byId[p.localCandidateId] || {}) + ">" + kindOf(byId[p.remoteCandidateId] || {});
        }
      });
      var count = function (map) {
        var out = [];
        Object.keys(map).forEach(function (k) { out.push(map[k] === true ? k : k + "x" + map[k]); });
        return out.join(",") || "none";
      };
      return "gathered " + count(local) + "; offered " + count(remote) + "; pairs " +
        (pairs.join(" | ") || "none") + "; using " + chosen;
    }).catch(function (error) { return "no stats: " + error; });
  }

  function retryOverTcp(pc, why) {
    if (settled) return;
    settled = true;
    summary(pc).then(function (text) {
      if (mode === "tcp") {
        post("gave_up", why + " over TCP too; " + text);
        return;
      }
      post("retry_tcp", why + "; " + text);
      try {
        var next = new URL(original);
        next.searchParams.set("ice", "tcp");
        setTimeout(function () { location.replace(next.toString()); }, 300);
      } catch (error) {
        post("gave_up", "could not reopen over TCP: " + error);
      }
    });
  }

  function Watched(configuration) {
    var pc = configuration === undefined ? new Native() : new Native(configuration);
    var born = Date.now();
    post("start", "open77-ice 1: peer connection " + (born - started) + " ms after load" +
      (mode === "tcp" ? "; UDP candidates are left out" : ""));
    pc.addEventListener("iceconnectionstatechange", function () {
      var state = pc.iceConnectionState;
      post("state", state + " after " + (Date.now() - born) + " ms");
      if ((state === "connected" || state === "completed") && !settled) {
        settled = true;
        summary(pc).then(function (text) { post("connected", text); });
      } else if (state === "failed") {
        retryOverTcp(pc, "failed after " + (Date.now() - born) + " ms");
      }
    });
    setTimeout(function () {
      var state = pc.iceConnectionState;
      if (state !== "connected" && state !== "completed" && state !== "closed") {
        retryOverTcp(pc, "not connected after " + CONNECT_WITHIN_MS + " ms (" + state + ")");
      }
    }, CONNECT_WITHIN_MS);

    var addIceCandidate = pc.addIceCandidate;
    pc.addIceCandidate = function (candidate) {
      var line = candidate && typeof candidate === "object" ? candidate.candidate : "";
      if (line && !keepCandidate(line)) return Promise.resolve();
      return addIceCandidate.apply(pc, arguments);
    };
    if (mode === "tcp") {
      // This PC's own UDP candidates are not offered either: the server would
      // otherwise check them and could be answered over UDP after all.
      var onCandidate = null;
      Object.defineProperty(pc, "onicecandidate", {
        configurable: true,
        get: function () { return onCandidate; },
        set: function (handler) { onCandidate = typeof handler === "function" ? handler : null; },
      });
      var addEventListener = pc.addEventListener;
      var wrapped = [];
      var passes = function (event) { return !event.candidate || isTcpCandidate(event.candidate.candidate); };
      addEventListener.call(pc, "icecandidate", function (event) {
        if (onCandidate && passes(event)) onCandidate.call(pc, event);
      });
      pc.addEventListener = function (type, listener, options) {
        if (type !== "icecandidate" || typeof listener !== "function") return addEventListener.apply(pc, arguments);
        var filtered = function (event) { if (passes(event)) listener.call(pc, event); };
        wrapped.push([listener, filtered]);
        return addEventListener.call(pc, type, filtered, options);
      };
      var removeEventListener = pc.removeEventListener;
      pc.removeEventListener = function (type, listener, options) {
        for (var i = 0; type === "icecandidate" && i < wrapped.length; i++) {
          if (wrapped[i][0] === listener) {
            var filtered = wrapped[i][1];
            wrapped.splice(i, 1);
            return removeEventListener.call(pc, type, filtered, options);
          }
        }
        return removeEventListener.apply(pc, arguments);
      };
    }
    var setRemoteDescription = pc.setRemoteDescription;
    pc.setRemoteDescription = function (description) {
      var args = Array.prototype.slice.call(arguments);
      if (mode === "tcp" && description && typeof description.sdp === "string") {
        args[0] = { type: description.type, sdp: filterSdp(description.sdp) };
      }
      return setRemoteDescription.apply(pc, args);
    };
    return pc;
  }
  Watched.prototype = Native.prototype;
  Object.keys(Native).forEach(function (key) { Watched[key] = Native[key]; });
  if (typeof Native.generateCertificate === "function") Watched.generateCertificate = Native.generateCertificate.bind(Native);
  window.RTCPeerConnection = Watched;
  if (window.webkitRTCPeerConnection) window.webkitRTCPeerConnection = Watched;
})();
