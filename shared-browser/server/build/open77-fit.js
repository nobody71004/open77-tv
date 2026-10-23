// open77-fit.js -- put into the shared browser's client page (neko's index.html)
// by the Open77 image, ahead of the page's own scripts.
//
// The television framing this page hands it the WHOLE box, edge to edge, and the
// box is often not the stream's shape: the game's sets are 16:9, 21:9 (2.39) and
// 2.35, the room's Chromium renders 1280x720 (`NEKO_DESKTOP_SCREEN` in
// `server/up.sh`) so the stream is 16:9 for all of them, and a client drives the
// surface with the picture across its whole viewport, whose shape is that PC's
// monitor -- 5120x1440, 32:9, measured 2026-10-16.
//
// Five fills were measured on a television on 2026-10-16 and all five were
// rejected, in this order:
//
//   * leaving the rest black -- "black stripes down the sides", the first report;
//   * a BLURRED copy of the stream -- "the two side bars are blurs": fog where a
//     page's own chrome has to read as the picture;
//   * the same stream scaled to COVER the box and clipped to the bar -- "showing
//     another part of the video... double letters": the video again in the bar;
//   * the picture's own EDGE COLUMN, stretched across the bar -- "it is like
//     stretching the video": a one-pixel column stretched sideways is a horizontal
//     smear per row, and no tuning makes it a picture;
//   * and the picture's own edge COLOURS, dimmed, as a wash -- "the blur side bars
//     are back". A wash is still something drawn beside the picture.
//
// So there is only one answer left, and it is the one a player picked when asked
// directly ("crop to fill", 2026-10-16): THE PICTURE FILLS THE BOX. It is scaled up,
// keeping its shape, until it reaches every edge of the box, and what hangs over is
// cut off -- `object-fit: cover`, which is the browser's own uniform scale, so the
// picture is cropped and never stretched. Nothing is drawn beside it, because there
// is nothing beside it: no canvas, no second stream, no filter, no fill of any kind
// (`streams: 0` and no layer at all are checks in both tests).
//
// What it costs is honest and measured: a 16:9 picture in a 2.39 box loses 26 % of
// its height (13 % off the top and 13 % off the bottom), and on a 32:9 client's own
// surface it loses half of it. That was reported too ("the top of the browser's own
// window gone") -- and it is still what a screen with no bars means. An operator who
// would rather keep every pixel than fill the glass has the other answer in the
// history above; the fill that keeps the picture whole is the one with bars in it.
//
// The cover is applied as a CLASS, never as an inline style: the picture's own style
// attribute belongs to the page -- neko sizes that element itself -- so nothing here
// writes to it, and taking the class off restores the page's layout exactly. That is
// also how the answer is undone: the moment the box IS the picture's own shape (a
// 16:9 set), or the picture goes away, the class comes off and the page is left as
// its client laid it out.
//
// What was done is told to the television when it changes (kind `fit`, which the
// television logs as browser_ice) and exposed as `window.__open77Fit()` for the
// tests.
(function () {
  "use strict";
  if (!window.parent || window.parent === window) return;

  var EDGE = 1;                  // px: a gap this thin is the picture, not a bar
  var COVER_CLASS = "open77-cover";
  var style = null;              // this script's own stylesheet, made once
  var cover = null;              // the picture wearing the cover class, when one is
  var saidBox = "";              // the last box reported, so a line goes out per shape

  function size(rect) {
    return Math.round(rect.width) + "x" + Math.round(rect.height);
  }

  function post(text) {
    try {
      window.parent.postMessage({ open77SharedBrowser: 1, kind: "fit", mode: "auto",
        text: String(text).slice(0, 600) }, "*");
    } catch (error) { /* telling the television is best effort */ }
  }

  function isMedia(target) {
    return !!target && (target.tagName === "VIDEO" || target.tagName === "AUDIO");
  }

  // The picture: the page's own video, the one carrying the stream. This script adds
  // no video of its own -- it adds nothing at all -- so there is nothing of its own
  // to skip.
  function picture() {
    var elements = document.querySelectorAll("video");
    for (var i = 0; i < elements.length; i++) {
      var stream = elements[i].srcObject;
      if (stream && typeof stream.getVideoTracks === "function" && stream.getVideoTracks().length > 0) return elements[i];
    }
    return null;
  }

  // The cover, as one rule on one class. `!important` because the page sizes that
  // element from its own layout, and its inline style is not this script's to argue
  // with -- while the class, not the style attribute, is what is added and taken
  // away, so taking it away restores the page's own layout exactly, whatever it was.
  function styleSheet() {
    if (style !== null) return;
    style = document.createElement("style");
    style.setAttribute("data-open77-fit", "cover");
    style.textContent = "." + COVER_CLASS + "{" +
      "position:fixed!important;left:0!important;top:0!important;" +
      "width:100%!important;height:100%!important;max-width:none!important;" +
      "max-height:none!important;margin:0!important;object-fit:cover!important;" +
      "object-position:50% 50%!important;z-index:0!important}";
    (document.head || document.documentElement).appendChild(style);
  }

  function coverOn(video) {
    styleSheet();
    if (cover === video) return;
    coverOff();
    video.classList.add(COVER_CLASS);
    cover = video;
  }

  function coverOff() {
    if (cover === null) return;
    try { cover.classList.remove(COVER_CLASS); } catch (error) { /* a page that took the class away itself */ }
    cover = null;
  }

  // What the box is, against the picture's own shape: the picture's aspect, the box's,
  // and the fraction of the picture the cover will therefore cut off -- of its height
  // when the box is wider than the picture, of its width when the box is taller.
  function coverShape(video, box) {
    var width = video.videoWidth;
    var height = video.videoHeight;
    if (!(width > 0) || !(height > 0)) return null;
    var shape = width / height;
    var screen = box.width / box.height;
    return { picture: shape, screen: screen, loss: screen > shape ? 1 - shape / screen : 1 - screen / shape };
  }

  function apply(why) {
    var video = picture();
    if (video === null || !video.isConnected) {
      coverOff();
      return;
    }
    var box = { width: window.innerWidth, height: window.innerHeight };
    var shape = coverShape(video, box);
    // A picture that is not there yet has no shape: nothing is cropped on a box this
    // page cannot measure against it. The page is not touched either, so a stream
    // that is still arriving draws exactly as its own client drew it.
    if (shape === null || video.readyState < 1 || !(box.width > 1) || !(box.height > 1)) {
      coverOff();
      return;
    }
    // The gap the shape difference makes, in px of the box: this is what says "this
    // box is already the picture's own shape" (a 16:9 set, nothing to do) apart from
    // "this box is eight pixels off it", which is a crop of eight pixels.
    var gap = shape.loss * (shape.screen > shape.picture ? box.width : box.height);
    if (gap <= EDGE) {
      coverOff();
      var same = size(box) + " box, " + shape.picture.toFixed(2) + ":1 picture";
      if (same !== saidBox) {
        saidBox = same;
        post("picture " + video.videoWidth + "x" + video.videoHeight + " in a " + same +
          ": the picture's own shape, nothing to crop");
      }
      return;
    }
    coverOn(video);
    if (video.readyState < 2) return;
    var percent = Math.round(shape.loss * 100);
    var each = Math.round(shape.loss * 50);
    var down = shape.screen > shape.picture;
    var line = "picture " + video.videoWidth + "x" + video.videoHeight + " in a " + size(box) +
      " box (" + shape.screen.toFixed(2) + ":1): scaled up until it covers the screen, " + percent +
      "% of the picture's " + (down ? "height" : "width") + " cut from the " +
      (down ? "top and bottom, " + each + "% each" : "left and right, " + each + "% each") +
      ", nothing stretched and nothing beside it";
    if (line !== saidBox) {
      saidBox = line;
      post(line + " (" + why + ")");
    }
  }

  // The picture's shape is the client's business, so this listens for it appearing and
  // for it being rebuilt: a stream that starts, a source swapped, a player the client
  // re-renders after a reconnect -- and, every two seconds, a look that covers a swap
  // the page passes through no event for.
  ["loadedmetadata", "playing"].forEach(function (name) {
    document.addEventListener(name, function (event) {
      if (isMedia(event.target)) apply("started");
    }, true);
  });
  window.addEventListener("resize", function () { apply("resized"); });
  setInterval(function () { apply("kept"); }, 2000);

  // For the tests and the server's own check, never for the television.
  window.__open77Fit = function () {
    var video = picture();
    var box = { width: window.innerWidth, height: window.innerHeight };
    var shape = video === null ? null : coverShape(video, box);
    var rect = video === null ? null : video.getBoundingClientRect();
    return {
      mode: cover !== null ? "cover" : "none",
      fitted: cover !== null,
      box: Math.round(box.width) + "x" + Math.round(box.height),
      // With the cover on the picture IS the whole box (that is what the rule asks
      // for); without it, the box the picture's own element occupies.
      picture: rect === null ? null : size(rect),
      stream: video === null ? null : video.videoWidth + "x" + video.videoHeight,
      loss: shape === null ? null : Math.round(shape.loss * 1000) / 1000,
      cover: cover !== null,
      coverClass: video !== null && video.classList.contains(COVER_CLASS),
      // Kept in the shape the tests and the log have always read, and empty or zero
      // because there is nothing beside the picture any more: no layer, no bars, no
      // second stream, no filter.
      sides: [],
      gaps: null,
      bars: 0,
      streams: 0,
      layer: document.getElementById("open77-fit"),
      line: saidBox,
    };
  };
})();
