// open77-input.js -- put into the shared browser's client page (neko's index.html)
// by tv_kit.py `clicks-fix` (2026-11-07), after the image's own four open77-*.js
// and ahead of the page's own scripts.
//
// What a click on a television does, told to the television (tv.js logs it as
// `browser_ice (input ...)` in the player's open77 log), and the guard that keeps
// a click from sticking.
//
// Why: on 2026-11-07 (02:52-03:03 UTC) a player clicked the TV room's home screen
// through F8 and nothing answered. The shared browser's neko said why, forty times:
// `button down failed error="debounced button 1"`. neko presses a button on its
// display when the page sends "button 1 down" and lets go when it sends "button 1
// up"; a second "down" while the first was never let go is refused -- and neko lets
// go by itself only after ten seconds. So the page sent the press of every click
// and, for nearly every one, never sent its release. The page sends the release
// from the client's own `mouseup` handler on the picture (its transparent
// textarea.overlay), and only when that handler saw the press too; a release that
// lands anywhere else, or never reaches this page, is lost.
//
// What this script does about it, on the client's own data channel and nothing
// else (it adds no element, and changes no event):
//
//   * it reads what the client sends (the legacy binary messages: MOVE x,y;
//     KEY_DOWN / KEY_UP with keys 1-7 meaning mouse buttons) and which buttons
//     the server therefore holds;
//   * before the client presses a button the server still holds (its release
//     was lost), it lets go of it first -- otherwise neko refuses the press;
//   * when a `mouseup` reaches this page but the client sends no release (it
//     landed on something other than the picture), it sends the release;
//   * when this page loses the focus, or goes away, with a button held, it lets
//     go of it (what any window does);
//   * once a lost release has been seen on this page ("learned"), a move that
//     says the button is up, or 1.5 s without one that says it is down, lets go
//     too: then a click is a click even if its release never arrives.
//
// And it tells the television, one line per click for the first forty (then only
// what a guard did, and a count every minute): where the press went, which element
// the page's own mousedown and mouseup reached, what `buttons` said, who sent the
// release and when, and what happened in between (moves, focus, resize, a change
// of who holds the controls). Never a key, a character or an address.
//
// Version 2 (2026-11-07), from that same log of a player's evening:
//
//   * the game sends most presses twice, a millisecond apart, and the release once:
//     the room's browser got a double click for every click (a tile opened twice,
//     a form's button pressed twice). A second press of the same button at the same
//     point within 30 ms, with no release between, is dropped: a click is a click.
//   * once a tile has put a service on the screen (Netflix, the web), nothing on the
//     screen leads back: the room's Chromium is a kiosk with no toolbar, and the
//     service's tab has no history. A small Home / Back control now sits in the
//     picture's top-left corner, shown while this page holds the controls or the
//     mouse moves over it. Home sends the room's browser Alt+Home (its home page is
//     the room's home screen, by policy) and Back sends Alt+Left, through the
//     client's own input channel -- taking the controls first if another viewer
//     holds them (as a click on the picture would).
//   * the keyboard is counted, never read: how many keys went down and up, any held
//     over 3 s, the longest run of repeats -- the signs of a lost key release.
//
// Version 3 (2026-11-07), from the same evening and a kiosk Chromium tried key by key:
//
//   * keys meant for the game reached the room's browser, and some of them take the
//     television away from the page: F1 put "Google Chrome Help" over the room (the
//     white screen a player was stuck on), F5 (the game's quick save) reloads the page
//     -- a half-typed sign-in with it --, F7 asks about caret browsing and takes the
//     keyboard, F3 opens find; Ctrl+T / Ctrl+N / Ctrl+W open or close tabs and windows,
//     Ctrl+Tab and Ctrl+1..9 switch to a tab nobody sees, Ctrl+H / Ctrl+J / Ctrl+P open
//     history, downloads and print, Ctrl+Shift+Delete Chrome's clear-data page (one
//     click from signing every service out), Shift+Esc Chrome's task manager window;
//     the window manager's own keys (Alt+Space, Alt+Esc, Ctrl/Shift+Alt+arrows, the
//     Windows key) show its menu, take the focus away or switch to an empty desktop.
//     All of those are kept from the room's browser now (press and release), and told
//     by name -- they are shortcuts, not text. Everything typed still goes through
//     untouched, AltGr (Ctrl+Alt) characters too; which modifiers are held is read from
//     the page's own key event, so a modifier whose release was lost cannot block a key.
//   * how the picture arrives, from WebRTC's own numbers: its size and frame rate, the
//     round trip to the server, how long frames wait to be smoothed and to be decoded,
//     loss, drops, freezes, and whether it came over UDP or TCP (never an address) --
//     once 10 s after it starts, then every minute, and at once when it gets slow.
//
// `window.__open77Input()` returns the same state for a test page.
(function () {
  "use strict";
  if (window.__open77Input) return;

  var VERSION = "open77-input 3";
  var DETAIL_CLICKS = 40;       // clicks told in full; after that only what goes wrong
  var STALE_MS = 10000;         // neko lets go of a held button by itself after 10 s
  var STILL_HELD_MS = 3000;     // a press with no release by then is told as such
  var LEARNED_HOLD_MS = 1500;   // once learned: a hold with no "button down" move this long is let go
  var LEARNED_MOVE_MS = 40;     // once learned: a move with the button up this long after the press lets go
  var SUMMARY_MS = typeof window.__open77InputSummaryMs === "number" ? window.__open77InputSummaryMs : 60000;  // a test page's only
  var DUP_MS = 30;              // a second press of a held button this soon, at the same point, is the game's duplicate
  var REMOTE_SHOW_MS = 4000;    // the Home / Back control stays this long after the mouse last moved over the page
  var CONTROL_WAIT_MS = 2000;   // how long Home / Back waits for the controls it asked for
  var KEY_ALT = 0xffe9, KEY_HOME = 0xff50, KEY_LEFT = 0xff51;   // X keysyms: Alt_L, Home, Left

  var started = Date.now();
  function since(t) { return (Date.now() - t) + " ms"; }

  // ---- telling the television
  var told = 0;
  function post(text) {
    try {
      if (window.parent && window.parent !== window) {
        window.parent.postMessage({ open77SharedBrowser: 1, kind: "input", mode: "auto",
          text: String(text).slice(0, 600) }, "*");
      }
      told++;
    } catch (error) { /* telling the television is best effort */ }
  }
  function describe(target) {
    if (!target || !target.tagName) return target === window ? "window" : (target === document ? "document" : "nothing");
    var name = target.tagName.toLowerCase();
    if (target.id) name += "#" + target.id;
    if (target.classList && target.classList.length) name += "." + Array.prototype.slice.call(target.classList, 0, 2).join(".");
    return name;
  }
  function isPicture(target) {
    return !!(target && target.tagName === "TEXTAREA" && target.classList && target.classList.contains("overlay"));
  }
  var BUTTON_NAME = { 1: "left", 2: "middle", 3: "right" };
  function buttonName(key) { return BUTTON_NAME[key] || ("button " + key); }
  // neko's key for a DOM button (0 left, 1 middle, 2 right) is button + 1; the DOM
  // `buttons` bit for each: left 1, right 2, middle 4.
  var BIT = { 1: 1, 2: 4, 3: 2, 4: 8, 5: 16 };

  // ---- who holds the controls, from the client's own socket
  var me = "", host = "", hostChanges = 0;
  var clientSocket = null;
  function watchSocket(ws) {
    clientSocket = ws;
    try {
      ws.addEventListener("message", function (event) {
        if (typeof event.data !== "string" || event.data.length > 65536 || event.data.indexOf("\"event\"") < 0) return;
        var message;
        try { message = JSON.parse(event.data); } catch (error) { return; }
        if (!message || typeof message !== "object") return;
        if (message.event === "signal/provide" && typeof message.id === "string") me = message.id;
        else if (message.event === "control/locked") { host = String(message.id || ""); hostChanges++; noteAll("host " + whose()); remoteShow(); }
        else if (message.event === "control/release") { host = ""; hostChanges++; noteAll("controls released"); remoteShow(); }
      });
    } catch (error) { /* watching is best effort */ }
  }
  function whose() { return host === "" ? "nobody" : (me !== "" && host === me ? "this page" : "another viewer"); }
  var NativeSocket = window.WebSocket;
  if (typeof NativeSocket === "function") {
    var WatchedSocket = function (url, protocols) {
      var ws = protocols === undefined ? new NativeSocket(url) : new NativeSocket(url, protocols);
      try {
        if (/\/(api\/)?ws$/.test(new URL(String(url), location.href).pathname)) watchSocket(ws);
      } catch (error) { /* not the client's socket */ }
      return ws;
    };
    WatchedSocket.prototype = NativeSocket.prototype;
    ["CONNECTING", "OPEN", "CLOSING", "CLOSED"].forEach(function (name) { WatchedSocket[name] = NativeSocket[name]; });
    window.WebSocket = WatchedSocket;
  }

  // ---- what the client sends on its data channel
  var Channel = window.RTCDataChannel;
  var nativeSend = Channel && Channel.prototype ? Channel.prototype.send : null;
  var channel = null;            // the channel the client's input went out on
  var pos = { x: -1, y: -1 };    // where the last MOVE put the server's pointer
  var held = {};                 // key -> the click holding it
  var guardReleasedAt = {};      // key -> when a guard let go of it (the client's own late release is then dropped)
  var learned = false;           // a lost release has been seen on this page
  var seq = 0, moves = 0;
  var stats = { clicks: 0, byClient: 0, byRepress: 0, byMouseup: 0, byMove: 0, byHold: 0, byFocus: 0, byStale: 0,
                lateDropped: 0, stillHeld: 0, dupDropped: 0 };
  var dupTold = false;
  var kbd = { downs: 0, ups: 0, heldLong: 0, run: 0, longestRun: 0, lastKey: -1, lastUpAt: 0, down: {} };
  var lastStats = "";

  function bytesOf(data) {
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    return null;
  }
  function keyMessage(opcode, key) {
    // The client's own layout (neko legacy): opcode, length 8 (LE), key as a 64-bit LE integer.
    var buffer = new ArrayBuffer(11);
    var view = new DataView(buffer);
    view.setUint8(0, opcode);
    view.setUint16(1, 8, true);
    view.setUint32(3, key, true);
    view.setUint32(7, 0, true);
    return buffer;
  }
  function sendUp(key) {
    if (!channel || !nativeSend || channel.readyState !== "open") return false;
    try {
      nativeSend.call(channel, keyMessage(0x04, key));
      return true;
    } catch (error) {
      return false;
    }
  }
  function useChannel(ch) {
    if (channel === ch) return;
    var first = channel === null;
    channel = ch;
    if (first) post("the client's input channel: '" + ch.label + "', " + (ch.ordered ? "ordered" : "UNORDERED") +
      (ch.maxRetransmits !== null && ch.maxRetransmits !== undefined ? ", maxRetransmits " + ch.maxRetransmits : ", reliable"));
  }
  // The channel the client makes for its input ('data'), known from its making, so Home works before the first click.
  // The peer connection it is made on (and any the client answers with) is kept too: its numbers say how the picture
  // arrives.
  var PC = window.RTCPeerConnection;
  var pcs = [];
  function rememberPc(pc) {
    if (!pc || pcs.indexOf(pc) >= 0) return;
    pcs.push(pc);
    if (pcs.length > 4) pcs.shift();
  }
  if (PC && PC.prototype && typeof PC.prototype.createDataChannel === "function") {
    var nativeCreate = PC.prototype.createDataChannel;
    PC.prototype.createDataChannel = function (label) {
      var ch = nativeCreate.apply(this, arguments);
      try { if (label === "data" && channel === null) channel = ch; rememberPc(this); } catch (error) { /* best effort */ }
      return ch;
    };
  }
  if (PC && PC.prototype && typeof PC.prototype.setRemoteDescription === "function") {
    var nativeSetRemote = PC.prototype.setRemoteDescription;
    PC.prototype.setRemoteDescription = function () {
      try { rememberPc(this); } catch (error) { /* best effort */ }
      return nativeSetRemote.apply(this, arguments);
    };
  }

  // The page's own last mousedown / mouseup / pointer state, read in the capture
  // phase, before the client's handler on the picture sends anything.
  var lastDown = null, lastUp = null;
  function eventInfo(event) {
    return { t: Date.now(), target: describe(event.target), picture: isPicture(event.target), button: event.button,
             buttons: event.buttons, trusted: event.isTrusted, x: Math.round(event.clientX), y: Math.round(event.clientY) };
  }
  function infoText(info) {
    if (!info) return "none";
    return "on " + info.target + " at " + info.x + "," + info.y + " (buttons=" + info.buttons + (info.trusted ? "" : ", synthetic") + ")";
  }

  function noteAll(text) {
    Object.keys(held).forEach(function (key) {
      var click = held[key];
      if (click && click.between.length < 8) click.between.push(text + " @" + since(click.at));
    });
  }

  function finish(click, via) {
    click.via = via;
    click.upAt = Date.now();
    if (click.timer !== null) { clearTimeout(click.timer); click.timer = null; }
    if (click.holdTimer !== null) { clearTimeout(click.holdTimer); click.holdTimer = null; }
    var guarded = via !== "client";
    if (via === "client") stats.byClient++;
    if (click.seq <= DETAIL_CLICKS || guarded) {
      var up = click.upEvent ? "the page's mouseup " + infoText(click.upEvent) : "no mouseup reached this page";
      post("click " + click.seq + " " + buttonName(click.key) + ": pressed at " + click.x + "," + click.y + " of the screen; " +
        (click.down ? "the page's mousedown " + infoText(click.down) : "no mousedown on this page") +
        (click.dups ? " (the game pressed it " + (click.dups + 1) + " times; " + click.dups + " dropped)" : "") +
        " | released by " + via + " after " + (click.upAt - click.at) + " ms; " + up +
        " | between: " + click.moves + " moves (" + click.movesUp + " with the button up)" +
        (click.between.length ? ", " + click.between.join(", ") : "") + (learned ? " | learned" : ""));
    }
  }

  function guardRelease(key, why, statName) {
    var click = held[key];
    if (!click) return false;
    if (!sendUp(key)) return false;
    delete held[key];
    guardReleasedAt[key] = Date.now();
    stats[statName] = (stats[statName] || 0) + 1;
    finish(click, why);
    return true;
  }

  function pressed(key) {
    var now = Date.now();
    var old = held[key];
    if (old && now - old.at < DUP_MS && pos.x === old.x && pos.y === old.y) {
      // The game's own second press of this click: dropped, so the click is not a double click.
      stats.dupDropped++;
      old.dups++;
      if (!dupTold) {
        dupTold = true;
        post("the game sent this press twice (the second " + (now - old.at) + " ms after the first, at the same point, with no " +
          "release between): the second is dropped from now on, so a click is one click and not a double click");
      }
      return false;
    }
    if (old) {
      delete held[key];
      if (now - old.at < STALE_MS) {
        // The server still holds it: neko would refuse this press. Let go first.
        sendUp(key);
        guardReleasedAt[key] = now;
        stats.byRepress++;
        if (!learned) post("a release was lost on this page (click " + old.seq + " never sent one): " +
          "from now on a move with the button up, or 1.5 s, lets go of a press");
        learned = true;
        finish(old, "the guard, at the next press");
      } else {
        stats.byStale++;
        finish(old, "neko itself (held over 10 s)");
      }
    }
    seq++;
    stats.clicks++;
    var click = { seq: seq, key: key, at: now, x: pos.x, y: pos.y, down: lastDown && now - lastDown.t < 1000 ? lastDown : null,
                  upEvent: null, moves: 0, movesUp: 0, between: [], via: "", upAt: 0, timer: null, holdTimer: null, dups: 0 };
    held[key] = click;
    click.timer = setTimeout(function () {
      click.timer = null;
      if (held[key] !== click) return;
      stats.stillHeld++;
      if (click.seq <= DETAIL_CLICKS || learned) {
        post("click " + click.seq + " " + buttonName(key) + ": still held after " + STILL_HELD_MS + " ms -- " +
          (click.upEvent ? "the page's mouseup came " + infoText(click.upEvent) + " but no release was sent"
                         : "no mouseup reached this page") +
          " (" + click.moves + " moves since the press, " + click.movesUp + " with the button up" +
          (click.between.length ? "; " + click.between.join(", ") : "") + ")");
      }
    }, STILL_HELD_MS);
    if (learned) armHold(click);
    return true;
  }

  function armHold(click) {
    if (click.holdTimer !== null) clearTimeout(click.holdTimer);
    click.holdTimer = setTimeout(function () {
      click.holdTimer = null;
      if (held[click.key] === click) guardRelease(click.key, "the guard, " + LEARNED_HOLD_MS + " ms without a move holding it", "byHold");
    }, LEARNED_HOLD_MS);
  }

  function released(key) {
    var click = held[key];
    if (!click) {
      var at = guardReleasedAt[key];
      if (at && Date.now() - at < 15000) {
        // A guard already let go: sending the client's release too would only make
        // neko log "button up failed". Dropped.
        delete guardReleasedAt[key];
        stats.lateDropped++;
        return false;
      }
      return true;
    }
    delete held[key];
    finish(click, "client");
    return true;
  }

  if (typeof nativeSend === "function") {
    Channel.prototype.send = function (data) {
      var forward = true;
      try {
        var bytes = bytesOf(data);
        if (bytes !== null && bytes.length >= 7) {
          var opcode = bytes[0];
          if (opcode === 0x01 && bytes.length === 7) {
            pos.x = bytes[3] | (bytes[4] << 8);
            pos.y = bytes[5] | (bytes[6] << 8);
            moves++;
            useChannel(this);
          } else if ((opcode === 0x03 || opcode === 0x04) && bytes.length === 11 &&
                     (bytes[7] | bytes[8] | bytes[9] | bytes[10]) === 0) {
            var key = bytes[3] | (bytes[4] << 8) | (bytes[5] << 16) | (bytes[6] << 24);
            useChannel(this);
            if (key >= 1 && key <= 7) {
              forward = opcode === 0x03 ? pressed(key) : released(key);
            } else {
              forward = keyGoes(opcode === 0x03, key);
              if (forward) countKey(opcode === 0x03, key);
            }
          }
        }
      } catch (error) {
        forward = true;
      }
      if (!forward) return undefined;
      return nativeSend.apply(this, arguments);
    };
  }

  // ---- the keyboard, counted (which key it was is never kept past this function's own bookkeeping, nor told)
  function countKey(down, key) {
    var now = Date.now();
    if (down) {
      kbd.downs++;
      if (key === kbd.lastKey && now - kbd.lastUpAt < 70) {     // a machine's repeat (the client's own is every 50 ms)
        kbd.run++;
        if (kbd.run > kbd.longestRun) kbd.longestRun = kbd.run;
      } else {
        kbd.run = 0;
      }
      kbd.lastKey = key;
      if (!kbd.down[key]) {
        kbd.down[key] = setTimeout(function () {
          if (kbd.down[key]) { kbd.heldLong++; kbd.down[key] = null; }
        }, 3000);
      }
    } else {
      kbd.ups++;
      kbd.lastUpAt = now;
      if (kbd.down[key]) { clearTimeout(kbd.down[key]); kbd.down[key] = null; }
    }
  }

  // ---- v3: the room's browser's and window manager's own keys, kept from it
  // X keysyms. Modifiers: what this page has sent down and not up (the fallback), and what the page's own last key
  // event said was held (what is used: a modifier whose release was lost must not block a letter typed later).
  var MOD = { 0xffe1: "shift", 0xffe2: "shift", 0xffe3: "ctrl", 0xffe4: "ctrl", 0xffe9: "alt", 0xffea: "alt",
              0xfe03: "altgr", 0xffe7: "super", 0xffe8: "super", 0xffeb: "super", 0xffec: "super", 0xffed: "super",
              0xffee: "super" };
  var KS = { TAB: 0xff09, LEFT_TAB: 0xfe20, ESC: 0xff1b, DEL: 0xffff, SPACE: 0x20, PGUP: 0xff55, PGDN: 0xff56,
             LEFT: 0xff51, UP: 0xff52, RIGHT: 0xff53, DOWN: 0xff54, F1: 0xffbe, F12: 0xffc9 };
  var modsSent = {};          // keysym -> true
  var modsSeen = null;        // { ctrl, alt, shift, meta, at } from the page's last keydown / keypress / keyup
  var keptDown = {};          // keysym -> name, for a press kept back (its release is kept back too)
  var kept = { total: 0, byName: {}, told: {} };
  function noteMods(event) {
    modsSeen = { ctrl: !!event.ctrlKey, alt: !!event.altKey, shift: !!event.shiftKey, meta: !!event.metaKey, at: Date.now() };
  }
  window.addEventListener("keydown", noteMods, true);
  window.addEventListener("keypress", noteMods, true);
  window.addEventListener("keyup", noteMods, true);
  function modsNow() {
    if (modsSeen && Date.now() - modsSeen.at < 250) return modsSeen;
    var m = { ctrl: false, alt: false, shift: false, meta: false };
    Object.keys(modsSent).forEach(function (k) {
      var n = MOD[k];
      if (n === "ctrl") m.ctrl = true; else if (n === "alt") m.alt = true; else if (n === "shift") m.shift = true;
      else if (n === "super") m.meta = true;
    });
    return m;
  }
  function keyName(key) {
    if (key >= KS.F1 && key <= KS.F12) return "F" + (key - KS.F1 + 1);
    var names = {}; names[KS.TAB] = "Tab"; names[KS.LEFT_TAB] = "Tab"; names[KS.ESC] = "Esc"; names[KS.DEL] = "Delete";
    names[KS.SPACE] = "Space"; names[KS.PGUP] = "PageUp"; names[KS.PGDN] = "PageDown"; names[KS.LEFT] = "Left";
    names[KS.UP] = "Up"; names[KS.RIGHT] = "Right"; names[KS.DOWN] = "Down";
    if (names[key]) return names[key];
    if (key >= 0x21 && key <= 0x7e) return String.fromCharCode(key).toUpperCase();
    return "0x" + key.toString(16);
  }
  function combo(m, key) {
    return (m.ctrl ? "Ctrl+" : "") + (m.shift ? "Shift+" : "") + (m.alt ? "Alt+" : "") + keyName(key);
  }
  // [name, what it would have done] for a press the room's browser must not get, or null.
  function keptWhy(key, m) {
    if (key >= KS.F1 && key <= KS.F12) {
      var f = key - KS.F1 + 1;
      return ["F" + f, { 1: "Chrome's help over the television", 3: "find, which takes the keyboard", 4: "closes the window with Alt",
                         5: "reloads the page (the game's quick save)", 6: "moves the keyboard to the browser's frame",
                         7: "caret browsing's question, which takes the keyboard", 10: "the browser's menu",
                         11: "full screen", 12: "developer tools" }[f] || "a function key (the game's)"];
    }
    if (MOD[key] === "super") return ["the Windows key", "the window manager's"];
    var lower = key >= 0x41 && key <= 0x5a ? key + 32 : key;
    if (m.ctrl && !m.alt) {
      if (lower === 0x74 || lower === 0x6e || lower === 0x77) return [combo(m, key), "opens or closes a tab or a window"];
      if (key === KS.TAB || key === KS.LEFT_TAB || key === KS.PGUP || key === KS.PGDN) return [combo(m, key), "switches to a tab nobody sees"];
      if (!m.shift && key >= 0x31 && key <= 0x39) return [combo(m, key), "switches to a tab nobody sees"];
      if (!m.shift && (lower === 0x68 || lower === 0x6a || lower === 0x70)) return [combo(m, key), "opens Chrome's history, downloads or print"];
      if (m.shift && key === KS.DEL) return [combo(m, key), "opens Chrome's clear-data page"];
      if (m.shift && lower === 0x71) return [combo(m, key), "quits the browser"];
    }
    if (m.shift && !m.ctrl && !m.alt && key === KS.ESC) return ["Shift+Esc", "opens Chrome's task manager window"];
    if (m.alt && !m.ctrl && !m.shift && (key === KS.SPACE || key === KS.ESC || key === KS.TAB)) {
      return [combo(m, key), "the window manager's menu, or takes the focus away"];
    }
    if (m.alt && m.ctrl && key === KS.TAB) return [combo(m, key), "the window manager's window switch"];
    if (m.alt && (m.ctrl || m.shift) && key >= KS.LEFT && key <= KS.DOWN) return [combo(m, key), "switches to an empty desktop"];
    return null;
  }
  function keyGoes(down, key) {
    if (MOD[key] && MOD[key] !== "super") {
      if (down) modsSent[key] = true; else delete modsSent[key];
      return true;
    }
    if (!down) {
      if (keptDown[key]) { delete keptDown[key]; return false; }
      return true;
    }
    var why = keptWhy(key, modsNow());
    if (!why) return true;
    keptDown[key] = why[0];
    kept.total++;
    kept.byName[why[0]] = (kept.byName[why[0]] || 0) + 1;
    if (!kept.told[why[0]]) {
      kept.told[why[0]] = true;
      post("kept " + why[0] + " from the room's browser: " + why[1] + " (keys like it are the game's or the browser's " +
        "own; typing goes through)");
    }
    return false;
  }
  function keptText() {
    return Object.keys(kept.byName).map(function (n) { return n + " x" + kept.byName[n]; }).join(", ");
  }

  // ---- Home / Back: the room's browser told by key, the way a person would tell it
  var remote = null, remoteLabel = null, remoteTimer = null, lastPointerAt = 0, remoteBusy = false;
  function sendKeys(keys) {
    if (!channel || !nativeSend || channel.readyState !== "open") return false;
    try {
      for (var i = 0; i < keys.length; i++) nativeSend.call(channel, keyMessage(0x03, keys[i]));
      for (var j = keys.length - 1; j >= 0; j--) nativeSend.call(channel, keyMessage(0x04, keys[j]));
      return true;
    } catch (error) {
      return false;
    }
  }
  function haveControls() { return me !== "" && host === me; }
  function askControls() {
    // Who this viewer is comes with the connection (signal/provide); without it there is nothing to compare with, and
    // the keys are sent as a click would be: the server takes them from whoever holds the controls.
    if (haveControls() || me === "") return Promise.resolve(true);
    if (!clientSocket || clientSocket.readyState !== 1) return Promise.resolve(false);
    try { clientSocket.send(JSON.stringify({ event: "control/request" })); } catch (error) { return Promise.resolve(false); }
    var askedAt = Date.now();
    return new Promise(function (resolve) {
      (function wait() {
        if (haveControls()) { resolve(true); return; }
        if (Date.now() - askedAt >= CONTROL_WAIT_MS) { resolve(false); return; }
        setTimeout(wait, 50);
      })();
    });
  }
  function say(text, bad) {
    if (!remoteLabel) return;
    remoteLabel.textContent = text;
    remoteLabel.style.color = bad ? "#ff8a80" : "#9fe3b0";
    remoteShow(2500);
  }
  function press(name, keys) {
    if (remoteBusy) return;
    remoteBusy = true;
    var started = Date.now();
    askControls().then(function (ok) {
      if (!ok) {
        post(name.toLowerCase() + ": not sent -- another viewer holds the controls and did not let go within " +
          CONTROL_WAIT_MS + " ms");
        say("Another viewer has the controls", true);
        return;
      }
      if (!sendKeys(keys)) {
        post(name.toLowerCase() + ": not sent -- the client's input channel is " + (channel ? channel.readyState : "not open yet"));
        say("Not connected yet", true);
        return;
      }
      post(name.toLowerCase() + ": " + (name === "Home" ? "Alt+Home" : "Alt+Left") + " sent to the room's browser after " +
        (Date.now() - started) + " ms (" + (name === "Home" ? "its home page is the room's home screen" : "back one page") +
        "; the controls: " + whose() + ")");
      say(name === "Home" ? "Going home..." : "Back...", false);
    }).then(function () {
      remoteBusy = false;
      var overlay = document.querySelector("textarea.overlay");
      if (overlay) { try { overlay.focus(); } catch (error) { /* fine */ } }
    });
  }
  function remoteShow(ms) {
    if (!remote) return;
    var show = haveControls() || Date.now() - lastPointerAt < REMOTE_SHOW_MS || (ms && ms > 0);
    remote.style.opacity = show ? "1" : "0";
    remote.style.pointerEvents = show ? "auto" : "none";
    if (remoteTimer) clearTimeout(remoteTimer);
    if (show && !haveControls()) remoteTimer = setTimeout(function () { remoteTimer = null; remoteShow(); }, ms || REMOTE_SHOW_MS);
  }
  function makeRemote() {
    if (remote || !document.body) return;
    remote = document.createElement("div");
    remote.id = "open77-remote";
    remote.setAttribute("style", "position:fixed;left:14px;top:14px;z-index:2147483000;display:flex;gap:8px;align-items:center;" +
      "font:600 15px/1 system-ui,sans-serif;opacity:0;pointer-events:none;transition:opacity .2s;user-select:none");
    var buttons = [["Home", "\u2302  Home", [KEY_ALT, KEY_HOME]], ["Back", "\u2039  Back", [KEY_ALT, KEY_LEFT]]];
    buttons.forEach(function (b) {
      var el = document.createElement("button");
      el.type = "button";
      el.id = "open77-remote-" + b[0].toLowerCase();
      el.textContent = b[1];
      el.setAttribute("style", "border:0;border-radius:10px;padding:9px 14px;background:rgba(10,13,19,.78);color:#e8eef7;" +
        "cursor:pointer;box-shadow:0 2px 10px rgba(0,0,0,.45)");
      // Keep the focus (the keyboard) on the picture, and keep the press off it.
      el.addEventListener("mousedown", function (event) { event.preventDefault(); event.stopPropagation(); });
      el.addEventListener("mouseup", function (event) { event.stopPropagation(); });
      el.addEventListener("click", function (event) { event.preventDefault(); event.stopPropagation(); press(b[0], b[2]); });
      remote.appendChild(el);
    });
    remoteLabel = document.createElement("span");
    remoteLabel.setAttribute("style", "padding:0 6px;text-shadow:0 1px 3px #000");
    remote.appendChild(remoteLabel);
    document.body.appendChild(remote);
  }
  if (document.body) makeRemote(); else document.addEventListener("DOMContentLoaded", makeRemote);
  window.addEventListener("mousemove", function () {
    lastPointerAt = Date.now();
    remoteShow();
  }, true);

  // ---- the page's own events, in the capture phase (seen before the client's handler)
  window.addEventListener("mousedown", function (event) {
    lastDown = eventInfo(event);
  }, true);

  function onUp(event) {
    var info = eventInfo(event);
    lastUp = info;
    var key = event.button + 1;
    var click = held[key];
    if (click && !click.upEvent) click.upEvent = info;
    // The client's handler on the picture runs during this dispatch; what it did
    // not send by the next task, it is not going to send.
    setTimeout(function () {
      if (held[key] === click && click) {
        guardRelease(key, "the guard: the page's " + event.type + " came on " + info.target + " but the client sent no release",
          "byMouseup");
      }
    }, 0);
  }
  window.addEventListener("mouseup", onUp, true);
  window.addEventListener("pointerup", function (event) {
    if (event.pointerType && event.pointerType !== "mouse") return;
    onUp(event);
  }, true);

  window.addEventListener("mousemove", function (event) {
    var buttons = event.buttons;
    Object.keys(held).forEach(function (k) {
      var click = held[k];
      if (!click) return;
      click.moves++;
      var down = (buttons & (BIT[click.key] || 0)) !== 0;
      if (!down) click.movesUp++;
      if (!learned) return;
      if (down) { armHold(click); return; }
      if (Date.now() - click.at >= LEARNED_MOVE_MS) {
        guardRelease(click.key, "the guard: a move with the button up", "byMove");
      }
    });
  }, true);

  function releaseAll(why) {
    Object.keys(held).forEach(function (k) {
      if (held[k]) guardRelease(held[k].key, "the guard: " + why, "byFocus");
    });
  }
  window.addEventListener("blur", function () {
    noteAll("blur");
    releaseAll("this page lost the focus with the button held");
  });
  window.addEventListener("focus", function () { noteAll("focus"); });
  window.addEventListener("resize", function () { noteAll("resize " + window.innerWidth + "x" + window.innerHeight); });
  document.addEventListener("visibilitychange", function () {
    if (document.visibilityState === "hidden") releaseAll("this page was hidden with the button held");
  });
  window.addEventListener("pagehide", function () { releaseAll("this page went away with the button held"); });
  document.addEventListener("mouseleave", function () { noteAll("pointer left the page"); });

  // ---- v3: how the picture arrives (WebRTC's own numbers; never an address)
  var STREAM_FIRST_MS = typeof window.__open77InputStreamFirstMs === "number" ? window.__open77InputStreamFirstMs : 10000;
  var STREAM_EVERY_MS = typeof window.__open77InputStreamMs === "number" ? window.__open77InputStreamMs : 60000;
  var SAMPLE_MS = typeof window.__open77InputSampleMs === "number" ? window.__open77InputSampleMs : 2000;
  var SLOW = window.__open77InputSlow || { rtt: 150, wait: 80, fps: 20, lost: 2 };     // a test page's own, or these
  var stream = { prev: null, win: null, firstAt: 0, lastToldAt: 0, slowAt: 0, told: 0, last: null };
  function freshWindow() {
    return { t0: Date.now(), frames: 0, bytes: 0, lost: 0, packets: 0, dropped: 0, freezes: 0, freezeS: 0,
             waitS: 0, waitN: 0, decodeS: 0, rttMax: 0, waitMax: 0, rttSum: 0, rttN: 0, fps: 0, size: "", path: "" };
  }
  function pick(report) {
    var video = null, transport = null, pair = null;
    report.forEach(function (s) {
      if (s.type === "inbound-rtp" && (s.kind === "video" || s.mediaType === "video")) {
        if (!video || (s.bytesReceived || 0) > (video.bytesReceived || 0)) video = s;
      } else if (s.type === "transport" && s.selectedCandidatePairId) {
        transport = s;
      }
    });
    if (transport) pair = report.get(transport.selectedCandidatePairId) || null;
    if (!pair) {
      report.forEach(function (s) {
        if (!pair && s.type === "candidate-pair" && s.state === "succeeded" && (s.selected || s.nominated)) pair = s;
      });
    }
    var local = pair ? report.get(pair.localCandidateId) : null, remoteC = pair ? report.get(pair.remoteCandidateId) : null;
    var path = local ? (local.protocol || "?") + " " + (local.candidateType || "?") +
      (local.candidateType === "relay" && local.relayProtocol ? " (relay over " + local.relayProtocol + ")" : "") +
      " -> " + (remoteC ? remoteC.candidateType || "?" : "?") : "";
    return { video: video, pair: pair, path: path };
  }
  function streamLine(w, why) {
    var secs = Math.max(0.001, (Date.now() - w.t0) / 1000);
    var fps = w.fps || w.frames / secs;
    var rtt = w.rttN ? Math.round(w.rttSum / w.rttN) : null;
    var wait = w.waitN ? Math.round(1000 * w.waitS / w.waitN) : null;
    var decode = w.frames ? Math.round(1000 * w.decodeS / w.frames) : null;
    var lostPct = w.packets + w.lost > 0 ? (100 * w.lost / (w.packets + w.lost)) : 0;
    var behind = rtt !== null && wait !== null ? Math.round(rtt / 2 + wait + (decode || 0)) : null;
    return "stream" + (why ? " (" + why + ")" : "") + ": " + (w.size || "?") + " at " + Math.round(fps) + " fps, " +
      (8 * w.bytes / secs / 1e6).toFixed(1) + " Mbit/s; round trip " + (rtt === null ? "?" : rtt + " ms") +
      (w.rttMax && rtt !== null && w.rttMax > rtt + 20 ? " (up to " + Math.round(w.rttMax) + ")" : "") +
      "; frames wait " + (wait === null ? "?" : wait + " ms") + " to be smoothed" +
      (w.waitMax && wait !== null && w.waitMax > wait + 20 ? " (up to " + Math.round(w.waitMax) + ")" : "") +
      ", " + (decode === null ? "?" : decode + " ms") + " to be decoded" +
      (behind === null ? "" : " -- the picture is about " + behind + " ms behind the server's screen, plus its capture") +
      "; lost " + lostPct.toFixed(1) + "%, dropped " + w.dropped + ", freezes " + w.freezes +
      (w.freezes ? " (" + w.freezeS.toFixed(1) + " s)" : "") + (w.path ? "; " + w.path : "");
  }
  function sample() {
    var live = pcs.filter(function (p) { return p && p.connectionState !== "closed" && typeof p.getStats === "function"; });
    if (!live.length) return;
    // The connection the picture comes in on: the one with the most video received.
    Promise.all(live.map(function (p) { return p.getStats().catch(function () { return null; }); })).then(function (reports) {
      var got = null;
      reports.forEach(function (report) {
        if (!report) return;
        var g = pick(report);
        if (g.video && (!got || (g.video.bytesReceived || 0) > (got.video.bytesReceived || 0))) got = g;
      });
      var v = got ? got.video : null;
      if (!v) return;
      var now = { t: Date.now(), frames: v.framesDecoded || 0, bytes: v.bytesReceived || 0, lost: v.packetsLost || 0,
                  packets: v.packetsReceived || 0, dropped: v.framesDropped || 0, freezes: v.freezeCount || 0,
                  freezeS: v.totalFreezesDuration || 0, waitS: v.jitterBufferDelay || 0, waitN: v.jitterBufferEmittedCount || 0,
                  decodeS: v.totalDecodeTime || 0 };
      var p = stream.prev;
      stream.prev = now;
      if (!p || now.frames < p.frames || now.bytes < p.bytes) return;      // the first sample, or a new connection
      if (!stream.win) { stream.win = freshWindow(); stream.firstAt = Date.now(); }
      var w = stream.win;
      w.frames += now.frames - p.frames; w.bytes += now.bytes - p.bytes; w.lost += Math.max(0, now.lost - p.lost);
      w.packets += now.packets - p.packets; w.dropped += now.dropped - p.dropped; w.freezes += now.freezes - p.freezes;
      w.freezeS += now.freezeS - p.freezeS; w.decodeS += now.decodeS - p.decodeS;
      var dWaitN = now.waitN - p.waitN, dWaitS = now.waitS - p.waitS;
      if (dWaitN > 0) {
        w.waitS += dWaitS; w.waitN += dWaitN;
        w.waitMax = Math.max(w.waitMax, 1000 * dWaitS / dWaitN);
      }
      if (got.pair && typeof got.pair.currentRoundTripTime === "number") {
        var rtt = 1000 * got.pair.currentRoundTripTime;
        w.rttSum += rtt; w.rttN++; w.rttMax = Math.max(w.rttMax, rtt);
      }
      if (typeof v.framesPerSecond === "number") w.fps = v.framesPerSecond;
      if (v.frameWidth) w.size = v.frameWidth + "x" + v.frameHeight;
      if (got.path) w.path = got.path;
      var t = Date.now();
      var stepRtt = got.pair && typeof got.pair.currentRoundTripTime === "number" ? 1000 * got.pair.currentRoundTripTime : 0;
      var stepWait = dWaitN > 0 ? 1000 * dWaitS / dWaitN : 0;
      var stepLost = (now.packets - p.packets) + Math.max(0, now.lost - p.lost) > 0
        ? 100 * Math.max(0, now.lost - p.lost) / ((now.packets - p.packets) + Math.max(0, now.lost - p.lost)) : 0;
      var stepFps = typeof v.framesPerSecond === "number" ? v.framesPerSecond : (now.frames - p.frames) / Math.max(0.001, (now.t - p.t) / 1000);
      var slow = [];
      if (stepRtt > SLOW.rtt) slow.push("round trip " + Math.round(stepRtt) + " ms");
      if (stepWait > SLOW.wait) slow.push("frames wait " + Math.round(stepWait) + " ms");
      if (stepFps < SLOW.fps && stepFps > 0) slow.push(Math.round(stepFps) + " fps");
      if (stepLost > SLOW.lost) slow.push(stepLost.toFixed(1) + "% lost");
      stream.last = { rtt: Math.round(stepRtt), wait: Math.round(stepWait), fps: Math.round(stepFps), lost: stepLost, path: got.path };
      if (slow.length && t - stream.slowAt >= 30000) {
        stream.slowAt = t;
        stream.told++;
        post("stream slow now: " + slow.join(", ") + (got.path ? " (" + got.path + ")" : ""));
      }
      var due = stream.lastToldAt ? t - stream.lastToldAt >= STREAM_EVERY_MS : t - stream.firstAt >= STREAM_FIRST_MS;
      if (due && w.frames > 0) {
        post(streamLine(w, stream.lastToldAt ? "" : "first " + Math.round((t - w.t0) / 1000) + " s"));
        stream.told++;
        stream.lastToldAt = t;
        stream.win = freshWindow();
      }
    }).catch(function () { /* numbers are best effort */ });
  }
  setInterval(sample, SAMPLE_MS);

  setInterval(function () {
    if (kbd.downs > 0 || kept.total > 0) {
      var keys = "keyboard: " + kbd.downs + " key press(es), " + kbd.ups + " release(s); held over 3 s " + kbd.heldLong +
        "; the longest run of repeats " + kbd.longestRun +
        (kept.total ? "; kept from the room's browser " + kept.total + " (" + keptText() + ")" : "");
      if (keys !== kbd.lastTold) { kbd.lastTold = keys; post(keys); }
    }
    if (stats.clicks === 0) return;
    var text = stats.clicks + " click(s): released by the client " + stats.byClient + ", by a guard " +
      (stats.byRepress + stats.byMouseup + stats.byMove + stats.byHold + stats.byFocus) + " (at the next press " + stats.byRepress +
      ", mouseup off the picture " + stats.byMouseup + ", move " + stats.byMove + ", hold " + stats.byHold + ", focus " + stats.byFocus +
      "), by neko after 10 s " + stats.byStale + "; still held at 3 s " + stats.stillHeld + "; late client releases dropped " +
      stats.lateDropped + "; the game's duplicate presses dropped " + stats.dupDropped + "; host changes " + hostChanges +
      " (now " + whose() + ")" + (learned ? "; learned" : "");
    if (text === lastStats) return;
    lastStats = text;
    post(text);
  }, SUMMARY_MS);

  post(VERSION + ": watching this page's clicks; guards on (the game's duplicate press dropped, release before a second " +
    "press, a mouseup off the picture, focus loss); Home / Back in the picture's corner; the browser's and window " +
    "manager's own keys (F1-F12, Ctrl+T/N/W/Tab, Shift+Esc, Alt+Space...) kept from it; the stream's numbers told" +
    (typeof nativeSend === "function" ? "" : " -- NO RTCDataChannel here, nothing watched"));

  window.__open77Input = function () {
    return { version: VERSION, learned: learned, held: Object.keys(held).map(Number), stats: JSON.parse(JSON.stringify(stats)),
             channel: channel ? channel.label : null, pos: { x: pos.x, y: pos.y }, me: me, host: host, hostChanges: hostChanges,
             told: told, lastDown: lastDown, lastUp: lastUp, uptimeMs: Date.now() - started, moves: moves,
             keyboard: { downs: kbd.downs, ups: kbd.ups, heldLong: kbd.heldLong, longestRun: kbd.longestRun,
                         kept: kept.total, keptByName: JSON.parse(JSON.stringify(kept.byName)),
                         keptHeld: Object.keys(keptDown).map(Number), modsSent: Object.keys(modsSent).map(Number) },
             stream: { peers: pcs.length, told: stream.told, last: stream.last },
             remote: remote ? { shown: remote.style.opacity === "1", label: remoteLabel.textContent } : null };
  };
})();
