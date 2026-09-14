# Integrating the television into an Open77 tree

`open77_media/` is self-contained. The **render** is not: a browser composited
onto a world quad is host-side work, so the resource needs a handful of seams in
the plugin. This document names each one, in the order they matter, and records
the two things that will otherwise cost you an evening.

`patches/` holds the television-relevant hunks of every modified seam file.
Those files also carry other unreleased work, so the patches are **not a clean
patch series** — apply them by hand, reading each one.

---

## 1. The one new module

**`client/src/api/MediaScreens.hpp` / `.cpp`** (copied whole into `native/`)

Binds a WebUI surface to a world prop, projects that prop's screen quad every
tick, publishes one `WorldOverlay` item per visible screen, and decides
visibility by testing line of sight to the screen's centre. It holds no entity:
the binding is `{"this surface, on that prop's screen"}` and it is dropped when
the prop is not projected. That is what lets a screen be bound before its prop
has streamed in, and lets the prop move without the screen caring.

`client/CMakeLists.txt` adds it to `Open77.Client`:

```cmake
src/api/MediaScreens.cpp
src/api/MediaScreens.hpp
```

## 2. The Lua surface

**`scripting/src/ResourceHost.cpp`**, **`scripting/include/op77/Scripting/ResourceHost.hpp`**

The host publishes an `Open77.media` table:

| Lua | behaviour |
|---|---|
| `Open77.media.bind{ prop=, surface=, offset=, right=, up=, width=, height=, label= }` | returns a screen id, or `nil, reason` |
| `Open77.media.update(id, {…})` | moves / relabels a screen |
| `Open77.media.unbind(id)` | releases one screen |
| `Open77.media.clear()` | releases every screen this resource owns |
| `Open77.media.list()` | the snapshots, for diagnostics |

All of it is gated by the **`world.props`** capability, not a new one: a screen
with no prop has nothing to be a screen on, so it is the same authority.

`GameBackend` grows the matching contract — `MediaScreenQuad`,
`MediaScreenDefinition`, `MediaScreenSnapshot`, and five `std::function` hooks:
`bindMediaScreen`, `updateMediaScreen`, `unbindMediaScreen`,
`releaseMediaScreens`, `mediaScreenSnapshot`. On the **server** these are null,
so the natives answer `media_backend_unavailable` rather than pretending.

## 3. The client supplies the backend

**`client/src/scripting/ClientResourceHost.cpp`**

Wires the five hooks to `Api::MediaScreens::Bind/Update/Unbind/ReleaseAll/…`,
translating a binding failure into the string the Lua caller sees.

## 4. The render seam

**`client/src/webui/WorldOverlay.hpp` / `.cpp`** — a new `Style::Screen`. This is
the one overlay style that is not a shape from the atlas: the content is a page
CEF has already rasterised into a texture the client holds a descriptor for, and
what the overlay supplies is the one thing a page cannot supply for itself —
**where on the world the rectangle goes**. Read the comment on the enum member
for the occlusion caveat; it is the same text as the README.

**`client/src/webui/WebUiService.hpp` / `.cpp`** — two additions:

* a **surface texture accessor**, so the overlay can sample the CEF texture for
  a given surface;
* **`hudSuppressed`**, per surface, on the create options. Without it a
  television's page is composited onto the world quad *and* blitted fullscreen
  across the viewport, which looks like a bug in the game rather than a bug in
  this feature. The resource's manifest asks for it with
  `web_ui_auto_create false` plus the flag on the create call.

## 5. The server has to load it

The resource list is per-config, so add `"open77_media"` alongside the other
`resources.list` entries. In the tree this came from that is:

```
server/server.jsonc
server/server.pvplab.jsonc
templates/freeroam/template.json
```

Its manifest declares `auto_start true`, `reload_policy "local"`, a
`web_ui_page "web/tv.html"` that is never auto-created, and exactly two
capabilities: `network.events` and `world.props`.

## 6. The menu tab

**`resources/gamemodes/freeroam/`** — `web/index.html` (the TV tab),
`web/app.js` (catalogue, spawn, live-screen list), `client/main.lua` (the
client half: applies the state snapshot, binds surfaces).

This is the second gotcha below: the tab **is not in `open77_media`**.

## 7. The suite, in both harnesses

* `tools/lua-test/run.lua` — registers `open77_media / records`, preloading
  `open77_admin/shared/config.lua` **before** `shared/records.lua`. The order
  matters: the suite cross-checks records against the admin prop-model aliases.
* `server/tests/Open77.Server.Tests/Resources/MediaRecordsTests.cs` — the same
  suite through the C# test host.

`tools/run-suite.py` in this repository does the same thing with no monorepo.

---

## The two things that bite

### A resource that exists is not a resource that loaded

A resource is discovered at **startup** and by an explicit `refresh`. The
automatic tree rescan is opt-in via `resources.watchIntervalMilliseconds`, and if
it is absent or zero, adding a directory changes nothing at all.

The failure is silent in the worst way: the server is healthy, players connect,
the resource's files are on disk, its name is in the config — and the resource is
not running. There is no error line, because nothing went wrong.

Check the server's own log for the resource announcing itself. `open77_media`
prints on load:

```
[resource:open77_media] discovered
[resource:open77_media] open77_media ready -- 8 television records
[resource:open77_media] started generation=1
```

No such line means it is not loaded. Fix by restarting the server, or — with a
running server and no restart — by running the console command `refresh`, which
re-runs discovery and starts everything marked `auto_start`, and republishes the
client resource set.

Adding `open77_media` to the config **and** starting the server before the
directory existed is exactly how this was hit: the config said one thing, the
running process had read another, and the fix was a restart, not a code change.

### The tab is in `freeroam`

With the resource loaded and the catalogue answering, a player can still see no
TV tab — because the tab ships in the `freeroam` gamemode, not in the
television resource. Deploying a new resource does not deploy a changed
gamemode.

Verify by file, not by assumption:

```bash
# on the server, against a checkout
diff <(cd <checkout>/resources/gamemodes/freeroam && find . -type f -exec md5sum {} +|sort) \
     <(cd <server>/resources/gamemodes/freeroam   && find . -type f -exec md5sum {} +|sort)
```

`freeroam` reloads itself when its files change and the watcher is on; the log
line to look for is `[resource:freeroam] reloaded generation=N`, followed by
`Resource set published generation=N`.

## Proving it without launching the game

Everything below is reachable with the server running:

```bash
# is the resource in the signed set the client downloads?
curl -s http://<server>:11802/resources/v1/set | python -m json.tool | grep -o open77_media

# does the package the client would fetch contain the page?
#   /resources/v1/packages/<digest> is CBOR; the file paths appear verbatim
curl -s http://<server>:11802/resources/v1/packages/<digest> | strings | grep web/tv
```

A resource's client package carries `client/`, `shared/`, `web/` and declared
`files` — **not** `server/main.lua`. That is the design, not a packaging bug, and
it is worth knowing before you go looking for the missing file.

The in-game render is the one step that needs a launch: spawn a set from the TV
tab and look for `MediaScreens: bound screen … prop=… surface=…` in the client
log.
