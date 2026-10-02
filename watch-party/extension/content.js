// Open77 Watch Party -- content.js (the extension's side of a Netflix tab)
//
// Once a second: asks this tab's player where it is (page.js), tells the party
// server, and gets back where the party is. If this player is more than two
// seconds off, or playing while the party is paused (or the reverse), it moves
// the player -- through Netflix's own controls. A member's own play, pause or
// seek in this tab goes to the party, so everyone follows them.
//
// The party code and this viewer's key arrive in the link copied in game
// ("#opxwatch=CODE.key"); they are stored here and wiped from the address bar.
(function () {
  "use strict";
  var Core = self.OpxWatchCore;
  var TAG = "opx-watch";
  var DEFAULT_BASE = "https://xbuniverse.duckdns.org/opx-watch-xb-staging";

  var settings = { base: DEFAULT_BASE, code: "", key: "", viewer: "", enabled: true };
  var loaded = false;
  var party = null, tMid = 0;
  var synced = false, syncedMovie = null;
  var expected = [], holdUntil = 0;   // our own commands' echoes to come; no correcting until
  var loadedAt = Date.now();
  var busy = false;
  var lastError = null;
  var lastStatusText = "";
  var nextTickAt = 0;
  var fragmentRead = false;   // this page load brought a party link

  // ------------------------------------------------------------ settings ----
  function randomId() {
    var a = new Uint8Array(12);
    crypto.getRandomValues(a);
    return Array.prototype.map.call(a, function (b) { return ("0" + b.toString(16)).slice(-2); }).join("");
  }

  function save() {
    try { chrome.storage.local.set({ settings: settings }); } catch (e) { /* extension reloaded */ }
  }

  var pendingFragment = null;   // a link read before the stored settings came

  function applyFragment(got) {
    // A code-only link for the party this browser is already in keeps its key.
    if (!(got.key === "" && got.code === settings.code && Core.validKey(settings.key))) settings.key = got.key;
    settings.code = got.code;
    settings.enabled = true;
    fragmentRead = true;
    save();
    party = null;
    synced = false;
  }

  function readFragment() {
    var got = Core.parseFragment(location.hash);
    if (!got) return false;
    // The key is this viewer's own: out of the address bar and the history.
    try {
      history.replaceState(history.state, "", location.pathname + location.search + Core.stripFragment(location.hash));
    } catch (e) { /* the page will drop it on its own navigation */ }
    if (loaded) applyFragment(got); else pendingFragment = got;
    return true;
  }

  // Read at once, before Netflix's page rewrites the address.
  readFragment();
  window.addEventListener("hashchange", readFragment);

  chrome.storage.local.get("settings", function (got) {
    var stored = got && got.settings;
    if (stored && typeof stored === "object") {
      if (typeof stored.base === "string" && /^https:\/\/xbuniverse\.duckdns\.org\//.test(stored.base)) settings.base = stored.base;
      if (!settings.code && Core.validCode(stored.code)) {
        settings.code = stored.code;
        settings.key = Core.validKey(stored.key) ? stored.key : "";
      }
      if (typeof stored.viewer === "string" && /^[a-f0-9]{24}$/.test(stored.viewer)) settings.viewer = stored.viewer;
      if (stored.enabled === false && !fragmentRead) settings.enabled = false;
    }
    if (!settings.viewer) settings.viewer = randomId();
    loaded = true;
    if (pendingFragment) { applyFragment(pendingFragment); pendingFragment = null; } else save();
  });
  chrome.storage.onChanged.addListener(function (changes) {
    if (!changes.settings || !changes.settings.newValue) return;
    var s = changes.settings.newValue;
    if (s.code !== settings.code || s.key !== settings.key || s.enabled !== settings.enabled || s.base !== settings.base) {
      settings.code = Core.validCode(s.code) ? s.code : "";
      settings.key = Core.validKey(s.key) ? s.key : "";
      settings.enabled = s.enabled !== false;
      if (typeof s.base === "string" && /^https:\/\/xbuniverse\.duckdns\.org\//.test(s.base)) settings.base = s.base;
      party = null;
      synced = false;
      nextTickAt = 0;
    }
  });

  // ------------------------------------------------------- the page bridge ----
  var pending = {};
  var seq = 0;
  function ask(cmd, args) {
    return new Promise(function (resolve) {
      var id = "c" + (++seq);
      pending[id] = resolve;
      window.postMessage({ tag: TAG, dir: "toPage", id: id, cmd: cmd, args: args || {} }, "*");
      setTimeout(function () {
        if (pending[id]) { delete pending[id]; resolve({ ok: false, error: "page_timeout" }); }
      }, 800);
    });
  }
  window.addEventListener("message", function (e) {
    if (e.source !== window || !e.data || e.data.tag !== TAG || e.data.dir !== "fromPage") return;
    if (e.data.id && pending[e.data.id]) {
      var resolve = pending[e.data.id];
      delete pending[e.data.id];
      resolve(e.data.reply || { ok: false });
    } else if (e.data.event) {
      onViewerEvent(e.data.event);
    }
  });

  // -------------------------------------------------------------- server ----
  function server(method, path, body) {
    return new Promise(function (resolve) {
      try {
        chrome.runtime.sendMessage({ type: "opx-fetch", method: method, url: settings.base + path,
          body: body ? JSON.stringify(body) : undefined }, function (resp) {
          if (chrome.runtime.lastError || !resp) {
            return resolve({ ok: false, error: chrome.runtime.lastError ? chrome.runtime.lastError.message : "no_answer" });
          }
          if (!resp.ok) return resolve({ ok: false, error: resp.error || "network" });
          var data = null;
          try { data = JSON.parse(resp.text); } catch (e) { /* not JSON */ }
          var ok = resp.status >= 200 && resp.status < 300 && data && data.ok !== false;
          resolve({ ok: ok, status: resp.status, data: data, error: data && data.error });
        });
      } catch (e) {
        resolve({ ok: false, error: "extension_reloaded" });
      }
    });
  }

  // --------------------------------------------------------------- badge ----
  var host = null, shadow = null, pill = null, card = null, open = false;
  function ensureBadge() {
    if (host && document.documentElement.contains(host)) return;
    host = document.createElement("div");
    host.style.cssText = "position:fixed;top:12px;right:12px;z-index:2147483647;";
    shadow = host.attachShadow({ mode: "closed" });
    shadow.innerHTML =
      "<style>" +
      ".pill{font:600 12px/1 'Segoe UI',Arial,sans-serif;color:#e9eef5;background:rgba(8,10,15,.88);" +
      "border:1px solid #2a3340;border-left:3px solid #e50914;padding:7px 10px;border-radius:3px;cursor:pointer;" +
      "letter-spacing:.3px;white-space:nowrap}" +
      ".ok{border-left-color:#19e3ef}.warn{border-left-color:#ffb547}.bad{border-left-color:#ff4d5a}" +
      ".card{margin-top:6px;width:300px;font:13px/1.45 'Segoe UI',Arial,sans-serif;color:#e9eef5;" +
      "background:rgba(8,10,15,.94);border:1px solid #2a3340;padding:10px 12px;display:none}" +
      ".card.open{display:block}.card b{color:#fff}.dim{color:#8d99ab}" +
      "button{font:600 12px 'Segoe UI',Arial,sans-serif;margin-top:8px;margin-right:6px;background:#e50914;color:#fff;" +
      "border:0;padding:6px 10px;border-radius:3px;cursor:pointer}button.alt{background:#1b2230}" +
      "</style><div class='pill' id='pill'></div><div class='card' id='card'></div>";
    pill = shadow.getElementById("pill");
    card = shadow.getElementById("card");
    pill.addEventListener("click", function () { open = !open; card.className = "card" + (open ? " open" : ""); });
    (document.body || document.documentElement).appendChild(host);
  }

  function setBadge(kind, text, details) {
    if (!document.documentElement) return;
    ensureBadge();
    pill.className = "pill " + (kind || "");
    pill.textContent = text;
    card.innerHTML = "";
    (details || []).forEach(function (node) { card.appendChild(node); });
    var statusText = kind + "|" + text;
    if (statusText !== lastStatusText) {
      lastStatusText = statusText;
      try {
        chrome.storage.local.set({ lastStatus: { kind: kind, text: text, code: settings.code,
          keyed: Core.validKey(settings.key), title: party && party.title, at: Date.now() } });
      } catch (e) { /* reloaded */ }
    }
  }

  function line(html) {
    var div = document.createElement("div");
    div.textContent = html;
    return div;
  }
  function button(label, alt, onClick) {
    var b = document.createElement("button");
    b.textContent = label;
    if (alt) b.className = "alt";
    b.addEventListener("click", onClick);
    return b;
  }

  function partyDetails(extra) {
    var out = [];
    if (party) {
      var t = document.createElement("div");
      var b = document.createElement("b");
      b.textContent = party.title || ("Netflix title " + party.netflixId);
      t.appendChild(b);
      out.push(t);
      out.push(line((party.playing ? "Playing" : "Paused") + " at " +
        Core.fmt(Core.target(party, tMid, Date.now())) + (party.durationMs ? " / " + Core.fmt(party.durationMs) : "")));
      out.push(line(party.inSync + " of " + party.browsers + " browsers in step, " + party.members + " in the party"));
    }
    var d = line("Party " + (settings.code || "-") + (Core.validKey(settings.key)
      ? " - your play/pause/seek moves the party" : " - following only (copy your own link in game to control)"));
    d.className = "dim";
    out.push(d);
    (extra || []).forEach(function (n) { out.push(n); });
    out.push(button("Leave party", true, function () {
      settings.code = ""; settings.key = ""; save(); party = null; synced = false;
      if (host) host.remove();
    }));
    return out;
  }

  // ------------------------------------------------------------ the loop ----
  async function tick() {
    if (busy || !loaded) return;
    if (Date.now() < nextTickAt) return;
    busy = true;
    try {
      readFragment();
      if (!settings.enabled || !Core.validCode(settings.code)) {
        if (host) host.remove();
        return;
      }
      var movieId = Core.movieIdFromPath(location.pathname);
      var st = movieId ? await ask("status") : null;
      var status = st && st.ok ? st.status : null;
      var ts = Date.now();
      var body = { viewer: settings.viewer, netflixId: movieId,
        positionMs: status && status.ready ? status.positionMs : null,
        playing: !!(status && status.playing), durationMs: status ? status.durationMs : null,
        title: status ? status.title : "" };
      if (Core.validKey(settings.key)) body.key = settings.key;
      var t0 = Date.now();
      var r = await server("POST", "/v1/party/" + settings.code + "/report", body);
      var t1 = Date.now();
      if (!r.ok) {
        if (r.error === "no_such_party") {
          party = null;
          setBadge("bad", "Watch party " + settings.code + " has ended", [button("Forget it", true, function () {
            settings.code = ""; settings.key = ""; save(); if (host) host.remove();
          })]);
          nextTickAt = Date.now() + 5000;
        } else if (r.error === "key_not_in_party") {
          settings.key = "";
          save();
          setBadge("warn", "Following party " + settings.code + " (your key left it)", partyDetails());
        } else {
          setBadge("warn", "Watch party: can't reach the server (" + (r.error || r.status) + ")", partyDetails());
          nextTickAt = Date.now() + 3000;
        }
        return;
      }
      party = r.data;
      tMid = (t0 + t1) / 2;
      var title = party.title || ("Netflix title " + party.netflixId);
      if (movieId !== party.netflixId) {
        if (synced && syncedMovie === movieId && movieId) {
          // The party moved on to another title while this tab was following it.
          location.href = Core.watchUrl(party.netflixId, Core.target(party, tMid, Date.now()));
          return;
        }
        setBadge("warn", "Watch party " + party.code + " is watching " + title, partyDetails([
          button("Open it here", false, function () {
            location.href = Core.watchUrl(party.netflixId, Core.target(party, tMid, Date.now()));
          })]));
        synced = false;
        return;
      }
      if (!status || !status.ready) {
        setBadge("warn", "Watch party " + party.code + ": waiting for the player", partyDetails());
        return;
      }
      var now = Date.now();
      if (now < holdUntil) return;
      var localNow = status.positionMs + (status.playing ? now - ts : 0);
      var targetNow = Core.target(party, tMid, now);
      var d = Core.decide(localNow, status.playing, targetNow, party.playing);
      if (d.seekMs != null || d.play || d.pause) {
        var until = Date.now() + Core.ECHO_GUARD_MS;
        expected = expected.filter(function (e) { return e.until > Date.now(); });
        if (d.seekMs != null) expected.push({ type: "seeked", positionMs: d.seekMs, until: until });
        if (d.play) expected.push({ type: "play", until: until });
        if (d.pause) expected.push({ type: "pause", until: until });
        if (d.seekMs != null) {
          if (!status.api) {
            setBadge("bad", "Watch party: this page's player can't be moved -- reload the tab", partyDetails());
            return;
          }
          // A little ahead while playing: the seek itself takes a moment.
          await ask("seek", { positionMs: d.seekMs + (party.playing ? 400 : 0) });
        }
        if (d.play) await ask("play");
        if (d.pause) await ask("pause");
        setBadge("warn", "↻ Syncing to party " + party.code, partyDetails());
      } else {
        setBadge("ok", "● Party " + party.code + " · " + (party.playing ? "in step" : "paused together"),
          partyDetails());
      }
      synced = true;
      syncedMovie = movieId;
    } catch (e) {
      lastError = String(e);
    } finally {
      busy = false;
    }
  }

  async function onViewerEvent(ev) {
    var now = Date.now();
    var echo = Core.matchEcho(ev, expected, now);
    if (echo >= 0) { expected.splice(echo, 1); return; }
    if (!party || !Core.validKey(settings.key)) return;
    var ctx = {
      synced: synced, canControl: true, now: now, loadedAt: loadedAt,
      party: party, movieId: Core.movieIdFromPath(location.pathname),
      partyPlaying: party.playing, partyPositionMs: Core.target(party, tMid, now),
    };
    if (!Core.isViewerAction(ev, ctx)) return;
    var action = ev.type === "seeked" ? "seek" : ev.type;
    holdUntil = now + 1500;
    var r = await server("POST", "/v1/party/" + settings.code + "/control",
      { key: settings.key, action: action, positionMs: ev.positionMs });
    if (r.ok && r.data) {
      party = r.data;
      tMid = Date.now();
    }
  }

  setInterval(tick, 1000);
})();
