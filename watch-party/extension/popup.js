// Open77 Watch Party -- popup.js: shows the party this browser follows, and lets
// the viewer type a code (and key) by hand, change the server, or leave.
"use strict";

var DEFAULT_BASE = "https://xbuniverse.duckdns.org/opx-watch-xb-staging";
var $ = function (id) { return document.getElementById(id); };

function load() {
  chrome.storage.local.get(["settings", "lastStatus"], function (got) {
    var s = got.settings || {};
    $("code").value = s.code || "";
    $("key").value = s.key || "";
    $("base").value = s.base || DEFAULT_BASE;
    var st = got.lastStatus;
    var box = $("status");
    if (st && st.code && st.code === s.code) {
      box.textContent = st.text + (st.keyed ? "" : " (following only)");
      box.className = "status " + (st.kind || "");
    } else if (s.code) {
      box.textContent = "Party " + s.code + ": open a Netflix tab to join in.";
      box.className = "status";
    } else {
      box.textContent = "No party yet.";
      box.className = "status";
    }
  });
}

$("save").addEventListener("click", function () {
  var code = $("code").value.trim().toUpperCase();
  var key = $("key").value.trim().toLowerCase();
  var base = $("base").value.trim().replace(/\/+$/, "") || DEFAULT_BASE;
  if (code && !/^[A-Z2-9]{6}$/.test(code)) { $("status").textContent = "A party code is six letters and digits."; return; }
  if (key && !/^[a-z2-9]{12}$/.test(key)) { $("status").textContent = "That key does not look right: copy your link again in game."; return; }
  if (!/^https:\/\/xbuniverse\.duckdns\.org\//.test(base + "/")) {
    $("status").textContent = "This build only talks to https://xbuniverse.duckdns.org/.";
    return;
  }
  chrome.storage.local.get("settings", function (got) {
    var s = got.settings || {};
    s.code = code; s.key = key; s.base = base; s.enabled = true;
    chrome.storage.local.set({ settings: s }, load);
  });
});

$("leave").addEventListener("click", function () {
  chrome.storage.local.get("settings", function (got) {
    var s = got.settings || {};
    s.code = ""; s.key = "";
    chrome.storage.local.set({ settings: s }, load);
  });
});

load();
