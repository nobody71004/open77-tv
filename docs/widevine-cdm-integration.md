# Wiring Google's official Widevine CDM into the TV's CEF build

This is the **legitimate** integration path: load Google's *official, unmodified*
Widevine Content Decryption Module so DRM video (Netflix and the rest) can go
through the normal Encrypted Media Extensions path. This is what Chrome, Brave,
Vivaldi and Opera do. It is ordinary DRM use.

It is **not** a way to strip or copy DRM video. Building a custom/forged CDM,
extracting content keys, spoofing the security level / VMP attestation, or
ripping a stream are circumvention and are deliberately out of scope. Where a
service refuses this host (see the caveats at the end), that refusal is the
answer — we do not forge past it.

## 1. Build flags (`args.gn`)

If you build CEF/Chromium from source, the GN arguments are:

```gn
# args.gn  (in the Chromium/CEF out/ dir you build)
is_official_build           = true
proprietary_codecs          = true
ffmpeg_branding             = "Chrome"
enable_widevine             = true
```

* `enable_widevine = true` turns on the Widevine integration in the build.
* `proprietary_codecs = true` + `ffmpeg_branding = "Chrome"` add H.264/AAC,
  which Netflix's streams need in addition to the DRM. Without these the CDM is
  present but the video codecs are not, and playback still fails.

> The `cef_binary_144.0.32+...` this repo currently consumes is a **prebuilt**
> distribution built without Widevine (measured: `requestMediaKeySystemAccess`
> answers `NotSupportedError`, `docs/webui-media-and-audio.md`). To get the CDM
> you either rebuild CEF from source with the flags above, or (below) point a
> build at Google's official `widevinecdm` component.

## 2. Getting the CDM binary

The Widevine CDM is distributed and updated by Google:

* **Chrome** ships it and updates it via the component updater
  (`chrome://components` → "Widevine Content Decryption Module").
* In **CEF** (Alloy runtime), the same Chromium **component updater** downloads
  the CDM into `<CefSettings.user_data_path>/WidevineCdm`. Do not block it with
  `--disable-component-update`; `--component-updater=fast-update` asks for an
  earlier update attempt.

The older `CefRegisterWidevineCdm()` disk-registration API is deprecated for the
Alloy runtime. Check the API for the exact CEF branch you ship.

## 3. Web host wiring

The web host creates each surface's `CefRequestContext` in
`webhost/src/SurfaceClient.cpp`. Two things matter for the CDM:

* `CefSettings.user_data_path` must be set, because that is where the component
  updater places `WidevineCdm`. It already is (the `--open77-data` tree).
* The updater itself is disabled by default: both `WebHostApp` and the
  self-test app append `disable-component-update` unless `--open77-widevine`
  is on the host's command line. Pass that switch to allow Google's official
  CDM to arrive; in the game the launcher forwards it when
  `OP77_WEBUI_WIDEVINE=1` is set in the client's environment. The self-test
  host honors the same switch, so the probe in §4 measures the flip. Default
  off changes nothing: no CDM, no updater traffic.
* If you prefer to load a specific `widevinecdm` you already have (e.g. the one
  Chrome installed on your machine) instead of waiting on the component updater,
  pass Chromium's command-line switch when the host starts `CefInitialize`:

  ```
  --widevine-cdm-path=<dir containing the widevinecdm binary>
  ```

  Do **not** redistribute Google's CDM binary with a build you give to others —
  that needs a Widevine licence from Google. Using the copy Chrome installed on
  your own machine for your own playback is fine.

## 4. Verify it

The page already measures this. With the CDM wired up, the TV's own
`requestMediaKeySystemAccess('com.widevine.alpha', …)` probe resolves and the
screen/log reports `widevine | available` instead of `unavailable`
(`open77_media/web/tv.js`). You can also run the host's media probe:

```
Open77.WebHost.exe --open77-self-test-probe=<out>.json --open77-self-test-url=<page>
```

and check the `requestMediaKeySystemAccess(widevine)` row flips from
`refused: NotSupportedError` to granted.

## 5. What the CDM does NOT unlock on its own

A resolvable `requestMediaKeySystemAccess` means a CDM is present. It is
**necessary but not sufficient** for Netflix. Three things must hold at once:

1. **The CDM** is in the surface that renders the video (this doc).
2. **A top-level browser surface.** Netflix refuses to be *framed*
   (`X-Frame-Options`), and the TV page shows links in a frame. So Netflix
   cannot be shown by `tv.js`'s iframe even with a CDM — it needs a real
   top-level browser. The shared browser (`shared-browser/`, a real Chromium on
   the server that streams its picture to the TV) is that surface and is where
   the CDM should be loaded for Netflix.

   > Status on XBUNIVERSE staging (2026-10-16): done, without baking Google's
   > binary into the image. `shared-browser/server/up.sh` mounts the host's own
   > Chrome copy read-only into the room container
   > (`/opt/google/chrome/WidevineCdm` → `/usr/lib/chromium/WidevineCdm`) and
   > refuses to recreate the container when the host CDM is missing. Measured
   > in the room Chromium the same day: `requestMediaKeySystemAccess` answers
   > `WIDEVINE_AVAILABLE` with the mount, `NotSupportedError` without it
   > (`shared-browser/README.md`). No Widevine licence handshake was needed
   > because nothing is redistributed -- it is the host's own copy, playing on
   > the host.
3. **Netflix accepting the host.** Netflix runs its own device / Verified Media
   Path check and may cap resolution or refuse a host it does not recognise.
   That is Netflix's gate. **We do not forge the VMP signature or spoof the
   security level to get past it** — that would be circumvention.

So: wire the CDM per above, show Netflix in the shared browser, and accept the
resolution Netflix grants. If Netflix refuses the host, the honest fallback is
the watch party (`watch-party/`): everyone plays it in their own browser, in
step.
