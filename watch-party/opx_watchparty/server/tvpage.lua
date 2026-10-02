-- opx_watchparty -- server/tvpage.lua
--
-- The page a television shows while a watch party is on it: what is playing,
-- where the film is, paused or playing, how many browsers are in step, and the
-- code. It shows NO picture of the film, and that is the design rather than a
-- gap: Netflix's video is protected, plays only in each viewer's own browser on
-- their own account, and is never captured or relayed onto a game surface. The
-- television is the party's clock, in the room.
--
-- Served by this resource itself (GET /v1/tv/<CODE>), so the page and the party
-- it reads come from one origin: it polls ../party/<CODE> once a second and
-- moves its own clock between answers.

WatchTvPage = {}

local PAGE = [==[<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Watch party __CODE__</title>
<style>
:root { --bg: #06080c; --panel: #0d1118; --line: #1c2430; --text: #e8edf4; --dim: #8a96a8;
        --red: #e50914; --cyan: #19e3ef; --amber: #ffb547; }
* { box-sizing: border-box; margin: 0; padding: 0; }
html, body { width: 100%; height: 100%; background: var(--bg); color: var(--text); overflow: hidden;
        font-family: "Segoe UI", "Helvetica Neue", Arial, sans-serif; }
body { display: flex; flex-direction: column; padding: 4.5vh 5vw; gap: 3vh;
       background: radial-gradient(120% 90% at 85% 0%, #1a0a10 0%, var(--bg) 55%),
                   linear-gradient(180deg, transparent 0 60%, rgba(229,9,20,.06)); }
.top { display: flex; justify-content: space-between; align-items: center; font-size: 2.6vh;
       letter-spacing: .35vh; text-transform: uppercase; color: var(--dim); }
.badge { color: var(--text); display: flex; align-items: center; gap: 1.2vh; }
.badge .mark { width: 3.6vh; height: 3.6vh; flex: none; }
.code { font-family: Consolas, "Courier New", monospace; color: var(--cyan); font-size: 3.4vh;
        letter-spacing: .6vh; border: .25vh solid rgba(25,227,239,.35); padding: .6vh 1.4vh; }
.middle { flex: 1; display: flex; flex-direction: column; justify-content: center; gap: 2.5vh; }
.title { font-size: 8.5vh; font-weight: 800; line-height: 1.05; text-shadow: 0 0 3vh rgba(229,9,20,.25);
         overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; }
.state { font-size: 3.6vh; letter-spacing: .5vh; text-transform: uppercase; color: var(--amber); }
.state.playing { color: var(--cyan); }
.bar { position: relative; height: 1.6vh; background: var(--line); border-radius: .8vh; overflow: hidden; }
.fill { position: absolute; left: 0; top: 0; bottom: 0; width: 0%; background: linear-gradient(90deg, var(--red), #ff4d6d);
        box-shadow: 0 0 2vh rgba(229,9,20,.6); }
.row { display: flex; justify-content: space-between; align-items: baseline; font-size: 3.2vh; }
.time { font-family: Consolas, "Courier New", monospace; letter-spacing: .2vh; }
.sync { color: var(--dim); font-size: 2.6vh; }
.sync b { color: var(--cyan); }
.how { color: var(--dim); font-size: 2.3vh; line-height: 1.5; border-top: .2vh solid var(--line); padding-top: 2vh; }
.how b { color: var(--text); font-weight: 600; }
.ended .middle { opacity: .55; }
</style>
</head>
<body>
<div class="top">
  <div class="badge"><svg class="mark" viewBox="0 0 128 128" aria-hidden="true"><rect x="3" y="3" width="122" height="122" rx="20" fill="#0a0d13" stroke="#e50914" stroke-width="6"/><path d="M106.57 79.49A45.3 45.3 0 0 1 42.04 103.62M21.43 48.51A45.3 45.3 0 0 1 85.96 24.38" fill="none" stroke="#19dfeb" stroke-width="6.5"/><path d="M47 39 93 63.5 47 88Z" fill="#e50914"/></svg> Watch party &middot; Netflix</div>
  <div class="code" id="code">__CODE__</div>
</div>
<div class="middle">
  <div class="title" id="title">Connecting&hellip;</div>
  <div class="state" id="state">&nbsp;</div>
</div>
<div class="bar"><div class="fill" id="fill"></div></div>
<div class="row">
  <div class="time" id="time">0:00</div>
  <div class="sync" id="sync">&nbsp;</div>
</div>
<div class="how" id="how">The film plays in each viewer's <b>own browser</b>, on their own Netflix account, kept in step with
this screen. In game: <b>/watch</b> &middot; join with <b>/watch join __CODE__</b>, then <b>Copy browser link</b> and
open it in Edge or Chrome with the <b>Open77 Watch Party</b> extension.</div>
<script>
(function () {
  "use strict";
  var CODE = "__CODE__";
  var URL_ = "../party/" + CODE;
  var el = function (id) { return document.getElementById(id); };
  var party = null;         // last answer
  var offset = 0;           // server clock - this clock, ms
  var receivedAt = 0;
  var misses = 0;

  function fmt(ms) {
    if (!(ms >= 0)) ms = 0;
    var t = Math.floor(ms / 1000), h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), s = t % 60;
    var two = function (n) { return (n < 10 ? "0" : "") + n; };
    return h > 0 ? h + ":" + two(m) + ":" + two(s) : m + ":" + two(s);
  }

  function position() {
    if (!party) return 0;
    var ms = party.positionMs;
    if (party.playing) ms += (Date.now() + offset) - party.serverMs;
    if (ms < 0) ms = 0;
    if (party.durationMs && ms > party.durationMs) ms = party.durationMs;
    return ms;
  }

  function draw() {
    if (!party) return;
    var ms = position();
    el("time").textContent = fmt(ms) + (party.durationMs ? " / " + fmt(party.durationMs) : "");
    el("fill").style.width = party.durationMs ? Math.min(100, ms / party.durationMs * 100).toFixed(2) + "%" : "0%";
  }

  function show(p) {
    party = p;
    document.body.classList.remove("ended");
    el("title").textContent = p.title || ("Netflix title " + p.netflixId);
    var st = el("state");
    st.textContent = p.playing ? "▶ Playing" : "❚❚ Paused";
    st.className = "state" + (p.playing ? " playing" : "");
    var who = p.lastBy ? " · last moved by " + p.lastBy : "";
    el("sync").innerHTML = "";
    var b = document.createElement("b");
    b.textContent = String(p.inSync);
    el("sync").appendChild(b);
    el("sync").appendChild(document.createTextNode(" of " + p.browsers + " browser" + (p.browsers === 1 ? "" : "s") +
      " in step · " + p.members + " in the party" + who));
    draw();
  }

  function poll() {
    var t0 = Date.now();
    var xhr = new XMLHttpRequest();
    xhr.open("GET", URL_ + "?t=" + t0, true);
    xhr.timeout = 4000;
    xhr.onload = function () {
      var t1 = Date.now();
      if (xhr.status === 200) {
        try {
          var p = JSON.parse(xhr.responseText);
          offset = p.serverMs - (t0 + t1) / 2;
          receivedAt = t1;
          misses = 0;
          show(p);
        } catch (e) { misses++; }
      } else if (xhr.status === 404) {
        party = null;
        document.body.classList.add("ended");
        el("title").textContent = "This watch party is not on right now";
        el("state").textContent = "Start one in game: /watch netflix <link>";
        el("state").className = "state";
        el("time").textContent = "";
        el("fill").style.width = "0%";
        el("sync").textContent = "";
      } else { misses++; }
    };
    xhr.onerror = xhr.ontimeout = function () {
      misses++;
      if (misses > 5) el("sync").textContent = "reconnecting…";
    };
    xhr.send();
  }

  poll();
  setInterval(poll, 1000);
  setInterval(draw, 250);
})();
</script>
</body>
</html>
]==]

---The page for `code` (already checked: six letters and digits).
function WatchTvPage.html(code)
    return (PAGE:gsub("__CODE__", code))
end
