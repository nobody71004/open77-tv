// opx_watchparty -- web/panel.js: the /watch remote.
//
// Owns no state: `watch:state` from the client says what to draw, and every
// button sends `watch:action`. Between states the clock moves on its own, so
// the time and the bar run smoothly while the film plays.
(function () {
  "use strict";

  var shim = { on: function () {}, emit: function () {}, ready: function () {} };
  var bridge = (typeof Open77 !== "undefined" && Open77) ? Open77 : shim;

  var $ = function (id) { return document.getElementById(id); };
  var state = null;
  var stateAt = 0;
  var lastToast = "";
  var toastTimer = null;

  function emit(action, extra) {
    var payload = { action: action };
    if (extra) for (var k in extra) payload[k] = extra[k];
    bridge.emit("watch:action", payload);
  }

  function fmt(ms) {
    if (!(ms >= 0)) ms = 0;
    var t = Math.floor(ms / 1000), h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), s = t % 60;
    var two = function (n) { return (n < 10 ? "0" : "") + n; };
    return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s);
  }

  function parseTime(text) {
    text = String(text || "").trim();
    var m;
    if ((m = /^(\d+):(\d\d):(\d\d)$/.exec(text))) return ((+m[1]) * 3600 + (+m[2]) * 60 + (+m[3])) * 1000;
    if ((m = /^(\d+):(\d\d)$/.exec(text))) return ((+m[1]) * 60 + (+m[2])) * 1000;
    if ((m = /^(\d+)$/.exec(text))) return (+m[1]) * 1000;
    return null;
  }

  function livePosition(p) {
    var ms = p.positionMs || 0;
    if (p.playing) ms += Date.now() - stateAt;
    if (p.durationMs && ms > p.durationMs) ms = p.durationMs;
    return ms;
  }

  function tick() {
    if (!state || !state.party) return;
    var p = state.party;
    var ms = livePosition(p);
    $("time").textContent = fmt(ms) + (p.durationMs ? " / " + fmt(p.durationMs) : "");
    $("fill").style.width = p.durationMs ? Math.min(100, ms / p.durationMs * 100).toFixed(2) + "%" : "0%";
  }

  function showToast(result) {
    if (!result || !result.text) return;
    var key = result.text + "|" + result.at;
    if (key === lastToast) return;
    lastToast = key;
    var t = $("toast");
    t.textContent = result.text;
    t.className = "toast" + (result.ok ? "" : " bad");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.className = "toast hidden"; }, 8000);
  }

  function render() {
    var root = $("panel");
    root.className = "panel" + (state && state.open ? " open" : "");
    if (!state) return;
    var p = state.party;
    $("in-party").classList.toggle("hidden", !p);
    $("out-party").classList.toggle("hidden", !!p);
    if (p) {
      $("title").textContent = p.title || ("Netflix title " + p.netflixId);
      var meta = $("meta");
      meta.textContent = (p.playing ? "▶ playing" : "❚❚ paused") +
        (p.hostName ? " · host " + p.hostName : "") + (p.lastBy ? " · last moved by " + p.lastBy : "");
      meta.className = "meta" + (p.playing ? " playing" : "");
      $("toggle").innerHTML = p.playing ? "&#x275A;&#x275A;" : "&#x25B6;";
      $("sync").textContent = p.inSync + " of " + p.browsers + " browser" + (p.browsers === 1 ? "" : "s") +
        " in step · " + p.members + " in party";
      $("code").textContent = p.code;
      $("stop").classList.toggle("hidden", !state.isHost);
      tick();
    } else {
      var n = state.nearby;
      $("nearby").classList.toggle("hidden", !n);
      if (n) {
        $("nearby-title").textContent = n.title || ("Netflix title " + n.netflixId);
        $("nearby-meta").textContent = (n.playing ? "playing" : "paused") + " · " + fmt(n.positionMs) +
          " · party " + n.code + " · " + n.members + " in it";
      }
      var s = state.screen;
      $("screen-line").textContent = s
        ? (s.inReach ? "It goes on the TV in front of you (" + s.label + ", " + s.distance + " m)."
                     : "Nearest TV: " + s.label + ", " + s.distance + " m away -- walk up to it to put the party on its screen.")
        : "No TV nearby: the party runs without a screen (spawn one with the TV remote).";
    }
    showToast(state.result);
  }

  // Buttons
  $("close").addEventListener("click", function () { emit("close"); });
  $("toggle").addEventListener("click", function () { emit("toggle"); });
  Array.prototype.forEach.call(document.querySelectorAll("[data-nudge]"), function (b) {
    b.addEventListener("click", function () { emit("nudge", { deltaMs: +b.getAttribute("data-nudge") }); });
  });
  $("bar").addEventListener("click", function (e) {
    if (!state || !state.party || !state.party.durationMs) return;
    var r = $("bar").getBoundingClientRect();
    var f = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
    emit("seek", { positionMs: Math.round(f * state.party.durationMs) });
  });
  function seekTyped() {
    var ms = parseTime($("seek-at").value);
    if (ms === null) { showToast({ ok: false, text: "write a time like 1:02:03, 62:03 or 95", at: Date.now() }); return; }
    emit("seek", { positionMs: ms });
    $("seek-at").value = "";
  }
  $("seek-go").addEventListener("click", seekTyped);
  $("seek-at").addEventListener("keydown", function (e) { if (e.key === "Enter") seekTyped(); });
  $("copy").addEventListener("click", function () { emit("copylink"); });
  $("leave").addEventListener("click", function () { emit("leave"); });
  $("stop").addEventListener("click", function () { emit("stop"); });
  $("start").addEventListener("click", function () {
    emit("start", { link: $("link").value, title: $("name").value });
  });
  $("link").addEventListener("keydown", function (e) { if (e.key === "Enter") $("start").click(); });
  $("join-near").addEventListener("click", function () { emit("joinnear"); });
  $("join").addEventListener("click", function () { emit("join", { code: $("join-code").value }); });
  $("join-code").addEventListener("keydown", function (e) { if (e.key === "Enter") $("join").click(); });

  document.addEventListener("keydown", function (e) {
    var typing = e.target && e.target.tagName === "INPUT";
    if (e.key === "Escape") { e.preventDefault(); emit("close"); return; }
    if (typing || !state || !state.party) return;
    if (e.key === " ") { e.preventDefault(); emit("toggle"); }
    else if (e.key === "ArrowLeft") { emit("nudge", { deltaMs: -10000 }); }
    else if (e.key === "ArrowRight") { emit("nudge", { deltaMs: 10000 }); }
  });

  bridge.on("watch:state", function (payload) {
    state = payload || null;
    stateAt = Date.now();
    render();
  });

  setInterval(tick, 250);
  bridge.ready();
  bridge.emit("watch:ready", {});
})();
