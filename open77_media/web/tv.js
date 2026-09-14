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
  //
  // The volume starts at the same 75 the server creates a television at
  // (`DEFAULT_VOLUME` in server/main.lua), so the slider and the mixer agree from
  // the first frame instead of the page holding 100 until the state round-trips.
  let state = { url: "", volume: 75, muted: false, paused: false, label: "" };
  // What is currently on screen: { kind, src, videoId }.
  let showing = { kind: "none" };

  // ---------------------------------------------------------------------------
  // The player's own verdict, and why it is needed
  // ---------------------------------------------------------------------------
  // A YouTube embed is the one thing this page cannot see. It can only ask the
  // player what it is doing, and the player answers over `postMessage` once it
  // has been told somebody is listening.
  //
  // Without that, the page reported `playing` the instant it set an `src` -- a
  // claim about something it had not observed. The first time it was wrong, a
  // television showed YouTube's own error card while both logs said the screen
  // was playing, which is the same shape of false success that hid the
  // `invalid_offset` failure for a whole session.
  //
  // The codes are YouTube's. 101/150 are "the owner forbids embedding"; 153 is
  // "the player refused the page" and in practice means it could not read a
  // referrer it found acceptable -- which is why `tv.html` pins one.
  const YOUTUBE_STATES = {
    "-1": "unstarted", "0": "ended", "1": "playing",
    "2": "paused", "3": "buffering", "5": "cued",
  };
  const YOUTUBE_ERRORS = {
    "2": "the video id was refused",
    "5": "the player could not play this video here",
    "100": "the video does not exist or is private",
    "101": "the owner does not allow this video to be embedded",
    "150": "the owner does not allow this video to be embedded",
    "153": "the player refused the page: it could not read an acceptable referrer",
  };

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
      //
      // `origin` is YouTube's own recommendation: it tells the player which
      // origin is allowed to command it. Kept, and the reason it is worth
      // stating is that a browser harness refused this embed with error 153
      // ("video player configuration error") while the game's own CEF host plays
      // the same video -- measured, `docs/research/webui-media-and-audio.md` --
      // so the parameter is not what 153 means, and dropping a documented
      // restriction to satisfy an automation browser would be a fix for the
      // wrong thing. What the page does instead is report the player's verdict
      // rather than assuming it (see `listenToYouTube`).
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

  /// Which channel a player command actually went out on, and what the player
  /// says its own level is afterwards.
  ///
  /// Volume and mute had no reporting at all, which made every volume complaint
  /// unfalsifiable from a log: the server's state changed, the page moved its own
  /// slider, and whether the picture's sound followed was invisible. The player
  /// answers its own level back when it can, so "asked for 20, the player says
  /// 20" is now a different log line from "there was nothing to command" -- and
  /// the second one is the answer that was missing.
  function reportVolume(volume, audible) {
    let target = "nothing";
    let actual = "?";
    if (youTubePlayer !== null) {
      target = "player api";
      if (typeof youTubePlayer.getVolume === "function") {
        try {
          actual = String(youTubePlayer.getVolume());
        } catch (error) {
          actual = "unreadable";
        }
      } else {
        // The API hands the player its transport methods asynchronously, so a
        // command issued before that point has no method to call at all.
        actual = "no setVolume yet";
      }
    } else if (youTubeFrame !== null) {
      target = "raw embed";
    }
    report(audible ? "volume_applied" : "mute_applied",
      "asked=" + volume + (audible ? "" : " muted") +
      " via " + target + " (player reports " + actual + ")");
  }

  function applyVolume() {
    const audible = !state.muted;
    const volume = Math.max(0, Math.min(100, Number(state.volume) || 0));

    elements.media.muted = !audible;
    elements.media.volume = volume / 100;
    if (showing.kind === "embed" && showing.videoId) {
      // `setVolume` is 0..100 on the player side, `volume` here is 0..100 too.
      // `unMute` is sent as well as the level: a player that was muted earlier
      // stays muted otherwise, and "the volume slider moves and nothing happens"
      // is the complaint this pair exists to prevent.
      if (audible) {
        sendYouTube("unMute", []);
        sendYouTube("setVolume", [volume]);
      } else {
        sendYouTube("mute", []);
      }
      reportVolume(volume, audible);
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
      //
      // `loading` until the element says it is playing (the `playing` listener
      // below), so the log distinguishes "asked for a picture" from "has one".
      report("loading", "direct media");
      notice("");
      applyVolume();
      applyPaused();
      return;
    }

    showOnly("embed");
    showing = { kind: "embed", src: decided.src, videoId: decided.videoId || null };

    if (decided.videoId) {
      // A YouTube link. YouTube's own player is preferred because it can be ASKED
      // what it is doing; the plain iframe follows the same policy and applies
      // the same volume and pause state, and is used when the API cannot load.
      notice("");
      if (changed) {
        if (youTubeApiState === "ready") {
          startApiPlayer(decided.videoId);
        } else if (youTubeApiState === "failed") {
          startRawEmbed(decided.src, decided.videoId);
        } else {
          // Nothing is created yet: the player is built from the video id, not
          // from this URL, and `loadYouTubeApi` re-reads the current state when
          // the script lands.
          loadYouTubeApi();
        }
      }
      applyVolume();
      applyPaused();
      return;
    }

    if (changed) {
      clearYouTubePlayer();
      const frame = document.createElement("iframe");
      frame.setAttribute("allow",
        "autoplay; fullscreen; encrypted-media; picture-in-picture");
      frame.setAttribute("referrerpolicy", "no-referrer");
      frame.src = decided.src;
      elements.embed.appendChild(frame);
      youTubeFrame = frame;
    }
    notice("Embedded site. Its own player cannot be controlled from here, and some sites refuse to be framed -- if nothing appears, this link cannot be shown on a television.");
    // `loading`, not `playing`: whether a framed site shows anything is not this
    // page's decision and cannot be observed from here.
    report("embed_unverified", "arbitrary embed");
    applyVolume();
    applyPaused();
  }

  // ---------------------------------------------------------------------------
  // The YouTube transport: the player API first, the raw embed as its fallback
  // ---------------------------------------------------------------------------
  // There have been two versions of this, and the difference is worth keeping.
  //
  // The first drove the embed by `postMessage` alone, on the argument that a page
  // inside a game should not depend on a script fetched from a CDN: a script that
  // fails to load is a control surface that silently stops existing. The argument
  // is sound, and the transport still never started playback. In a session the
  // embed reported `youtube_loaded` and then nothing whatsoever -- no state, no
  // error, no picture -- because a player that is not driven through its API
  // answers no state questions, and this page had no way to ask. A television
  // whose only two possible log lines are "loaded" and "nothing" cannot be
  // diagnosed from a log, which is how this was found.
  //
  // So the API is loaded, and the raw iframe remains for the case where it cannot
  // be: offline, blocked, or a future YouTube that moves the endpoint. Every
  // state the player reports is logged verbatim, so the next session says whether
  // the player refused the video, refused the page, or simply never started.
  let youTubeApiState = "unloaded"; // unloaded | loading | ready | failed
  let youTubePlayer = null;
  let youTubeFrame = null;
  let youTubeWatchdog = null;

  /// Empties the container, whatever is in it.
  function clearYouTubePlayer() {
    if (youTubeWatchdog !== null) {
      clearInterval(youTubeWatchdog);
      youTubeWatchdog = null;
    }
    if (youTubePlayer && typeof youTubePlayer.destroy === "function") {
      try {
        youTubePlayer.destroy();
      } catch (error) {
        // The container is about to be emptied anyway; a player that refuses to
        // tear itself down must not stop the next one being created.
      }
    }
    youTubePlayer = null;
    youTubeFrame = null;
    elements.embed.innerHTML = "";
  }

  /// Fetches the player API once, and says which way it went.
  function loadYouTubeApi() {
    if (youTubeApiState !== "unloaded") return;
    youTubeApiState = "loading";
    report("loading", "youtube embed (player api)");

    const fallBackToRawEmbed = function (reason) {
      youTubeApiState = "failed";
      report("youtube_api_failed", reason);
      const current = classify(state.url);
      if (current.kind === "embed" && current.videoId) {
        startRawEmbed(current.src, current.videoId);
      }
    };

    // A script that never answers is the case the first version could not
    // report, so it is bounded and named rather than left to hang.
    const giveUp = setTimeout(function () {
      if (youTubeApiState === "ready") return;
      fallBackToRawEmbed("the player script did not load within six seconds");
    }, 6000);

    window.onYouTubeIframeAPIReady = function () {
      clearTimeout(giveUp);
      youTubeApiState = "ready";
      report("youtube_api_ready", "");
      const current = classify(state.url);
      if (current.kind === "embed" && current.videoId) {
        startApiPlayer(current.videoId);
      }
    };

    const script = document.createElement("script");
    script.src = "https://www.youtube.com/iframe_api";
    script.async = true;
    script.addEventListener("error", function () {
      clearTimeout(giveUp);
      fallBackToRawEmbed("the player script refused to load");
    });
    document.head.appendChild(script);
  }

  /// "error 153: the player refused the page...", the way the log wants it.
  function errorText(code) {
    const key = String(code);
    return "error " + key + ": " + (YOUTUBE_ERRORS[key] || "the player reported an error");
  }

  /// The player's own state, reported with the clock: "playing at 5.9s" is the
  /// difference between a picture and a black rectangle that claims to be one.
  function notePlayerState(code, player) {
    const name = YOUTUBE_STATES[String(code)];
    if (!name) return;
    const time = player && typeof player.getCurrentTime === "function"
      ? player.getCurrentTime() : -1;
    report("player_state", name + (time >= 0 ? " at " + time.toFixed(1) + "s" : ""));
    if (name === "playing") {
      notice("");
      report("playing", "youtube embed");
    }
  }

  /// Watches the first fifteen seconds of a player's life.
  ///
  /// Bounded on purpose. This is the window in which a player that is never
  /// going to start is still worth another command, and after it the interval
  /// stops rather than reporting the same state on a television for an hour.
  function watchYouTube(player) {
    if (youTubeWatchdog !== null) clearInterval(youTubeWatchdog);
    let attempts = 0;
    youTubeWatchdog = setInterval(function () {
      attempts += 1;
      if (attempts > 6) {
        clearInterval(youTubeWatchdog);
        youTubeWatchdog = null;
        return;
      }
      if (!player || typeof player.getPlayerState !== "function") return;
      const name = YOUTUBE_STATES[String(player.getPlayerState())] || "unknown";
      const time = typeof player.getCurrentTime === "function" ? player.getCurrentTime() : -1;
      report("player_state", name + " at " + (time >= 0 ? time.toFixed(1) : "?") + "s");
      if ((name === "unstarted" || name === "cued") && !state.paused
          && typeof player.playVideo === "function") {
        player.playVideo();
      }
    }, 2500);
  }

  /// The preferred path: the player the API builds, whose state can be read.
  function startApiPlayer(videoId) {
    if (youTubeApiState !== "ready" || typeof YT === "undefined" || !YT.Player) return;
    clearYouTubePlayer();
    youTubePlayer = new YT.Player(elements.embed, {
      videoId: videoId,
      playerVars: {
        autoplay: 1,
        controls: 0,
        disablekb: 1,
        playsinline: 1,
        rel: 0,
        modestbranding: 1,
        origin: window.location.origin,
      },
      events: {
        onReady: function (event) {
          report("player_ready", videoId);
          applyVolume();
          applyPaused();
          watchYouTube(event.target);
        },
        onStateChange: function (event) { notePlayerState(event.data, event.target); },
        onError: function (event) {
          notice("This video will not play here: " + errorText(event.data) + ".");
          report("youtube_error", errorText(event.data));
        },
      },
    });
  }

  /// The fallback: a plain embed, driven by `postMessage` when `enablejsapi=1`
  /// is on its URL. Same picture, same commands, no state read-back.
  function startRawEmbed(src, videoId) {
    clearYouTubePlayer();
    youTubeFrame = document.createElement("iframe");
    youTubeFrame.setAttribute("allow",
      "autoplay; fullscreen; encrypted-media; picture-in-picture");
    youTubeFrame.setAttribute("referrerpolicy", "no-referrer");
    youTubeFrame.addEventListener("load", function () {
      report("youtube_loaded", videoId);
      listenToYouTube();
    });
    youTubeFrame.src = src;
    elements.embed.appendChild(youTubeFrame);
    report("loading", "youtube embed (no player api)");
  }

  /// Asks a plain embed to start talking to this page. The player posts nothing
  /// until it is told somebody is listening, and it matches the reply to the
  /// `id`/`channel` it was given -- so both are sent, and both are constants
  /// because this page has exactly one embed.
  function listenToYouTube() {
    if (!youTubeFrame || !youTubeFrame.contentWindow) return;
    try {
      youTubeFrame.contentWindow.postMessage(JSON.stringify({
        event: "listening",
        id: 1,
        channel: "widget",
      }), "*");
    } catch (error) {
      report("control_failed", String(error));
    }
  }

  /// Drives whichever player is in the container: the API's own object when
  /// there is one, the plain frame's `postMessage` protocol otherwise. Both
  /// answer to the same command names, so the callers above need not know which.
  function sendYouTube(func, args) {
    const parameters = args || [];
    if (youTubePlayer && typeof youTubePlayer[func] === "function") {
      try {
        youTubePlayer[func].apply(youTubePlayer, parameters);
      } catch (error) {
        report("control_failed", String(error));
      }
      return;
    }
    if (!youTubeFrame || !youTubeFrame.contentWindow) {
      // A command with no channel is dropped, and dropping it in silence is how
      // "I turned it down and nothing happened" becomes unanswerable. There are
      // two real ways to get here -- a state change between `new YT.Player` and
      // the API handing it its methods, and a player torn down under a live
      // surface -- and this line is what tells them apart from the log.
      report("control_unavailable", func + " (no player and no embed frame)");
      return;
    }
    const message = JSON.stringify({
      event: "command",
      func: func,
      args: parameters,
    });
    try {
      youTubeFrame.contentWindow.postMessage(message, "*");
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
  elements.media.addEventListener("playing", function () {
    report("playing", "direct media");
  });
  elements.media.addEventListener("error", function () {
    notice("This file could not be played. The build has no H.264/AAC decoder -- a WebM link or a YouTube link will work.");
    report("media_error", elements.media.error ? String(elements.media.error.code) : "");
  });
  // The player's own words. Reported rather than acted on: whether a video can
  // be shown is the player's decision, and this page's job is to say what it
  // said instead of guessing a reason.
  window.addEventListener("message", function (event) {
    // The API path reports through its own `events` callbacks; this listener is
    // the plain-iframe path's only voice, so it stays out of the way when the
    // player object is present rather than reporting everything twice.
    if (youTubePlayer) return;
    let payload = event.data;
    if (typeof payload === "string") {
      try {
        payload = JSON.parse(payload);
      } catch (error) {
        return;
      }
    }
    if (!payload || typeof payload !== "object" || typeof payload.event !== "string") return;

    if (payload.event === "onError") {
      const code = String(payload.info);
      const meaning = YOUTUBE_ERRORS[code] || "the player reported an error";
      notice("This video will not play here: " + meaning + " (error " + code + ").");
      report("youtube_error", code + ": " + meaning);
      return;
    }

    if (payload.event === "onStateChange") {
      const name = YOUTUBE_STATES[String(payload.info)];
      if (name) report("player_state", name);
      // The picture is real once the player says it is playing, and at no point
      // before that.
      if (name === "playing") {
        notice("");
        report("playing", "youtube embed");
      }
    }
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
