// open77-paste.js -- put into the shared browser's client page (neko's
// index.html) by the Open77 image, ahead of the page's own scripts.
//
// Pasting into the shared browser: copy a link on this PC, click into the
// browser on the television, Ctrl+V, and it is in the field that has the focus
// over there -- the address bar, a search box.
//
// Without this a paste cannot work in the game. The client page plays every key
// to the server's Chromium, Ctrl+V included, and that pastes the SERVER's
// clipboard. The client's own way of carrying this PC's clipboard across reads it
// with navigator.clipboard.readText(), a permission the game's browser never
// grants. So a paste is done in two steps here:
//
//   1. Ctrl+V (or Shift+Insert) on the picture is kept from the client's key
//      handler, so this page's own browser pastes instead -- and that `paste`
//      event carries this PC's clipboard text, with no permission needed.
//   2. The text goes to the server as the shared browser's clipboard (the
//      client's own `control/clipboard` message, on the client's own socket),
//      and Ctrl+V is then played to the server's Chromium through the client's
//      own key handler, so it pastes that text.
//
// This PC's clipboard is read only when its player pastes, and only that paste's
// text is sent, to the server the page came from.
//
// What happened is said twice: on the picture, to the player who pasted (a small
// note only their own screen shows), and to the television framing the page,
// which logs it (`browser_ice (paste ...)`): which key, on what, whether a paste
// followed and how long it was -- never the text itself. A right-click on the
// picture opens the SERVER browser's menu, whose Paste is the server's clipboard,
// so the note says to use Ctrl+V instead.
(function () {
  "use strict";
  var NativeSocket = window.WebSocket;
  if (typeof NativeSocket !== "function") return;

  var MAX_TEXT = 8192;
  // Long enough for the server to have set its clipboard before the key that
  // pastes it arrives (both go over the same socket, in order, but the server
  // hands the clipboard to the display asynchronously).
  var SETTLE_MS = typeof window.__open77PasteSettleMs === "number" ? window.__open77PasteSettleMs : 150;

  // ---- telling the player, and the television
  function report(text) {
    try {
      if (window.parent && window.parent !== window) {
        window.parent.postMessage({ open77SharedBrowser: 1, kind: "paste", mode: "auto", text: String(text).slice(0, 300) }, "*");
      }
    } catch (error) { /* telling the television is best effort */ }
  }
  var note = null;
  var noteTimer = null;
  function show(text, ms) {
    try {
      if (note === null) {
        note = document.createElement("div");
        note.setAttribute("style", "position:fixed;left:50%;top:14px;transform:translateX(-50%);z-index:2147483647;" +
          "pointer-events:none;max-width:80%;padding:9px 16px;border-radius:8px;background:rgba(10,14,22,.88);" +
          "color:#fff;font:600 17px/1.35 system-ui,sans-serif;box-shadow:0 2px 12px rgba(0,0,0,.5);text-align:center");
      }
      note.textContent = text;
      if (!note.isConnected) (document.body || document.documentElement).appendChild(note);
      if (noteTimer !== null) clearTimeout(noteTimer);
      noteTimer = setTimeout(function () { if (note && note.isConnected) note.remove(); }, ms || 3500);
    } catch (error) { /* a note is a courtesy */ }
  }
  function describe(target) {
    if (!target || !target.tagName) return "nothing";
    var name = target.tagName.toLowerCase();
    if (target.id) name += "#" + target.id;
    if (target.classList && target.classList.length) name += "." + Array.prototype.slice.call(target.classList, 0, 2).join(".");
    return name;
  }

  // The client's socket: the one it opens to its server's /ws.
  var socket = null;
  function isClientSocket(url) {
    try {
      return /\/(api\/)?ws$/.test(new URL(String(url), location.href).pathname);
    } catch (error) {
      return false;
    }
  }
  function Watched(url, protocols) {
    var ws = protocols === undefined ? new NativeSocket(url) : new NativeSocket(url, protocols);
    if (isClientSocket(url)) socket = ws;
    return ws;
  }
  Watched.prototype = NativeSocket.prototype;
  ["CONNECTING", "OPEN", "CLOSING", "CLOSED"].forEach(function (name) { Watched[name] = NativeSocket[name]; });
  window.WebSocket = Watched;

  // The picture: the client's transparent text field over the video, which takes
  // the keys and the clicks.
  function isPicture(target) {
    return !!(target && target.tagName === "TEXTAREA" && target.classList && target.classList.contains("overlay"));
  }
  function isPasteKey(event) {
    var key = String(event.key || "").toLowerCase();
    if ((event.ctrlKey || event.metaKey) && !event.altKey && (key === "v" || event.code === "KeyV")) return true;
    return event.shiftKey && !event.ctrlKey && !event.altKey && (key === "insert" || event.code === "Insert");
  }
  function socketOpen() { return socket !== null && socket.readyState === 1; }

  var ctrlHeld = false;
  var keptAt = 0;      // when a paste key was kept from the client
  var keptCode = "";   // which key, so its release is kept too

  var pasteWatch = null;
  window.addEventListener("keydown", function (event) {
    if (!event.isTrusted) return;
    if (event.key === "Control") ctrlHeld = true;
    if (!isPasteKey(event)) return;
    var which = event.shiftKey && !event.ctrlKey ? "Shift+Insert" : "Ctrl+V";
    if (!isPicture(event.target)) {
      report(which + " on " + describe(event.target) + ", not the picture: left to the page");
      return;
    }
    if (!socketOpen()) {
      report(which + " with the client's socket " + (socket === null ? "not found" : "in state " + socket.readyState) +
        ": left to the client (the server pastes its own clipboard)");
      return;
    }
    // Not cancelled: the browser's own paste follows, and it is what reads the
    // clipboard. Only the client's key handler is kept out of it.
    event.stopImmediatePropagation();
    keptAt = Date.now();
    keptCode = event.code || String(event.key || "").toLowerCase();
    if (pasteWatch !== null) clearTimeout(pasteWatch);
    pasteWatch = setTimeout(function () {
      pasteWatch = null;
      if (keptAt === 0) return;
      keptAt = 0;
      report(which + " kept from the client, but no paste followed in 800 ms: this browser did not paste");
      show("Paste did not work here -- copy the link again, click the field, then Ctrl+V", 5000);
    }, 800);
  }, true);
  window.addEventListener("keypress", function (event) {
    if (event.isTrusted && keptAt !== 0 && isPicture(event.target) && isPasteKey(event)) event.stopImmediatePropagation();
  }, true);
  window.addEventListener("keyup", function (event) {
    if (!event.isTrusted) return;
    if (event.key === "Control") ctrlHeld = false;
    var code = event.code || String(event.key || "").toLowerCase();
    if (keptCode !== "" && code === keptCode) {
      keptCode = "";
      event.stopImmediatePropagation();
    }
  }, true);

  function key(target, type, init, keyCode) {
    var event = new KeyboardEvent(type, init);
    try {
      Object.defineProperty(event, "keyCode", { get: function () { return keyCode; } });
      Object.defineProperty(event, "which", { get: function () { return keyCode; } });
    } catch (error) { /* the key's name is enough for the client */ }
    target.dispatchEvent(event);
  }
  // Ctrl+V to the server's Chromium, through the client's own key handler. Ctrl
  // is pressed and released here only if the player has already let go of it:
  // while it is held, the server has it held too.
  function playPaste(target) {
    var held = ctrlHeld;
    var control = { key: "Control", code: "ControlLeft", location: 1, bubbles: true, cancelable: true };
    var v = { key: "v", code: "KeyV", bubbles: true, cancelable: true, ctrlKey: true };
    if (!held) key(target, "keydown", Object.assign({ ctrlKey: true }, control), 17);
    key(target, "keydown", v, 86);
    key(target, "keyup", v, 86);
    if (!held) key(target, "keyup", Object.assign({ ctrlKey: false }, control), 17);
  }

  window.addEventListener("paste", function (event) {
    if (!event.isTrusted || !isPicture(event.target)) return;
    // Never into the hidden field that takes the keys.
    event.preventDefault();
    event.stopImmediatePropagation();
    if (pasteWatch !== null) { clearTimeout(pasteWatch); pasteWatch = null; }
    var kept = keptAt !== 0 && Date.now() - keptAt < 2000;
    keptAt = 0;
    var text = event.clipboardData ? String(event.clipboardData.getData("text/plain") || "") : "";
    if (!kept) { report("a paste on the picture that was not Ctrl+V: not sent"); return; }
    if (text === "") {
      report("Ctrl+V: this PC's clipboard holds no text");
      show("Nothing to paste: copy a link on this PC first", 4000);
      return;
    }
    if (!socketOpen()) { report("Ctrl+V: the client's socket closed before the paste"); return; }
    if (text.length > MAX_TEXT) text = text.slice(0, MAX_TEXT);
    socket.send(JSON.stringify({ event: "control/clipboard", text: text }));
    var target = event.target;
    setTimeout(function () { playPaste(target); }, SETTLE_MS);
    report("Ctrl+V: " + text.length + " characters sent to the shared browser and pasted there");
    show("Pasted from this PC", 2000);
  }, true);

  // A right-click opens the server browser's own menu: its Paste is the server's
  // clipboard, not this PC's. Said once in a while, not on every click.
  var hintedAt = 0;
  window.addEventListener("mousedown", function (event) {
    if (!event.isTrusted || event.button !== 2 || !isPicture(event.target)) return;
    if (Date.now() - hintedAt < 30000) return;
    hintedAt = Date.now();
    show("To paste a link from this PC: click the address bar, then press Ctrl+V", 5000);
    report("right-click on the picture: told to use Ctrl+V");
  }, true);
})();
