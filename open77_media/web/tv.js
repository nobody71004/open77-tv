// =============================================================================
// open77_media -- web/tv.js
// =============================================================================
// The page on one television.
//
// It owns no state. Everything it shows and everything it knows came from the
// server through `media:state`; it reports what happened and asks for changes,
// and it never assumes a change took effect. That is the whole contract, and it
// is why two players watching the same television cannot see different things.
//
// -----------------------------------------------------------------------------
// WHAT THIS CEF BUILD CAN AND CANNOT PLAY (measured, see
// docs/research/webui-media-and-audio.md)
// -----------------------------------------------------------------------------
//   VP9 + Opus (WebM)          works
//   YouTube                    works, embedded and controllable
//   H.264 / AAC                refused at the demuxer -- no proprietary codecs
//   HLS (`.m3u8`)              not present in this build at all
//   Widevine / PlayReady       no CDM in libcef, so DRM video cannot play
//
// The consequence a person will actually hit: plain `.mp4` does NOT play, and
// the failure is silent from a distance -- a black rectangle. So an `.mp4` link
// is refused here with a sentence on screen naming the codec, rather than
// handed to a `<video>` element that will sit there black.
//
// Netflix is not a limitation of this code: no CDM can ship in a game that runs
// under EAC, and Netflix additionally gates desktop playback on a hardware
// signature a CEF host cannot present.

(function () {
  "use strict";

  // ---------------------------------------------------------------------------
  // Bridge
  // ---------------------------------------------------------------------------
  // The surface has no bridge when the page is opened in a normal browser while
  // developing it. Shimming it means the page runs, draws and can be iterated on
  // without a game launch, which is the difference between a five-second edit
  // loop and a five-minute one.
  const bridge = (typeof Open77 !== "undefined" && Open77) ? Open77 : {
    on: function () {},
    emit: function () {},
    ready: function () {},
  };

  const elements = {
    idle: document.getElementById("idle"),
    idleLabel: document.getElementById("idle-label"),
    idleDetail: document.getElementById("idle-detail"),
    media: document.getElementById("media"),
    embed: document.getElementById("embed"),
    notice: document.getElementById("notice"),
    controls: document.getElementById("controls"),
    play: document.getElementById("play"),
    seek: document.getElementById("seek"),
    seekFill: document.getElementById("seek-fill"),
    time: document.getElementById("time"),
    mute: document.getElementById("mute"),
    volume: document.getElementById("volume"),
    volumeText: document.getElementById("volume-text"),
    url: document.getElementById("url"),
    go: document.getElementById("go"),
  };

  // The server's last word. Never mutated locally except by a `media:state`.
  let state = { url: "", volume: 100, muted: false, paused: false, label: "" };
  // What is currently on screen: { kind, src, videoId }.
  let showing = { kind: "none" };

  function report(status, detail) {
    bridge.emit("media:report", { status: status, detail: detail || "" });
  }

  function notice(text) {
    if (!text) {
      elements.notice.hidden = true;
      elements.notice.textContent = "";
      return;
    }
    elements.notice.hidden = false;
    elements.notice.textContent = text;
  }

  function showOnly(kind) {
    elements.idle.classList.toggle("on", kind === "idle");
    elements.media.classList.toggle("on", kind === "media");
    elements.embed.classList.toggle("on", kind === "embed");
  }

  // ---------------------------------------------------------------------------
  // URL normalisation
  // ---------------------------------------------------------------------------

  const MEDIA_EXTENSIONS = ["webm", "ogg", "ogv", "opus"];
  const REFUSED_EXTENSIONS = {
    mp4: "MP4 (H.264/AAC) is not playable in this build -- it has no proprietary codecs. Use a WebM link or a YouTube link.",
    m4v: "MP4 (H.264/AAC) is not playable in this build. Use a WebM link or a YouTube link.",
    mov: "QuickTime is not playable in this build. Use a WebM link or a YouTube link.",
    mp3: "MP3 is not playable in this build -- no MP3 decoder. Use Opus or Vorbis in WebM/Ogg.",
    m4a: "AAC audio is not playable in this build. Use Opus or Vorbis.",
    aac: "AAC audio is not playable in this build. Use Opus or Vorbis.",
    m3u8: "HLS is not implemented in this build at all. Use a WebM link.",
    mpd: "MPEG-DASH is not implemented in this build. Use a WebM link.",
    mp4v: "MP4 (H.264/AAC) is not playable in this build.",
  };

  function extensionOf(url) {
    const withoutQuery = url.split("#")[0].split("?")[0];
    const match = /\.([A-Za-z0-9]{1,5})$/.exec(withoutQuery);
    return match ? match[1].toLowerCase() : "";
  }

  /// The id of a YouTube video in any of the shapes a person actually pastes.
  function youTubeId(url) {
    let parsed;
    try {
      parsed = new URL(url);
    } catch (error) {
      return null;
    }
    const host = parsed.hostname.replace(/^www\./, "").replace(/^m\./, "");
    if (host === "youtu.be") return parsed.pathname.slice(1).split("/")[0] || null;
    if (host !== "youtube.com" && host !== "youtube-nocookie.com") return null;
    if (parsed.pathname === "/watch") return parsed.searchParams.get("v");
    const parts = parsed.pathname.split("/").filter(Boolean);
    if (parts.length >= 2 && ["embed", "shorts", "live", "v"].indexOf(parts[0]) !== -1) {
      return parts[1];
    }
    return null;
  }

  function vimeoId(url) {
    const match = /vimeo\.com\/(?:video\/)?(\d+)/.exec(url);
    return match ? match[1] : null;
  }

  /// Decides how a URL is shown. Returns `{ kind, src, videoId }` or
  /// `{ kind: "refused", reason }`.
  function classify(url) {
    if (!url) return { kind: "idle" };

    const videoId = youTubeId(url);
    if (videoId) {
      // The embed player, with `enablejsapi` so the transport can be driven by
      // postMessage. No external script is loaded for this: the player's own
      // message protocol is what makes control work, and depending on a script
      // from a CDN to control a screen in a game is a dependency that can be
      // withdrawn at any time.
      const params = [
        "autoplay=1",
        "enablejsapi=1",
        "rel=0",
        "playsinline=1",
        "modestbranding=1",
        "origin=" + encodeURIComponent(window.location.origin),
      ];
      return {
        kind: "embed",
        src: "https://www.youtube.com/embed/" + encodeURIComponent(videoId) + "?" + params.join("&"),
        videoId: videoId,
      };
    }

    const vimeo = vimeoId(url);
    if (vimeo) {
      return { kind: "embed", src: "https://player.vimeo.com/video/" + vimeo + "?autoplay=1" };
    }

    const extension = extensionOf(url);
    if (REFUSED_EXTENSIONS[extension]) {
      return { kind: "refused", reason: REFUSED_EXTENSIONS[extension] };
    }
    if (MEDIA_EXTENSIONS.indexOf(extension) !== -1) {
      return { kind: "media", src: url };
    }

    // Anything else is attempted as an embed, and the page says so on screen.
    // Whether a site permits framing cannot be known from here -- a refused
    // frame reports the same `load` as an accepted one -- so this is stated as
    // an attempt rather than dressed up as a guarantee.
    return { kind: "embed", src: url, unchecked: true };
  }

  // ---------------------------------------------------------------------------
  // Rendering
  // ---------------------------------------------------------------------------

  function applyVolume() {
    const audible = !state.muted;
    const volume = Math.max(0, Math.min(100, Number(state.volume) || 0));

    elements.media.muted = !audible;
    elements.media.volume = volume / 100;
    if (showing.kind === "embed" && showing.videoId) {
      // `setVolume` is 0..100 on the player side, `volume` here is 0..100 too.
      sendYouTube(audible ? "setVolume" : "mute", audible ? [volume] : []);
    } else if (showing.kind === "embed") {
      // An arbitrary site's player is inside a document this page cannot reach.
      // Said once, on screen, rather than leaving an operator to wonder why the
      // volume slider moves and nothing happens.
      notice(audible
        ? "Volume cannot be controlled for an embedded site's own player."
        : "Mute cannot be controlled for an embedded site's own player.");
    }

    elements.volume.value = String(volume);
    elements.volumeText.textContent = String(volume);
    elements.mute.innerHTML = audible ? "&#128266;" : "&#128263;";
  }

  function applyPaused() {
    if (showing.kind === "media") {
      if (state.paused) {
        elements.media.pause();
      } else {
        const attempt = elements.media.play();
        if (attempt && typeof attempt.catch === "function") {
          attempt.catch(function () {
            // Autoplay policy, or an unsupported stream. Both are worth saying
            // out loud: one is fixed by a user gesture, the other never is.
            report("play_refused", "the browser refused playback");
          });
        }
      }
    } else if (showing.kind === "embed" && showing.videoId) {
      sendYouTube(state.paused ? "pauseVideo" : "playVideo", []);
    }
    elements.play.innerHTML = state.paused ? "&#9654;" : "&#10074;&#10074;";
  }

  function render() {
    const decided = classify(state.url);
    elements.idleLabel.textContent = state.label || "Open77 television";

    if (decided.kind === "idle") {
      showOnly("idle");
      elements.idleDetail.textContent = "no signal -- set a URL from the menu";
      notice("");
      showing = { kind: "none" };
      report("idle", "");
      return;
    }

    if (decided.kind === "refused") {
      showOnly("idle");
      elements.idleDetail.textContent = "this link cannot be played here";
      notice(decided.reason);
      showing = { kind: "none" };
      report("refused", decided.reason);
      return;
    }

    // A change of source means a recreation. Reusing the element across two
    // different sources leaks the previous one's socket and, for the embed,
    // its player.
    const changed = showing.src !== decided.src || showing.kind !== decided.kind;

    if (decided.kind === "media") {
      if (changed) {
        elements.media.src = decided.src;
        elements.media.load();
      }
      showOnly("media");
      showing = { kind: "media", src: decided.src };
      // An audio-only file in a video element draws a black rectangle. The
      // element stays visible anyway: a black screen for a podcast is honest,
      // and hiding it would show the test pattern instead, which is a lie.
      report("playing", "direct media");
      notice("");
      applyVolume();
      applyPaused();
      return;
    }

    if (changed) {
      elements.embed.src = decided.src;
    }
    showOnly("embed");
    showing = { kind: "embed", src: decided.src, videoId: decided.videoId || null };
    notice(decided.videoId
      ? ""
      : "Embedded site. Its own player cannot be controlled from here, and some sites refuse to be framed -- if nothing appears, this link cannot be shown on a television.");
    report(decided.videoId ? "playing" : "embed_unverified",
      decided.videoId ? "youtube embed" : "arbitrary embed");
    applyVolume();
    applyPaused();
  }

  // ---------------------------------------------------------------------------
  // YouTube transport, without the YouTube API script
  // ---------------------------------------------------------------------------
  // The embedded player accepts a documented postMessage protocol when
  // `enablejsapi=1` is on the URL. Using it avoids a CDN dependency inside a
  // game process, which matters here more than it would in a browser: a script
  // that fails to load is a control surface that silently stops existing.
  function sendYouTube(func, args) {
    if (!elements.embed.contentWindow) return;
    const message = JSON.stringify({
      event: "command",
      func: func,
      args: args || [],
    });
    try {
      elements.embed.contentWindow.postMessage(message, "*");
    } catch (error) {
      report("control_failed", String(error));
    }
  }

  // ---------------------------------------------------------------------------
  // Control strip
  // ---------------------------------------------------------------------------
  // Shown only while the surface has focus. A television watched from the street
  // must not have a control strip across the bottom of its picture.

  function syncControlsVisibility() {
    const focused = document.hasFocus();
    elements.controls.hidden = !focused;
  }

  function currentTimeText(seconds) {
    if (!isFinite(seconds) || seconds < 0) return "--:--";
    const total = Math.floor(seconds);
    const minutes = Math.floor(total / 60);
    const rest = total % 60;
    return minutes + ":" + (rest < 10 ? "0" : "") + rest;
  }

  function syncSeek() {
    const duration = elements.media.duration;
    if (isFinite(duration) && duration > 0) {
      const fraction = elements.media.currentTime / duration;
      elements.seek.value = String(Math.round(fraction * 1000));
      elements.seekFill.style.width = (fraction * 100) + "%";
    }
    elements.time.textContent =
      currentTimeText(elements.media.currentTime) + " / " + currentTimeText(duration);
  }

  // ---------------------------------------------------------------------------
  // Events
  // ---------------------------------------------------------------------------

  bridge.on("media:state", function (payload) {
    if (!payload || typeof payload !== "object") return;
    const previousUrl = state.url;
    state = {
      url: typeof payload.url === "string" ? payload.url : "",
      volume: typeof payload.volume === "number" ? payload.volume : state.volume,
      muted: payload.muted === true,
      paused: payload.paused === true,
      label: typeof payload.label === "string" ? payload.label : "",
    };
    elements.url.value = state.url === previousUrl ? elements.url.value : state.url;
    render();
    syncControlsVisibility();
  });

  // The menu can ask for the controls explicitly, for a television someone has
  // opened to configure rather than to watch.
  bridge.on("media:controls", function (payload) {
    const show = payload && payload.show === true;
    elements.controls.hidden = !show;
  });

  elements.media.addEventListener("loadedmetadata", syncSeek);
  elements.media.addEventListener("timeupdate", syncSeek);
  elements.media.addEventListener("error", function () {
    notice("This file could not be played. The build has no H.264/AAC decoder -- a WebM link or a YouTube link will work.");
    report("media_error", elements.media.error ? String(elements.media.error.code) : "");
  });
  elements.embed.addEventListener("load", function () {
    if (showing.videoId) report("youtube_loaded", showing.videoId);
  });

  elements.play.addEventListener("click", function () {
    bridge.emit("media:volume", { paused: !state.paused });
  });
  elements.mute.addEventListener("click", function () {
    bridge.emit("media:volume", { muted: !state.muted });
  });
  elements.volume.addEventListener("input", function () {
    bridge.emit("media:volume", { volume: Number(elements.volume.value) });
  });
  elements.seek.addEventListener("input", function () {
    if (showing.kind !== "media") return;
    const duration = elements.media.duration;
    if (!isFinite(duration)) return;
    elements.media.currentTime = (Number(elements.seek.value) / 1000) * duration;
  });
  elements.go.addEventListener("click", function () {
    bridge.emit("media:url", { url: elements.url.value });
  });
  elements.url.addEventListener("keydown", function (event) {
    if (event.key === "Enter") bridge.emit("media:url", { url: elements.url.value });
  });

  window.addEventListener("focus", syncControlsVisibility);
  window.addEventListener("blur", syncControlsVisibility);

  window.addEventListener("keydown", function (event) {
    if (event.target === elements.url) return;
    if (event.key === " ") {
      event.preventDefault();
      bridge.emit("media:volume", { paused: !state.paused });
    } else if (event.key === "m" || event.key === "M") {
      bridge.emit("media:volume", { muted: !state.muted });
    } else if (event.key === "ArrowUp") {
      bridge.emit("media:volume", { volume: Math.min(100, state.volume + 5) });
    } else if (event.key === "ArrowDown") {
      bridge.emit("media:volume", { volume: Math.max(0, state.volume - 5) });
    }
  });

  // The transport is polled at 2 Hz for the seek bar only, and only for a direct
  // media file -- the one case where the page owns the element and a time read is
  // a local property access. Nothing here decides anything; a stale bar catches
  // up on the next read.
  setInterval(function () {
    syncControlsVisibility();
    if (showing.kind === "media") syncSeek();
  }, 500);

  // A dark start. Until the server says otherwise the television shows its test
  // pattern, which is the honest picture: this page is alive and has nothing to
  // show yet. It is also how the composite chain is proved end to end without
  // depending on any external site.
  render();
  bridge.ready();
  bridge.emit("media:ready", {});
})();
