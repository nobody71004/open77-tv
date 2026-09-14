# open77-tv

A web browser on a spawnable prop's screen, for Open77.

Spawn a television, give it a URL, and it renders inside the game world on a quad
attached to that prop — visible to everyone who can see the prop, with transport
and audio controls. The page is a real Chromium surface (CEF) hosted by the client
plugin, so it is not a texture the server pushes: it is a browser the world
happens to be looking at.

```
open77_media/          the resource -- this is the television
  open77.lua             manifest: permissions, the page, what ships to clients
  server/main.lua        authority: which screen exists, on which prop, showing what
  client/main.lua        binds a surface to a prop and keeps the quad on it
  shared/records.lua     the catalogue: 8 television records, their models and quads
  web/tv.html|css|js     the page one television shows
  tests/records_test.lua the catalogue suite (208 assertions)
native/
  MediaScreens.hpp|cpp   the host-side module: bind a surface to a prop, project
                         its screen quad, publish overlay items, line-of-sight test
patches/                 TV-only hunks of the host-side seams (see docs/integration.md)
docs/
  integration.md         every seam, what it does, and the gotcha that bites
  webui-media-and-audio.md  what this CEF build can and cannot actually play
tests/fixtures/          a snapshot of open77_admin's prop-model aliases
tools/
  run-suite.py           run the suite with no monorepo
  extract-tv-patches.py  regenerate patches/ from a checkout
```

## What actually plays

Measured against the CEF build the client hosts, not assumed — the full figures
and the method are in `docs/webui-media-and-audio.md`:

| Source | Result |
|---|---|
| YouTube (`watch?v=`, `youtu.be/`, `/shorts/`, `/live/`, `/embed/`) | works, and is controllable |
| WebM / Ogg (VP9 + Opus, Vorbis) | works, with seek |
| Vimeo | works as an embed |
| plain `.mp4` / `.m4v` / `.mov` | **refused** — no H.264/AAC decoder in this build |
| `.mp3` / `.m4a` / `.aac` | **refused** — no MP3/AAC decoder |
| HLS (`.m3u8`), MPEG-DASH (`.mpd`) | **refused** — not implemented in this build |
| Netflix, and any Widevine/PlayReady service | **impossible** — no CDM. A CDM cannot ship inside a process running under EAC, and Netflix additionally gates desktop playback on a hardware signature a CEF host cannot present. |

The refusals are deliberate and visible: the page names the codec and why, on
screen. Handing an `.mp4` to a `<video>` element produces a silent black
rectangle, which is the same picture as "the composite is broken" and costs an
afternoon to tell apart.

## The honest limitations

* **Occlusion.** The world overlay has no depth buffer, so a screen the producer
  decides is visible is painted over whatever geometry stands between the camera
  and it. `Api::MediaScreens` line-of-sight tests the screen's *centre* and the
  whole quad is then drawn or not, so a partially occluded screen is drawn whole.
* **Volume on a foreign embed.** A YouTube embed is driven through its
  `postMessage` transport, so volume, mute, pause and seek all work. Any other
  site's player lives in a document this page cannot reach: the slider moves and
  nothing happens, and the page says so rather than pretending.
* **Screen budget.** Eight CEF surfaces per resource, held by the client. A
  server may hold far more televisions than any one client will materialise, so
  the client decides which are worth building from the distances the server
  publishes.
* **Facing.** The screen's position is server-authoritative (read from the
  player's own snapshot), but the yaw is accepted from the caller and clamped,
  because nothing in the player snapshot publishes a heading.

## Running the suite

```bash
python tools/run-suite.py --from /path/to/open77-base   # live prop-model list
python tools/run-suite.py                               # vendored snapshot only
```

Lua 5.4 required (pass `--lua /path/to/lua` if it is not on `PATH`). The suite
cross-checks every record's `model` against the prop-model aliases that
`open77_admin` publishes, which is why it needs one of the two sources above; the
vendored snapshot lets it run with no checkout, but only the `--from` form cannot
drift. Refresh the snapshot with
`--refresh-fixture --from <checkout>`.

## Installing it into a server

The resource is self-contained; the **render** is not, because a browser on a
world quad is host-side work. `docs/integration.md` names every seam, and
`patches/` carries the TV-only hunks of each one.

Two things bite during install, both written up in that document:

1. A newly added resource is **not** picked up because its files appeared. The
   automatic rescan is opt-in, so either restart the server or run the console
   command `refresh` — and until that happens the server looks healthy while the
   resource simply is not there.
2. The menu tab lives in the **`freeroam`** resource, not in `open77_media`. Both
   have to be current, or the catalogue loads and no tab appears.

## Provenance

Extracted from `Open2077/open77-base` (branch `feat/cynosure-dev2`), which is
where this was developed and verified. `open77_media/` and `native/` are
verbatim; `patches/` holds only the television-relevant hunks of files that
carry other unreleased work, selected mechanically by
`tools/extract-tv-patches.py` and therefore not a clean patch series — apply
them by hand.
