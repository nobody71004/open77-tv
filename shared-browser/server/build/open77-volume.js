// open77-volume.js -- put into the shared browser's client page (neko's
// index.html) by the Open77 image, ahead of the page's own scripts.
//
// The television's volume and mute, on the picture's sound. The television
// (open77_media's tv.js, the page framing this one) cannot reach into this page,
// so it posts its level here -- whenever the level changes, and once this page
// has loaded.
//
// The level cannot simply be put on the stream's media element. The game's
// browser takes a page's sound as the page produces it, and a WebRTC stream's
// element volume is applied later, by the audio device -- so in game the
// element's volume and mute changed nothing (2026-10-02: 75, 90 and 100 on the
// element, one loudness, and that one too quiet). So the sound is played through
// Web Audio, where the level is part of the samples:
//
//   * the stream's own audio track is disabled, which silences the element's
//     playout (WebRTC plays a disabled remote track at zero);
//   * a clone of that track, which disabling the original does not silence,
//     goes through a gain -- the television's level -- and a limiter to the
//     output.
//
// The level is a curve and goes above the stream's own: 100 is twice it (+6 dB)
// with the limiter catching the peaks, 50 a little under it, 0 silence. The
// original track is only disabled once Web Audio is running, so a page where
// it cannot run (or has not been allowed to start yet) keeps its sound, with the
// level on the element as before.
//
// Only the framing television is listened to, and only for this. What was done
// is told back to it when that changes (kind `volume`, which the television logs
// as browser_ice): a level, a gain and a state, nothing else.
(function () {
  "use strict";
  if (!window.parent || window.parent === window) return;

  var wanted = null; // { volume: 0..1, muted: boolean }
  var told = "";
  var audio = null;  // { context, gain, limiter, meter } once made; false if it cannot be
  var hooked = null; // { element, stream, track, clone, source }

  function post(text) {
    try {
      window.parent.postMessage({ open77SharedBrowser: 1, kind: "volume", mode: "auto",
        text: String(text).slice(0, 600) }, "*");
    } catch (error) { /* telling the television is best effort */ }
  }

  function isMedia(target) {
    return !!target && (target.tagName === "VIDEO" || target.tagName === "AUDIO");
  }

  function gainFor(level) {
    return level.muted ? 0 : 2 * Math.pow(level.volume, 1.5);
  }

  function makeAudio() {
    if (audio !== null) return audio;
    var Context = window.AudioContext || window.webkitAudioContext;
    if (typeof Context !== "function") {
      audio = false;
      return audio;
    }
    try {
      var context = new Context({ latencyHint: "playback" });
      var gain = context.createGain();
      var limiter = context.createDynamicsCompressor();
      limiter.threshold.value = -3;
      limiter.knee.value = 0;
      limiter.ratio.value = 20;
      limiter.attack.value = 0.002;
      limiter.release.value = 0.2;
      var meter = context.createAnalyser();
      meter.fftSize = 2048;
      gain.connect(limiter);
      limiter.connect(meter);
      meter.connect(context.destination);
      context.addEventListener("statechange", function () { apply("audio " + context.state); });
      audio = { context: context, gain: gain, limiter: limiter, meter: meter };
    } catch (error) {
      audio = false;
    }
    return audio;
  }

  function audioTrackOf(element) {
    var stream = element.srcObject;
    if (!stream || typeof stream.getAudioTracks !== "function") return null;
    var tracks = stream.getAudioTracks();
    return tracks.length > 0 && tracks[0].readyState === "live" ? tracks[0] : null;
  }

  function unhook() {
    if (hooked === null) return;
    try { hooked.source.disconnect(); } catch (error) { /* already gone */ }
    try { hooked.clone.stop(); } catch (error) { /* already gone */ }
    // The element keeps its stream: if it is still the same one, it gets its
    // own sound back.
    try { hooked.track.enabled = true; } catch (error) { /* already gone */ }
    hooked = null;
  }

  // The stream's sound through the gain, once Web Audio runs. Returns whether
  // the element's own playout is the gain's now.
  function hook(element) {
    var track = audioTrackOf(element);
    if (track === null) return false;
    if (hooked !== null && hooked.track === track && hooked.element === element) {
      if (track.enabled) track.enabled = false;
      return true;
    }
    var made = makeAudio();
    if (!made || made.context.state !== "running") return false;
    unhook();
    try {
      var clone = track.clone();
      var source = made.context.createMediaStreamSource(new MediaStream([clone]));
      source.connect(made.gain);
      track.enabled = false;
      hooked = { element: element, stream: element.srcObject, track: track, clone: clone, source: source };
      return true;
    } catch (error) {
      return false;
    }
  }

  function apply(why) {
    if (wanted === null) return;
    var made = makeAudio();
    if (made && made.context.state === "suspended") {
      made.context.resume().catch(function () { /* waits for a click in the page */ });
    }
    var elements = document.querySelectorAll("video, audio");
    var onGain = 0;
    for (var i = 0; i < elements.length; i++) {
      var element = elements[i];
      if (onGain === 0 && hook(element)) {
        onGain++;
        continue;
      }
      // No Web Audio for it (yet): the level on the element, as far as that goes.
      if (Math.abs(element.volume - wanted.volume) > 0.001) element.volume = wanted.volume;
      if (element.muted !== wanted.muted) element.muted = wanted.muted;
    }
    if (hooked !== null && onGain === 0) unhook();
    var gain = gainFor(wanted);
    if (made && hooked !== null) {
      made.gain.gain.setTargetAtTime(gain, made.context.currentTime, 0.03);
    }
    var level = Math.round(wanted.volume * 100) + (wanted.muted ? " muted" : "");
    var line = hooked !== null
      ? level + " on the stream's sound (gain " + gain.toFixed(2) + ", Web Audio " + made.context.state + ")"
      : level + " on " + elements.length + (elements.length === 1 ? " element" : " elements") +
        (made ? " (Web Audio " + made.context.state + ")" : " (no Web Audio)");
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
  // to the element: an element's own level is put back while it is the one
  // playing, and a stream that starts (the client's first, or a new one after a
  // reconnect) is taken through the gain.
  document.addEventListener("volumechange", function (event) {
    if (wanted === null || !isMedia(event.target) || (hooked !== null && hooked.element === event.target)) return;
    var element = event.target;
    if (Math.abs(element.volume - wanted.volume) > 0.001 || element.muted !== wanted.muted) apply("kept");
  }, true);
  ["loadedmetadata", "playing"].forEach(function (name) {
    document.addEventListener(name, function (event) {
      if (isMedia(event.target)) apply("started");
    }, true);
  });
  // A page that may not start sound by itself starts it at the first click or key
  // (asked again at each step of the gesture: the first may come before the
  // browser counts it as one).
  ["pointerdown", "pointerup", "click", "keydown", "keyup"].forEach(function (name) {
    window.addEventListener(name, function () {
      if (audio && audio.context.state === "suspended") audio.context.resume().catch(function () {});
    }, true);
  });
  // A stream swapped without an event the page passes through, or a track turned
  // back on: looked at every two seconds.
  setInterval(function () {
    if (wanted === null) return;
    if (hooked !== null && (hooked.element.srcObject !== hooked.stream || hooked.track.readyState !== "live" ||
        hooked.track.enabled)) {
      apply("stream changed");
    } else if (hooked === null && audio && audio.context.state === "running") {
      var elements = document.querySelectorAll("video, audio");
      for (var i = 0; i < elements.length; i++) {
        if (audioTrackOf(elements[i]) !== null) {
          apply("stream found");
          break;
        }
      }
    }
  }, 2000);

  // For the tests and the server's own check, never for the television: what is
  // in use, and how loud the output is (the root mean square of its last 2048
  // samples, after the gain and the limiter).
  window.__open77Volume = function () {
    var level = -1;
    if (audio && hooked !== null) {
      var samples = new Float32Array(audio.meter.fftSize);
      audio.meter.getFloatTimeDomainData(samples);
      var sum = 0;
      for (var i = 0; i < samples.length; i++) sum += samples[i] * samples[i];
      level = Math.sqrt(sum / samples.length);
    }
    return {
      wanted: wanted, hooked: hooked !== null, audio: audio ? audio.context.state : (audio === false ? "none" : "not made"),
      gain: audio ? audio.gain.gain.value : null, original: hooked !== null ? hooked.track.enabled : null, level: level,
    };
  };
})();
