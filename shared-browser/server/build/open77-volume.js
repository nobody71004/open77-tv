// open77-volume.js -- put into the shared browser's client page (neko's
// index.html) by the Open77 image, ahead of the page's own scripts.
//
// The television's volume and mute, on the picture's sound. The television
// (open77_media's tv.js, the page framing this one) cannot reach into this page,
// so it posts its level here -- whenever the level changes, and once this page has
// loaded -- and this puts it on the stream's media element.
//
// The client keeps a volume of its own: saved in the browser, put on the element
// when its player mounts, moved by its own slider, and copied back from the
// element whenever the element's level changes. So the television's level is put
// on the element and put back whenever anything else moves it, and the client
// then keeps the television's level as its own.
//
// Only the framing television is listened to, and only for this. What was applied
// is told back to it when that changes (kind `volume`, which the television logs
// as browser_ice): a level and a count of elements, nothing else.
(function () {
  "use strict";
  if (!window.parent || window.parent === window) return;

  var wanted = null; // { volume: 0..1, muted: boolean }
  var told = "";

  function post(text) {
    try {
      window.parent.postMessage({ open77SharedBrowser: 1, kind: "volume", mode: "auto",
        text: String(text).slice(0, 600) }, "*");
    } catch (error) { /* telling the television is best effort */ }
  }

  function isMedia(target) {
    return !!target && (target.tagName === "VIDEO" || target.tagName === "AUDIO");
  }

  function apply(why) {
    if (wanted === null) return;
    var elements = document.querySelectorAll("video, audio");
    for (var i = 0; i < elements.length; i++) {
      var element = elements[i];
      if (Math.abs(element.volume - wanted.volume) > 0.001) element.volume = wanted.volume;
      if (element.muted !== wanted.muted) element.muted = wanted.muted;
    }
    var line = Math.round(wanted.volume * 100) + (wanted.muted ? " muted" : "") + " on " +
      elements.length + (elements.length === 1 ? " element" : " elements");
    if (line !== told) {
      told = line;
      post(line + " (" + why + ")");
    }
  }

  window.addEventListener("message", function (event) {
    if (event.source !== window.parent) return;
    var data = event.data;
    if (!data || typeof data !== "object" || data.open77SharedBrowserVolume !== 1) return;
    var volume = Number(data.volume);
    if (!isFinite(volume)) return;
    wanted = { volume: Math.max(0, Math.min(1, volume)), muted: data.muted === true };
    apply("asked");
  });

  // Media events do not bubble, but they pass through the document on their way
  // to the element: whatever moved an element's level, the television's goes back.
  document.addEventListener("volumechange", function (event) {
    if (wanted === null || !isMedia(event.target)) return;
    var element = event.target;
    if (Math.abs(element.volume - wanted.volume) > 0.001 || element.muted !== wanted.muted) apply("kept");
  }, true);

  // An element the client makes after the level came gets it as it starts.
  ["loadedmetadata", "playing"].forEach(function (name) {
    document.addEventListener(name, function (event) {
      if (isMedia(event.target)) apply("started");
    }, true);
  });
})();
