// Open77 Watch Party -- sync-core.js
//
// The extension's decisions, as pure functions: no DOM, no network, no clock read.
// The content script uses them in the browser; tests/sync-core.test.js runs them
// in node against the same cases the server's Lua playhead is tested with, so
// "where is the party" and "what does this browser do about it" are one rule.
(function (root) {
  "use strict";

  var CODE = /^[A-Z2-9]{6}$/;
  var KEY = /^[a-z2-9]{12}$/;

  var Core = {
    TOLERANCE_MS: 2000,     // closer than this to the party: leave the player alone
    ECHO_GUARD_MS: 2500,    // our own commands' events are ignored for this long
    SETTLE_MS: 5000,        // after a page loads, its autoplay is not a viewer's choice

    /** "#opxwatch=ABC123.k3ykeyk3ykey" -> { code, key } (key may be ""), else null. */
    parseFragment: function (hash) {
      if (typeof hash !== "string") return null;
      var m = /(?:^#|[#&])opxwatch=([A-Za-z2-9]{6})(?:\.([a-z2-9]{12}))?(?:&|$)/.exec(hash);
      if (!m) return null;
      return { code: m[1].toUpperCase(), key: m[2] || "" };
    },

    /** The fragment with our part removed ("" when nothing else was in it). */
    stripFragment: function (hash) {
      if (typeof hash !== "string") return "";
      var rest = hash.replace(/^#/, "").split("&").filter(function (p) { return !/^opxwatch=/.test(p); });
      return rest.length ? "#" + rest.join("&") : "";
    },

    validCode: function (code) { return typeof code === "string" && CODE.test(code); },
    validKey: function (key) { return typeof key === "string" && KEY.test(key); },

    /** The Netflix title id of a /watch/<id> page, else null. */
    movieIdFromPath: function (pathname) {
      var m = /^\/watch\/(\d{5,12})(?:[/?#]|$)/.exec(String(pathname || ""));
      return m ? m[1] : null;
    },

    /**
     * Where the party's film is at local time `nowLocal`, from an answer the
     * server stamped `serverMs` when this browser's clock read `tMid` (the middle
     * of the request: the clock offset is taken as half the round trip).
     */
    target: function (party, tMid, nowLocal) {
      var ms = party.positionMs || 0;
      if (party.playing) ms += Math.max(0, nowLocal - tMid);
      if (ms < 0) ms = 0;
      if (party.durationMs && ms > party.durationMs) ms = party.durationMs;
      return Math.round(ms);
    },

    /** Same rule as WatchPlayhead.decide on the server. */
    decide: function (localMs, localPlaying, targetMs, targetPlaying, toleranceMs) {
      if (toleranceMs == null) toleranceMs = Core.TOLERANCE_MS;
      var out = { driftMs: Math.round(localMs - targetMs) };
      if (Math.abs(localMs - targetMs) > toleranceMs) out.seekMs = Math.round(targetMs);
      if (targetPlaying && !localPlaying) out.play = true;
      if (!targetPlaying && localPlaying) out.pause = true;
      return out;
    },

    /**
     * Which of this extension's own recent commands an event is the echo of, or
     * -1. A seek's echo is a `seeked` near where it was sent; play and pause echo
     * as themselves. Anything else -- a seek somewhere different, a pause right
     * after our play -- is somebody's hand on the player.
     */
    matchEcho: function (event, expected, now) {
      if (!expected) return -1;
      for (var i = 0; i < expected.length; i++) {
        var e = expected[i];
        if (now > e.until || e.type !== event.type) continue;
        if (event.type === "seeked" && Math.abs((event.positionMs || 0) - e.positionMs) > Core.TOLERANCE_MS) continue;
        return i;
      }
      return -1;
    },

    /**
     * Whether a play / pause / seek the browser saw was the VIEWER's (and so goes
     * to the party): not the page's autoplay, not on another title, and not
     * something the party already agrees with. (Echoes of this extension's own
     * commands are taken out first, by `matchEcho`.)
     */
    isViewerAction: function (event, ctx) {
      if (!ctx || !ctx.synced || !ctx.canControl) return false;
      if (ctx.now - ctx.loadedAt < Core.SETTLE_MS) return false;
      if (!ctx.party || ctx.party.netflixId !== ctx.movieId) return false;
      if (event.type === "play") return !ctx.partyPlaying;
      if (event.type === "pause") return ctx.partyPlaying;
      if (event.type === "seeked") return Math.abs(event.positionMs - ctx.partyPositionMs) > Core.TOLERANCE_MS;
      return false;
    },

    fmt: function (ms) {
      if (!(ms >= 0)) ms = 0;
      var t = Math.floor(ms / 1000), h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), s = t % 60;
      var two = function (n) { return (n < 10 ? "0" : "") + n; };
      return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s);
    },

    /** A watch page for the party's title, at its position. */
    watchUrl: function (netflixId, positionMs) {
      var seconds = Math.floor((positionMs || 0) / 1000);
      return "https://www.netflix.com/watch/" + netflixId + (seconds > 0 ? "?t=" + seconds : "");
    },
  };

  if (typeof module !== "undefined" && module.exports) module.exports = Core;
  else root.OpxWatchCore = Core;
})(typeof self !== "undefined" ? self : this);
