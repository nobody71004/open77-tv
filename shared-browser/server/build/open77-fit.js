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
// So the box and the picture disagree, and every answer has now been measured on
// a television:
//
//   * Cropping the picture to the box costs the picture: on a 32:9 client that took
//     half of its height, the browser's own toolbar and the top of the page gone.
//   * Leaving the rest black is what the bars were -- black stripes down the sides
//     of the set, the first thing reported.
//   * A blurred copy of the stream is fog: a player reported the bars as blurs, and
//     a page's own chrome or a film's letterbox cannot be fog.
//   * The same stream scaled to COVER the box and clipped to the bar was worse than
//     fog: it is the video again, so a player saw the frame's content a second time
//     in the bars -- "showing another part of the video... double letters".
//   * The picture's own edge column, stretched across the bar (that afternoon), was
//     the fourth: a one-pixel column has no structure of its own, so stretching it
//     sideways drew a horizontal smear per row -- "it is like stretching the video"
//     (a screenshot of the cinema panel, 2026-10-16).
//
// What is here instead, and there are only two answers because there are only two
// ways this ends well:
//
//   * COVER, where cropping is cheap. The picture is scaled up, keeping its shape,
//     until it reaches every edge of the box, and what hangs over is cut off: the
//     screen is the film and nothing else, no smear and no second copy. It costs
//     the picture a fraction of its own short side -- 26 % of its height on a 2.39
//     cinema screen from a 16:9 stream, 13 % off the top and 13 % off the bottom --
//     and that is the price a player chose over a stripe down the side (2026-10-16).
//   * A GLOW, where cropping is not cheap. Past a third of the picture this script
//     will not cut it up: the picture is left exactly as the client laid it out and
//     each gap carries the picture's own edge COLOURS, averaged into a few stops and
//     drawn as a soft wash that follows the rows (or columns) of the picture beside
//     it. It is a glow behind the picture: no structure, so nothing can look like a
//     stretched picture; dimmed, so it cannot be mistaken for the picture itself;
//     and drawn from the picture's own element, so it is never a second stream. The
//     32:9 client surface -- where covering would take half the picture -- is the
//     case this is for.
//
// Where the layer goes is the half that took the measuring, and it is unchanged: the
// layer is inserted immediately BEFORE the picture, inside the picture's own box. A
// child paints over its ancestors' backgrounds but under what comes after it in the
// page's own order, so a layer before the picture is above every black the client
// paints around it (div.player's own #000) and below the picture itself -- a layer
// one ancestor further out sits under the black of the box that shows the bars and
// paints nothing at all, which `test/fit-local.mjs` measures.
//
// The cover is a class, not an inline style: the picture's own style attribute
// belongs to the page (neko sizes that element itself), so nothing here writes to it.
// The class is added and taken off as the box changes shape and is gone the moment
// the box is the picture's own shape, or the picture goes away.
//
// What was done is told to the television when it changes (kind `fit`, which the
// television logs as browser_ice), and exposed as `window.__open77Fit()` for the
// tests.
(function () {
  "use strict";
  if (!window.parent || window.parent === window) return;

  var EDGE = 1;            // px: a gap this thin is the picture, not a bar
  var CROP_LIMIT = 1 / 3;  // of the picture's own short side: the most a cover may cost
  var COVER_CLASS = "open77-cover";
  var GLOW_STOPS = 6;      // colours the wash is built from, down (or along) the edge
  var GLOW_STRIP = 12;     // px of the picture's own edge each stop is averaged over
  var GLOW_DIM = 0.6;      // the wash is a glow, not the picture: it is dimmed
  var GLOW_FPS = 20;       // it follows the frame; it does not chase it

  var style = null;        // this script's own stylesheet, made once
  var cover = null;        // the picture wearing the cover class, when one is
  var layer = null;        // { element, bars, picture, stream, box, sides, gaps, key, line, timer }
  var scratch = {};        // the tiny canvases the wash is averaged into
  var saidWhole = "";      // the last "the box is the picture" shape reported

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
  // no video of its own (its bars are canvases), so there is nothing of its own to
  // skip.
  function picture() {
    var elements = document.querySelectorAll("video");
    for (var i = 0; i < elements.length; i++) {
      var stream = elements[i].srcObject;
      if (stream && typeof stream.getVideoTracks === "function" && stream.getVideoTracks().length > 0) return elements[i];
    }
    return null;
  }

  // The cover, as one rule on one class. `!important` because the page sizes that
  // element from its own layout and its inline style is not this script's to argue
  // with -- and the class, not the style attribute, is what is added and taken away,
  // so taking it away restores the page's own layout exactly, whatever it was.
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

  // What the box has that the picture's own shape does not, as the fraction of the
  // picture the other answer would cut off: of its height when the box is wider than
  // the picture, of its width when the box is taller.
  function coverShape(video, box) {
    var width = video.videoWidth;
    var height = video.videoHeight;
    if (!(width > 0) || !(height > 0)) return null;
    var shape = width / height;
    var screen = box.width / box.height;
    return { picture: shape, screen: screen, loss: screen > shape ? 1 - shape / screen : 1 - screen / shape };
  }

  // A value in `object-position`, as px of the room it moves through: a percentage
  // of that room (the standard's own rule), one of the keywords, or a length.
  function offset(value, room) {
    if (value === undefined || value === "center") return room / 2;
    if (value === "left" || value === "top") return 0;
    if (value === "right" || value === "bottom") return room;
    if (value.slice(-1) === "%") return parseFloat(value) / 100 * room;
    var px = parseFloat(value);
    return isNaN(px) ? room / 2 : Math.min(Math.max(px, 0), room);
  }

  // The picture's own rectangle, which is not always the element's box: an element
  // larger than the stream's shape letterboxes it inside itself (`object-fit:
  // contain`), and the bars this script exists for live between the two. Read from
  // the page's computed styles, because in the glow case the picture is exactly as
  // the client laid it out and that is where it is. The cover case never asks.
  function painted(video) {
    var element = video.getBoundingClientRect();
    var width = video.videoWidth;
    var height = video.videoHeight;
    if (!(width > 0) || !(height > 0)) return element;
    var style_ = getComputedStyle(video);
    var fit = style_.objectFit;
    if (fit === "fill") return element;
    var scale = fit === "cover" ? Math.max(element.width / width, element.height / height)
      : fit === "none" ? 1
        : Math.min(element.width / width, element.height / height);
    var shown = { width: width * scale, height: height * scale };
    var at = (style_.objectPosition || "50% 50%").split(/\s+/);
    return {
      left: element.left + offset(at[0], element.width - shown.width),
      top: element.top + offset(at[1], element.height - shown.height),
      width: shown.width,
      height: shown.height,
    };
  }

  // What the box has that the picture does not, per side, in px.
  function gapsFor(box, inner) {
    var whole = function (n) { return Math.max(0, Math.round(n)); };
    return {
      left: whole(inner.left),
      right: whole(box.width - (inner.left + inner.width)),
      top: whole(inner.top),
      bottom: whole(box.height - (inner.top + inner.height)),
    };
  }

  function sidesOf(gaps) {
    return ["left", "right", "top", "bottom"].filter(function (side) { return gaps[side] > EDGE; });
  }

  // Where a bar goes, in the box's own coordinates: exactly the gap, so no clipping
  // is needed -- the bar IS the gap. The side bars run the whole way down (or up) the
  // box, which is what makes a corner belong to them, and the top and bottom bars sit
  // between them.
  function place(side, gaps, box) {
    if (side === "left") return { left: 0, top: 0, width: gaps.left, height: box.height };
    if (side === "right") return { left: box.width - gaps.right, top: 0, width: gaps.right, height: box.height };
    if (side === "top") return { left: gaps.left, top: 0, width: box.width - gaps.left - gaps.right, height: gaps.top };
    return { left: gaps.left, top: box.height - gaps.bottom, width: box.width - gaps.left - gaps.right, height: gaps.bottom };
  }

  function drop() {
    if (layer === null) return;
    if (layer.timer !== null) clearInterval(layer.timer);
    if (layer.element.parentNode) layer.element.parentNode.removeChild(layer.element);
    layer = null;
  }

  // The picture's own edge, averaged into the few colours the wash is made of: a
  // strip of the picture's edge, drawn small. Drawing it small is the averaging --
  // the browser does it, and this script never reads the picture a pixel at a time.
  // A side bar reads the column at the picture's left (or right) edge, a top or
  // bottom one the row at its top (or bottom); `near` is that end.
  function stops(video, side) {
    var width = video.videoWidth;
    var height = video.videoHeight;
    var column = side === "left" || side === "right";
    var key = column ? "column" : "row";
    var canvas = scratch[key];
    if (canvas === undefined) {
      canvas = document.createElement("canvas");
      canvas.width = column ? 1 : GLOW_STOPS;
      canvas.height = column ? GLOW_STOPS : 1;
      scratch[key] = canvas;
    }
    var context = canvas.getContext("2d", { willReadFrequently: true });
    var near = side === "left" || side === "top";
    context.imageSmoothingEnabled = true;
    context.clearRect(0, 0, canvas.width, canvas.height);
    if (column) {
      var x = near ? 0 : Math.max(0, width - GLOW_STRIP);
      context.drawImage(video, x, 0, Math.min(GLOW_STRIP, width), height, 0, 0, 1, GLOW_STOPS);
    } else {
      var y = near ? 0 : Math.max(0, height - GLOW_STRIP);
      context.drawImage(video, 0, y, width, Math.min(GLOW_STRIP, height), 0, 0, GLOW_STOPS, 1);
    }
    return context.getImageData(0, 0, canvas.width, canvas.height).data;
  }

  function stop(data, index, count) {
    var at = Math.min(count - 1, Math.max(0, index)) * 4;
    return "rgb(" + Math.round(data[at] * GLOW_DIM) + "," + Math.round(data[at + 1] * GLOW_DIM) +
      "," + Math.round(data[at + 2] * GLOW_DIM) + ")";
  }

  // Punches out one bar: the picture's own edge colours at the picture's own rows (or
  // columns), drawn across the bar as a smooth wash. The middle band is where the
  // picture is, so the wash and the picture meet row for row and line up at the seam;
  // the two outer bands (only present when the box is taller, or wider, than the
  // picture as well) carry the edge's own first or last colour to the end of the box.
  function draw(bar, video, inner) {
    if (video.videoWidth < 1 || video.videoHeight < 1 || video.readyState < 2) return;
    var canvas = bar.element;
    var context = bar.context;
    try {
      var data = stops(video, bar.side);
      context.setTransform(bar.ratio, 0, 0, bar.ratio, 0, 0);
      context.clearRect(0, 0, canvas.width, canvas.height);
      var row = bar.side === "top" || bar.side === "bottom";
      // The picture's own rows (for a side bar) or columns (for a top or bottom one),
      // as this bar's coordinates: the bar is offset by the gaps around it.
      var span = row ? canvas.width : canvas.height;
      var from = Math.round(row ? inner.left - bar.place.left : inner.top);
      var to = Math.round(row ? inner.left + inner.width - bar.place.left : inner.top + inner.height);
      var start = Math.max(0, from);
      var end = Math.min(span, to);
      if (end <= start) return;
      var gradient = row ? context.createLinearGradient(start, 0, end, 0)
        : context.createLinearGradient(0, start, 0, end);
      for (var i = 0; i < GLOW_STOPS; i++) {
        gradient.addColorStop(i / (GLOW_STOPS - 1), stop(data, i, GLOW_STOPS));
      }
      if (start > 0) {
        context.fillStyle = stop(data, 0, GLOW_STOPS);
        if (row) context.fillRect(0, 0, start, canvas.height);
        else context.fillRect(0, 0, canvas.width, start);
      }
      if (end < span) {
        context.fillStyle = stop(data, GLOW_STOPS - 1, GLOW_STOPS);
        if (row) context.fillRect(end, 0, span - end, canvas.height);
        else context.fillRect(0, end, canvas.width, span - end);
      }
      context.fillStyle = gradient;
      if (row) context.fillRect(start, 0, end - start, canvas.height);
      else context.fillRect(0, start, canvas.width, end - start);
    } catch (error) { /* a frame that is not there yet draws nothing */ }
  }

  function render() {
    if (layer === null) return;
    var video = layer.picture;
    if (video === null || video.readyState < 2) return;
    var inner = painted(video);
    for (var i = 0; i < layer.bars.length; i++) draw(layer.bars[i], video, inner);
  }

  function make(video, stream, gaps, sides, box) {
    var element = document.createElement("div");
    element.id = "open77-fit";
    element.setAttribute("aria-hidden", "true");
    // `fixed`: the viewport IS the box this page is given, and the picture's own box
    // is not (neko's player box is the picture's size, the container around it is
    // neither) -- so the layer is sized to the viewport rather than to whichever of
    // them it would have to be inserted into.
    element.style.cssText = "position:fixed;inset:0;overflow:hidden;display:block;" +
      "z-index:0;pointer-events:none";
    var ratio = Math.min(2, window.devicePixelRatio || 1);
    var bars = [];
    sides.forEach(function (side) {
      var where = place(side, gaps, box);
      var canvas = document.createElement("canvas");
      canvas.setAttribute("data-open77-fit", side);
      canvas.setAttribute("aria-hidden", "true");
      canvas.width = Math.max(1, Math.round(where.width * ratio));
      canvas.height = Math.max(1, Math.round(where.height * ratio));
      canvas.style.cssText = "position:absolute;display:block;pointer-events:none;background:none;" +
        "left:" + where.left + "px;top:" + where.top + "px;" +
        "width:" + where.width + "px;height:" + where.height + "px";
      element.appendChild(canvas);
      bars.push({ side: side, element: canvas, context: canvas.getContext("2d"),
        place: where, ratio: ratio });
    });
    // Immediately before the picture, inside the box the picture is in: above the
    // background that shows the bars, below the picture.
    var parent = video.parentElement || document.body;
    parent.insertBefore(element, video);
    // The layer is positioned and an unpositioned picture is not: a positioned
    // element paints over one that is not, whatever the page's order says, which
    // would be the bars OVER the picture instead of beside it. neko's own client
    // positions it (`.player .player-container video { position: absolute }`,
    // measured 2026-10-16), so this is a guard -- and one that changes nothing about
    // the picture: a relative element with no offsets is laid out and drawn exactly
    // where it was. Deliberately no z-index on the picture: the page's own order then
    // puts it over the layer, the way it puts the client's own overlays over it.
    if (getComputedStyle(video).position === "static") video.style.position = "relative";
    // A wash is not a picture: it does not need the picture's frame rate, and reading
    // the picture's edge back every frame would. It is drawn on its own clock.
    var timer = setInterval(render, Math.round(1000 / GLOW_FPS));
    return { element: element, bars: bars, picture: video, stream: stream, box: parent,
      sides: sides, gaps: gaps, timer: timer, key: "", line: "" };
  }

  function apply(why) {
    var video = picture();
    if (video === null || !video.isConnected) {
      drop();
      coverOff();
      return;
    }
    var stream = video.srcObject;
    var box = { width: window.innerWidth, height: window.innerHeight };
    var shape = coverShape(video, box);
    // A picture that is not there yet has no shape: nothing is cropped and nothing
    // is filled on a box this page cannot measure against it.
    if (shape === null || video.readyState < 1 || !(box.width > 1) || !(box.height > 1)) {
      drop();
      coverOff();
      return;
    }
    // The box is the picture's own shape: nothing to crop and nothing to fill. The
    // gap this asks about is the one the shape difference makes, so a box within a
    // pixel of the picture's shape is the picture's shape (a 1280x720 box for a
    // 1280x720 stream, and a 1280x728 one is 8 px off it, which is a crop).
    var gap = shape.loss * (shape.screen > shape.picture ? box.width : box.height);
    if (gap <= EDGE) {
      drop();
      coverOff();
      var whole = size(box) + " box, " + shape.picture.toFixed(2) + ":1 picture";
      if (whole !== saidWhole) {
        saidWhole = whole;
        post("picture " + size({ width: video.videoWidth, height: video.videoHeight }) +
          " in a " + whole + ": the picture's own shape, nothing to crop and nothing to fill");
      }
      return;
    }
    // COVER: the picture is scaled up, keeping its shape, until it reaches every edge
    // of the box, and the overhang is cut. Nothing is drawn beside it because there is
    // nothing beside it.
    if (shape.loss <= CROP_LIMIT) {
      drop();
      coverOn(video);
      if (video.readyState < 2) return;
      var percent = Math.round(shape.loss * 100);
      var each = Math.round(shape.loss * 50);
      var line = "picture " + video.videoWidth + "x" + video.videoHeight + " in a " + size(box) +
        " box (" + shape.screen.toFixed(2) + ":1): scaled up until it covers the screen, " + percent +
        "% of the picture's " + (shape.screen > shape.picture ? "height" : "width") + " cut from the " +
        (shape.screen > shape.picture ? "top and bottom, " + each + "% each" : "left and right, " + each + "% each") +
        ", nothing stretched and no second copy anywhere";
      if (line !== saidWhole) {
        saidWhole = line;
        post(line + " (" + why + ")");
      }
      return;
    }
    // GLOW: past a third of the picture this script does not cut the picture up. It
    // is left exactly as the client laid it out and each gap carries its own edge
    // colours, dimmed, as a wash.
    coverOff();
    var inner = painted(video);
    var gaps = gapsFor(box, inner);
    var sides = sidesOf(gaps);
    var parent = video.parentElement;
    if (sides.length === 0 || parent === null) {
      drop();
      return;
    }
    // The bars' geometry is the gaps themselves, so a box that changes shape within
    // the same sides (the control strip taking its height out of the box, a window
    // dragged wider) is a layer to make again, not to keep.
    var key = sides.map(function (side) { return side + ":" + gaps[side]; }).join(" ") +
      " in " + box.width + "x" + box.height + " @" + Math.min(2, window.devicePixelRatio || 1);
    if (layer === null || layer.stream !== stream || layer.box !== parent ||
        !layer.element.isConnected || layer.element.nextSibling !== video || layer.key !== key) {
      drop();
      layer = make(video, stream, gaps, sides, box);
      layer.key = key;
    }
    render();
    if (video.readyState < 2) return;
    var list = sides.map(function (side) { return side + " " + gaps[side] + " px"; });
    var line2 = "picture " + size(inner) + " in a " + size(box) + " box (" + shape.screen.toFixed(2) +
      "): the picture whole -- covering it would cost " + Math.round(shape.loss * 100) +
      "% of the picture -- and its own edge colours, dimmed, in the gap: " +
      (list.length > 1 ? list.slice(0, -1).join(", ") + " and " + list[list.length - 1] : list[0]) +
      ", nothing stretched, nothing cropped, no second copy";
    if (line2 !== layer.line) {
      layer.line = line2;
      post(line2 + " (" + why + ")");
    }
  }

  // The picture's shape is the client's business, so this listens for it appearing
  // and for it being rebuilt: a stream that starts, a source swapped, a player the
  // client re-renders after a reconnect -- and, every two seconds, a look that covers
  // a swap the page passes through no event for.
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
    return {
      mode: cover !== null ? "cover" : (layer !== null ? "glow" : "none"),
      fitted: layer !== null || cover !== null,
      box: Math.round(box.width) + "x" + Math.round(box.height),
      // With the cover on, the picture IS the whole box (that is what the rule asks
      // for); without it, where the client laid the picture out.
      picture: video === null ? null : size(cover !== null ? video.getBoundingClientRect() : painted(video)),
      stream: video === null ? null : video.videoWidth + "x" + video.videoHeight,
      loss: shape === null ? null : Math.round(shape.loss * 1000) / 1000,
      cover: cover !== null,
      coverClass: video !== null && video.classList.contains(COVER_CLASS),
      sides: layer === null ? [] : layer.sides,
      gaps: layer === null ? null : layer.gaps,
      bars: layer === null ? 0 : layer.bars.length,
      streams: layer === null ? 0 : layer.element.querySelectorAll("video").length,
      line: cover !== null ? saidWhole : (layer === null ? saidWhole : layer.line),
    };
  };
})();
