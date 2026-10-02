// Open77 Watch Party -- page.js (runs in the Netflix page's own world)
//
// The only part that touches Netflix's player, and only through the player's own
// controls -- the same calls its buttons make: where the film is, play, pause,
// and seek. It never reads, copies or captures the picture or the sound.
//
// Netflix's player object is `netflix.appContext.state.playerApp.getAPI()
// .videoPlayer`; seeking goes through it because setting the <video> element's
// time directly makes Netflix stop with an error. The <video> element is the
// fallback for reading the time and for play/pause, and the source of the
// viewer's own play/pause/seek events.
(function () {
  "use strict";
  var TAG = "opx-watch";

  function player() {
    try {
      var vp = window.netflix.appContext.state.playerApp.getAPI().videoPlayer;
      var ids = vp.getAllPlayerSessionIds();
      if (!ids || !ids.length) return null;
      var sid = null;
      for (var i = 0; i < ids.length; i++) if (/^watch/.test(ids[i])) sid = ids[i];
      return vp.getVideoPlayerBySessionId(sid || ids[ids.length - 1]) || null;
    } catch (e) {
      return null;
    }
  }

  function video() {
    var all = document.getElementsByTagName("video");
    return all.length ? all[all.length - 1] : null;
  }

  function call(obj, name) {
    try { return typeof obj[name] === "function" ? obj[name]() : undefined; } catch (e) { return undefined; }
  }

  function status() {
    var p = player();
    var v = video();
    var position = p ? call(p, "getCurrentTime") : undefined;
    if (typeof position !== "number" && v) position = v.currentTime * 1000;
    var paused = p ? call(p, "isPaused") : undefined;
    if (typeof paused !== "boolean" && v) paused = v.paused;
    var duration = p ? call(p, "getDuration") : undefined;
    if (typeof duration !== "number" && v && isFinite(v.duration)) duration = v.duration * 1000;
    var titleEl = document.querySelector('[data-uia="video-title"]');
    var title = titleEl ? (titleEl.textContent || "").replace(/\s+/g, " ").trim().slice(0, 80) : "";
    return {
      ready: typeof position === "number" && typeof paused === "boolean",
      api: !!p,
      positionMs: typeof position === "number" ? Math.round(position) : null,
      playing: paused === false,
      durationMs: typeof duration === "number" && duration > 0 ? Math.round(duration) : null,
      title: title,
    };
  }

  function command(cmd, args) {
    var p = player();
    var v = video();
    if (cmd === "seek") {
      if (!p || typeof p.seek !== "function") return { ok: false, error: "no_player_api" };
      p.seek(Math.max(0, Math.round(args.positionMs)));
      return { ok: true };
    }
    if (cmd === "play") {
      if (p && typeof p.play === "function") { p.play(); return { ok: true }; }
      if (v) { var r = v.play(); if (r && r.catch) r.catch(function () {}); return { ok: true }; }
      return { ok: false, error: "no_player" };
    }
    if (cmd === "pause") {
      if (p && typeof p.pause === "function") { p.pause(); return { ok: true }; }
      if (v) { v.pause(); return { ok: true }; }
      return { ok: false, error: "no_player" };
    }
    if (cmd === "status") return { ok: true, status: status() };
    return { ok: false, error: "unknown_command" };
  }

  window.addEventListener("message", function (e) {
    if (e.source !== window || !e.data || e.data.tag !== TAG || e.data.dir !== "toPage") return;
    var reply;
    try { reply = command(e.data.cmd, e.data.args || {}); } catch (err) { reply = { ok: false, error: String(err) }; }
    window.postMessage({ tag: TAG, dir: "fromPage", id: e.data.id, reply: reply }, "*");
  });

  // The viewer's own actions, from whichever <video> is current.
  var watched = null;
  function report(type) {
    var v = watched;
    window.postMessage({ tag: TAG, dir: "fromPage", event: {
      type: type, positionMs: v ? Math.round(v.currentTime * 1000) : null } }, "*");
  }
  var onPlay = function () { report("play"); };
  var onPause = function () { report("pause"); };
  var onSeeked = function () { report("seeked"); };
  setInterval(function () {
    var v = video();
    if (v === watched) return;
    if (watched) {
      watched.removeEventListener("play", onPlay);
      watched.removeEventListener("pause", onPause);
      watched.removeEventListener("seeked", onSeeked);
    }
    watched = v;
    if (v) {
      v.addEventListener("play", onPlay);
      v.addEventListener("pause", onPause);
      v.addEventListener("seeked", onSeeked);
    }
  }, 500);
})();
