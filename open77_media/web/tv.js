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
//   H.264 / AAC                no decoder in this build -- but the HOST can
//                              re-encode such a link into WebM on the fly
//                              (see /op77/media/probe and /op77/media/stream,
//                              served by the web host itself)
//   Widevine / PlayReady       no CDM in libcef, so DRM video cannot play
//
// So a link is no longer judged by its file extension. The page ASKS the host:
// /op77/media/probe runs ffprobe on the link and answers with the container,
// the codecs and a verdict -- playable, transcoded, or nothing. `playable`
// feeds the element directly; `transcoded` feeds it the host's own
// /op77/media/stream route, whose answer is the same link decoded into VP9/
// Opus WebM, seekable because the page owns the element; `nothing` is a 404 or
// an HTML page, and says so out loud instead of showing a black rectangle.
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

  // Extension tables kept only for the classification fast path: a WebM is
  // playable without a round-trip to ffprobe, and everything else is decided
  // by the probe. An extension that USED to be refused here (mp4, m3u8, mp3...)
  // now goes to the probe like any other link, because the host can re-encode
  // what this build cannot decode -- refusing by name was correct when there
  // was no decoder, and is wrong now.
  const MEDIA_EXTENSIONS = ["webm", "ogg", "ogv", "opus"];
  const PROBE_EXTENSIONS = new Set(["mp4", "m4v", "mov", "mp3", "m4a", "aac", "m3u8", "mpd", "mp4v", "ts", "flv", "mkv"]);
  // YouTube and Vimeo keep their embed paths: their players are controllable
  // and report their own state, which a bare stream is not.
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

    // The fast path: a WebM/Ogg link plays as it is, no probe round-trip. A
    // recognised media extension goes to the probe like everything else (the
    // host may re-encode it), and an unknown URL -- an HTML page, a shortener,
    // a site that serves video from URLs with no extension -- goes to the probe
    // too, because ffprobe is the only thing here that actually knows. An
    // embed is the LAST resort now, not the default: a page this host can
    // decode beats a page this page cannot see.
    const extension = extensionOf(url);
    if (MEDIA_EXTENSIONS.indexOf(extension) !== -1) {
      return { kind: "media", src: url };
    }
    return { kind: "probe", url: url };
  }

  // ---------------------------------------------------------------------------
  // The probe: the host's answer to "what is this link"
  // ---------------------------------------------------------------------------
  // One fetch to /op77/media/probe, answered by the host from inside this
  // process. The verdict decides the path: playable -> the element gets the
  // link itself; transcoded -> the element gets the host's own stream route,
  // which is the same link decoded into WebM this build plays natively.
  // Nothing here is guessed from an extension.
  let probeToken = 0;

  function startProbe(url) {
    const token = ++probeToken;
    report("probing", url);
    fetch("/op77/media/probe?u=" + encodeURIComponent(url), { cache: "no-store" })
      .then(function (response) { return response.json(); })
      .then(function (answer) {
        if (token !== probeToken || classify(state.url).url !== url) return; // stale
        applyProbeVerdict(url, answer || {});
      })
      .catch(function (error) {
        if (token !== probeToken) return;
        notice("The decoder could not be asked about this link (" + error + ").");
        report("probe_failed", String(error));
      });
  }

  function applyProbeVerdict(url, answer) {
    if (answer.verdict === "playable") {
      // The extension was misleading; the decoder says the bytes play. Hand
      // the element the link itself.
      showing = { kind: "media", src: url };
      elements.media.src = url;
      elements.media.load();
      showOnly("media");
      report("loading", "direct media (probed: " + (answer.container || "?") + ")");
      notice("");
      applyVolume();
      applyPaused();
      return;
    }
    if (answer.verdict === "transcoded") {
      // The host decodes the link into WebM. The element's src is the ROUTE,
      // so the same element, transport and volume path serve it; the
      // duration is unknown until the stream says otherwise, which the seek
      // bar handles by disabling itself rather than lying.
      const streamUrl = "/op77/media/stream?u=" + encodeURIComponent(url) +
        "&ss=" + encodeURIComponent(String(answer.startSeconds || 0));
      showing = { kind: "media", src: streamUrl, transcoded: true };
      elements.media.src = streamUrl;
      elements.media.load();
      showOnly("media");
      report("loading", "host-decoded (" + (answer.video || "audio") + "/" + (answer.audio || "none") +
        " in " + (answer.container || "?") + ")");
      notice("This link is being decoded by the game host -- first picture in a few seconds.");
      applyVolume();
      applyPaused();
      return;
    }
    if (answer.verdict === "disabled") {
      notice(answer.detail || "The host cannot decode links: no decoder tools were staged.");
      report("transcode_disabled", answer.detail || "");
      return;
    }
    // "nothing": a 404, an HTML page, or a file no demuxer claimed.
    //
    // Those are two different answers, and treating them as one is why a website
    // link still showed nothing after the frame grant was in place. `classify`
    // sends an unknown URL to the probe precisely because ffprobe is the only
    // thing here that knows what a link is -- and for a page, "not media" is the
    // correct answer, not a verdict. The television's own log from the first run
    // with websites enabled says it exactly: `probing (https://.../hdtoday/)`
    // followed by `not_media (the decoder refused the link)`, with the frame that
    // was waiting on the other side never built.
    //
    // So the extension decides which of the two this is. A link that names a
    // media container and that no demuxer claimed is dead, and the screen says
    // so. A link that names no container is somebody's site, and it is shown the
    // only way a site can be shown here: framed.
    if (!PROBE_EXTENSIONS.has(extensionOf(url))) {
      report("not_stream_site", answer.detail || "");
      showSite(url, null);
      return;
    }
    showOnly("idle");
    elements.idleDetail.textContent = "this link is not a playable stream";
    notice("The decoder found nothing playable here" +
      (answer.detail ? " (" + answer.detail + ")" : "") +
      ". YouTube links always work.");
    report("not_media", answer.detail || "");
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

  /// A seek on a host-decoded stream is a NEW stream request starting at the
  /// requested position, because ffmpeg is decoding the source live. The
  /// element keeps its element identity (no recreation, no volume re-apply
  /// hiccup); the swap of src is what restarts the decoder at the offset.
  function seekTranscoded(seconds) {
    const url = showing && showing.transcoded && state.url;
    if (!url) return;
    const streamUrl = "/op77/media/stream?u=" + encodeURIComponent(url) +
      "&ss=" + encodeURIComponent(String(Math.max(0, Math.floor(seconds))));
    report("seek", "re-decoding from " + Math.floor(seconds) + "s");
    elements.media.src = streamUrl;
    elements.media.load();
    showing.src = streamUrl;
    const attempt = elements.media.play();
    if (attempt && typeof attempt.catch === "function") attempt.catch(function () {});
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

    if (decided.kind === "probe") {
      // The answer arrives later and the state may have moved on by then --
      // the television is not obliged to keep the link. The probe carries the
      // url it was asked about and render() re-classifies before acting.
      startProbe(decided.url);
      showing = { kind: "probing", url: decided.url };
      showOnly("idle");
      elements.idleDetail.textContent = "asking the decoder about this link...";
      notice("");
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
      showing = { kind: "media", src: decided.src, transcoded: decided.transcoded === true };
      // An audio-only file in a video element draws a black rectangle. The
      // element stays visible anyway: a black screen for a podcast is honest,
      // and hiding it would show the test pattern instead, which is a lie.
      //
      // `loading` until the element says it is playing (the `playing` listener
      // below), so the log distinguishes "asked for a picture" from "has one".
      report("loading", decided.transcoded ? "host-decoded stream" : "direct media");
      notice("");
      applyVolume();
      applyPaused();
      return;
    }

    showSite(decided.src, decided.videoId);
  }

  /// Shows somebody else's page in the frame, and reports what this side can
  /// honestly observe about it.
  ///
  /// Split out of `render()` because there are now two ways a page arrives here:
  /// classified as a site from the start (a YouTube or Vimeo link, or a URL the
  /// page recognises as one), and the probe's `nothing` verdict on a link that
  /// names no media container. Both end in the same frame, the same sandbox and
  /// the same two log lines -- and the second path is the one that used to end in
  /// "this link is not a playable stream" instead.
  /// Which handoff a late answer belongs to. The host is asked about a link over
  /// the network, and the link under the player can change while the question is
  /// in flight; a stale answer that framed itself would put the previous link's
  /// shell on the screen with the current link still in the state.
  let siteToken = 0;

  /// Builds the frame for a page, and says what is being shown.
  ///
  /// Split out from `showSite` because the URL is now decided asynchronously --
  /// by the host, which is the only side that can see a site's framing headers --
  /// and the frame must not be built until that answer is in.
  function buildEmbedFrame(src, note) {
    clearYouTubePlayer();
    const frame = document.createElement("iframe");
    frame.setAttribute("allow",
      "autoplay; fullscreen; encrypted-media; picture-in-picture");
    frame.setAttribute("referrerpolicy", "no-referrer");
    // A framed third-party page is the one thing on this screen that is not
    // ours, so it runs with the few capabilities a player needs and without
    // the ones that are only useful to something hostile. `allow-scripts` and
    // `allow-same-origin` are what let a site's own player run at all;
    // `allow-top-navigation-by-user-activation` keeps a click able to take the
    // screen to the site's own player URL (some sites play that way, and the
    // surface's policy permits the navigation) while refusing the silent
    // redirect that would otherwise hijack the screen and remove the player's
    // own controls with no click and no way back. Popups are granted here and
    // refused by the host, which is the only side that can tell a window a click
    // asked for from the five an advertising script opens on load.
    frame.setAttribute("sandbox",
      "allow-scripts allow-same-origin allow-forms allow-popups " +
      "allow-popups-to-escape-sandbox allow-top-navigation-by-user-activation " +
      "allow-presentation");
    frame.src = src;
    // The one thing this page CAN observe about somebody else's document, and
    // worth a line because it is the difference between two failures that look
    // identical on screen: a frame this page's policy refused never navigates
    // and never fires this, while a site that refuses to be framed
    // (`X-Frame-Options` / `frame-ancestors`) still arrives as an error page
    // and does. So `embed_framed` means "a document was allowed to arrive" --
    // never "there is a picture", which stays unobservable from here.
    frame.addEventListener("load", () => report("embed_framed", src));
    elements.embed.appendChild(frame);
    youTubeFrame = frame;
    notice(note || "Embedded site. Its own player cannot be controlled from here, and some sites refuse to be framed -- if nothing appears, this link cannot be shown on a television.");
    // `loading`, not `playing`: whether a framed site shows anything is not this
    // page's decision and cannot be observed from here.
    report("embed_unverified", "arbitrary embed");
    applyVolume();
    applyPaused();
  }

  /// Asks the host what this link can be shown as, then shows it.
  ///
  /// A pasted link is frequently a shell: a few kilobytes that refuse to be
  /// framed and wrap the application that actually plays. This side cannot tell
  /// that apart from a dead site -- an `X-Frame-Options` refusal arrives as an
  /// error page and is invisible to the embedder -- so the question is asked from
  /// outside, and the answer says both whether the link may be framed and which
  /// page to frame instead when it may not. See `/op77/web/frame`, and
  /// `op77/WebUI/FramePolicy.hpp` for what the host judges.
  ///
  /// A `fetch` that fails means there is no host to ask -- this page opened in a
  /// browser while being developed -- and the link is framed as it is, which is
  /// exactly what it did before there was anything to ask.
  function resolveSite(src) {
    const token = ++siteToken;
    const frame = (url, note) => {
      if (token !== siteToken) return;
      buildEmbedFrame(url, note);
    };
    fetch("/op77/web/frame?u=" + encodeURIComponent(src))
      .then((response) => response.json())
      .then((answer) => {
        if (!answer || answer.ok !== true || answer.frameable === true) {
          frame(src, "");
          return;
        }
        if (answer.via === "embed" && answer.best) {
          // The link was a shell and the host found the application inside it.
          report("embed_shell_resolved", answer.best);
          frame(answer.best,
            "This link is a shell around another site. The screen is showing that site's own player.");
          return;
        }
        report("embed_refused", answer.violation || "refused");
        frame(src,
          "This site refuses to be shown on a television" +
          (answer.violation ? " (" + answer.violation + ")" : "") +
          ", and wraps no page that does not.");
      })
      .catch(() => frame(src, ""));
  }

  function showSite(src, videoId) {
    const changed = showing.kind !== "embed" || showing.src !== src;
    showOnly("embed");
    showing = { kind: "embed", src: src, videoId: videoId || null };

    if (videoId) {
      // A YouTube link. YouTube's own player is preferred because it can be ASKED
      // what it is doing; the plain iframe follows the same policy and applies
      // the same volume and pause state, and is used when the API cannot load.
      notice("");
      if (changed) {
        if (youTubeApiState === "ready") {
          startApiPlayer(videoId);
        } else if (youTubeApiState === "failed") {
          startRawEmbed(src, videoId);
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
      // The frame is built by `resolveSite` once the host has answered, so the
      // notice and the `embed_*` lines come from there. Volume and pause state
      // are applied either way, and again when the frame arrives.
      resolveSite(src);
    }
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
      elements.seek.disabled = false;
    } else {
      // A host-decoded stream states no duration up front, so the bar reads
      // elapsed time and the slider disables itself. Disabled is honest; a
      // slider that jumps the picture to nothing is not.
      elements.seekFill.style.width = "0%";
      elements.seek.disabled = true;
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
    report("playing", showing.transcoded ? "host-decoded stream" : "direct media");
  });
  // A live, host-decoded stream carries no duration; once frames arrive the
  // seek bar flips from disabled to elapsed-time. This is also the moment the
  // "decoding" notice goes away.
  elements.media.addEventListener("loadeddata", function () {
    if (showing.transcoded) {
      notice("");
      report("stream_open", "the host is delivering decoded frames");
    }
  });
  elements.media.addEventListener("error", function () {
    if (showing.transcoded) {
      notice("The host stopped decoding this link. It may have ended, stalled, or the site refused the decoder.");
      report("media_error", "host stream ended");
      return;
    }
    notice("This file could not be played. A WebM link or a YouTube link will work.");
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
    if (!isFinite(duration) || duration <= 0) return;
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
