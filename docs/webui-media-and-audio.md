# What the in-game TV can actually play — measured (2026-09-14)

Research note for the CEF "TV / url player" feature. Everything below was measured
against the libcef build this repo ships (`cef_binary_144.0.32+g5ce7d26+chromium-144.0.7559.258_windows64`),
on the development machine, with the game closed. No part of it is read off a build
config.

The question it answers is not academic: the feature promise was "plays youtube videos
netflix any link". Two of those three are impossible in this runtime, and a TV that
silently shows a black rectangle for half of them is worse than one that says why.

## How to reproduce

```bash
# codec / DRM / reachability probe (page reports; host writes the file)
artifacts/Release/web/Open77.WebHost.exe \
  --open77-self-test-probe=<out>.json \
  --open77-self-test-url=http://127.0.0.1:<port>/index.html

# media playback probe: a VP9+Opus sample and an H.264/AAC sample, six seconds of
# real playback, page report on the first line and host PCM observation on the second
artifacts/Release/web/Open77.WebHost.exe \
  --open77-self-test-probe=<out>.json \
  --open77-self-test-url=http://127.0.0.1:<port>/media.html

# the audio sink on its own: needs neither Chromium nor the game
artifacts/Release/web/Open77.WebHost.exe --open77-self-test-audio=3
```

The probe pages and the two generated samples used for the run recorded here are the
`tone.webm` (vp9+opus) / `tone.mp4` (h264+aac) pair produced by `ffmpeg` from the same
sine and the same colour source, so codec pair is the only difference between them.

## Result

| Capability | Measured | Meaning |
|---|---|---|
| `video/webm; codecs="vp9,opus"` | `probably` | the free codec pair is present |
| VP9+Opus playback | **playing**, `readyState=4`, 6.16 s of a 634 s stream | works |
| YouTube (full player, real stream) | **playing**, `quality=medium`, `currentTime=5.97` | works |
| `video/mp4; codecs="avc1.42E01E,mp4a.40.2"` | `""` (empty) | absent |
| H.264/AAC playback | `DEMUXER_ERROR_NO_SUPPORTED_STREAMS` | refused at the demuxer, not merely "not advertised" |
| MSE `video/mp4; codecs="avc1…"` | `false` | no MSE path around it either |
| MSE `video/webm; codecs="vp9"` | `true` | the WebM path is real, not a fallback |
| HLS (`application/vnd.apple.mpegurl`) | `""` | no native HLS |
| MediaSource | `true` / `mediaSourceMp4=false` | WebM-only MSE |
| EME present | `true` | the API exists… |
| `requestMediaKeySystemAccess(widevine)` | `refused: NotSupportedError` | …but no Widevine CDM ships in libcef |
| playready | `refused: NotSupportedError` | as above |
| page reachability to YouTube | `true` | the network path is fine |

Netflix is out for two independent reasons: no CDM in this runtime, and — even with a
CDM present (Edge's `widevinecdm.dll` exists on this machine, so `--widevine-cdm-path`
is technically reachable) — Netflix gates L3 desktop playback on a VMP signature that a
CEF host does not have. Neither can be solved by configuration, and neither CDM can be
redistributed with a game that runs under EAC.

## Audio

Windowless CEF owns no audio device: it decodes and hands the PCM to the client, and a
client that returns no `CefAudioHandler` gets a page that plays perfectly and a machine
that stays silent.

The first probe run is the evidence for that, in the form of its own absence:
`OP77PROBE` reported `webmState=playing` with **no `OP77AUDIO` line at all**, because the
probe wrote its report and closed the browser ~1 s in — before any audio thread had
delivered a packet. Holding the session open through six seconds of real playback
produced, on the same build and the same page:

```
OP77AUDIO:{"streams":1,"stops":0,"packets":263,"frames":269312,"channels":2,
           "sampleRate":44100,"peak":0.062833,"error":""}
```

263 packets × 1024 frames = 269,312 frames = 6.1 s of stereo 44.1 kHz float PCM, and the
peak is `0.0628` — the generated 0.125-amplitude sine at the page's `volume = 0.5`,
exactly. So the page's volume and mute controls are honoured *in the PCM*, not merely
stored.

The sink that plays this PCM is `webhost/src/AudioSink.cpp`. Its self-test:

```
Open77 audio self-test: device="Speakers (Echo Dot-JBM)" rate=44100 channels=2 float32
Open77 audio self-test: ok (pushed=132096 played=145971 expected~132300)
```

`pushed` matches wall clock (3.0 s at 44.1 kHz); the surplus `played` is the 100 ms
silence the device is primed with plus the 300 ms drain after the last push, which is
also what the residual `underrun` counts. Steady-state underrun is zero.

One design detail worth keeping: a shared-mode WASAPI client **cannot choose its own
sample rate**, so the device format is probed first and handed back to CEF from
`GetAudioParameters`. Answering with CEF's 44.1 kHz default on a 48 kHz device would
play every page fast and sharp.

## Decisions taken with the user

* **Ship the WebM/YouTube path.** YouTube, Vimeo and any VP9/WebM+Opus source work with
  sound. H.264 MP4 links and DRM services are refused *with a reason shown in the TV UI*
  instead of a silent black screen. No proprietary-codec build, no CDM, nothing to
  license or redistribute.
* **Audio is produced in the webhost process.** One `AudioSink` per surface, so each TV
  has its own gain and mute and the audio engine mixes them on one device. World
  positioning (falloff with distance from the TV) is a separate, later milestone: it
  needs the PCM in the game process, not just the host's.

## What this note does not cover

The in-world half — a spawnable TV prop with a screen the surface is drawn onto, its
spawn controls and the TV record catalogue in the freeroam menu — is not built here.
This pass is the browser half only: what the runtime can play, and the audio path that
makes it audible.
