// Open77 Watch Party -- background.js
//
// The extension's one network door. The content script asks; this fetches from
// the watch party server (and only from the hosts this extension was granted in
// manifest.json), so neither Netflix's page policy nor cross-origin rules stand
// between a viewer and their party.
"use strict";

var ALLOWED = /^https:\/\/xbuniverse\.duckdns\.org\//;

chrome.runtime.onMessage.addListener(function (msg, sender, sendResponse) {
  if (!msg || msg.type !== "opx-fetch") return false;
  if (typeof msg.url !== "string" || !ALLOWED.test(msg.url)) {
    sendResponse({ ok: false, error: "server_not_allowed" });
    return false;
  }
  var init = { method: msg.method === "POST" ? "POST" : "GET", cache: "no-store", credentials: "omit" };
  if (init.method === "POST") {
    // text/plain keeps it a simple request: no preflight.
    init.headers = { "Content-Type": "text/plain;charset=UTF-8" };
    init.body = typeof msg.body === "string" ? msg.body : "";
  }
  var controller = new AbortController();
  var timer = setTimeout(function () { controller.abort(); }, 5000);
  init.signal = controller.signal;
  fetch(msg.url, init).then(function (r) {
    return r.text().then(function (text) {
      clearTimeout(timer);
      sendResponse({ ok: true, status: r.status, text: text });
    });
  }).catch(function (e) {
    clearTimeout(timer);
    sendResponse({ ok: false, error: String(e && e.message || e) });
  });
  return true; // answered asynchronously
});
