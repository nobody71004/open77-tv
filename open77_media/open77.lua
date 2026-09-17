resource "open77_media"
version "0.1.0"
open77_version ">=0.0.1"
auto_start true
reload_policy "local"

-- A television is a prop with a web page on its screen.
--
-- Two registries, deliberately kept apart:
--
--   * the ENTITY is an ordinary world prop, created through `Open77.props` and
--     replicated by the props channel. It streams in and out, it is culled by
--     distance, it survives a client reconnect, and none of that is this
--     resource's business.
--   * the SCREEN is a `Open77.media` binding: "this page, on that prop's screen
--     quad". It is a projection over an entity, holds no entity, and is dropped
--     whenever the prop is not projected.
--
-- Keeping them apart is what lets a screen be bound before its prop has streamed
-- in -- the binding is a number and the lookup simply fails until it can
-- succeed -- and lets the prop move without the screen caring.
-- USING IT -- the three things a player has to know, and where each is decided:
--
--   * THE KEY. F5 opens the panel at the nearest set. It is registered with the
--     engine (`RegisterKeyMapping`, permission `input.actions`), so it appears in
--     the pause menu with the game's own keys, a rebind is remembered per machine,
--     and it is configurable without a resource edit. Nothing in this resource
--     prints the key as a literal: the panel footer and every idle screen ask
--     `Open77.input.keyFor` for the EFFECTIVE binding, so a player who moved it is
--     told the key they actually have.
--
--   * THE REACH. A set is drivable from within ITS OWN reach -- a per-record number
--     computed by `shared/placement.lua`, carried on the wire, and drawn in the
--     panel header next to the distance. It scales with the set because the server
--     puts a screen down in front of whoever spawned it at the set's own distance:
--     1.1 m for a 1.16 m television, 30.6 m for a 150 ft cinema screen. A flat range
--     refused the cinema the moment it was spawned. When the nearest set is out of
--     reach the panel says which set, how far, and how far it can be driven -- it
--     never silently does nothing.
--
--   * THE PASTE. Ctrl+V is the HOST's edit command (`SurfaceClient::Send` turns the
--     accelerator into CEF's own paste), because a page cannot read the clipboard;
--     no page-side work would change that. The URL field therefore takes the caret
--     when the panel opens, so a paste has somewhere to land, and it is never
--     hidden -- spawning starts with nothing in reach by definition, and the field
--     is the link the new set is born with.
--
--   * THE CONTROLS, and the two that are not about the set in front of you. The
--     curtain has three modes, not two (`media.curtain <id> open|closed|reveal`),
--     and the panel offers all three: the reveal -- the countdown the page owns and
--     the game-side effects it triggers -- used to be reachable from the screen's
--     own strip and the console and nowhere else. And the panel lists EVERY set in
--     the world, not only its subject, because `remove` is the one control that
--     cannot be gated on reach: a set spawned and then walked away from is out of
--     range of the panel and of everything on it, so a mis-spawn used to stay in
--     the world for good. The page may name a set for that one action; the client
--     validates the id against its own list of screens, so a page can name a set
--     it was shown and nothing else.
shared_script "shared/records.lua"

-- Where a set stands and which way it points, nudged and turned from the panel or
-- the console. Pure arithmetic (which way "left" is, what a quarter turn does to
-- a heading), shared because the server applies it and the tests pin it.
shared_script "shared/placement.lua"

-- Listed explicitly, one per line, never globbed. Manifest order IS load order
-- within each group, so these land exactly as written:
--
--   1. `config.lua`  publishes `MediaServerConfig` -- the seed the ad blocklist
--      starts from on a server nobody has told anything yet;
--   2. `adblock.lua` publishes `MediaAdBlock` (the pure policy: the rule grammar,
--      the refusals, the payload, the store format);
--   3. `main.lua`    reads BOTH at load, validates the seed and prints the
--      banner, and owns the live list.
server_script "server/config.lua"
server_script "server/adblock.lua"
server_script "server/main.lua"
client_script "client/main.lua"

-- The page ONE television shows. Created by client/main.lua per screen and
-- destroyed with it, so it is never auto-created and never follows the world
-- stream. `hudSuppressed = true` on the create call is what keeps it off the
-- player's own screen: without it the page would be composited into the world
-- quad AND blitted fullscreen across the viewport.
web_ui_page "web/tv.html"
web_ui_auto_create false
web_files { "web/**" }

-- Carried, not executed. The suites are run by `tools/lua-test/run.lua` (and by
-- the Lua test harness in the C# suite); listing them under `files` is what makes
-- the packaged resource include the bytes rather than shipping without them.
-- All three, including the client half: a resource whose catalogue suite ships
-- and whose client suite does not is one where the half that chooses which screens
-- to materialise is the half nobody can run outside the game.
files { "tests/records_test.lua", "tests/placement_test.lua", "tests/client_test.lua",
        "tests/adblock_test.lua" }

permissions {
    -- Spawn/remove/URL requests from the panel, and the state pushes back down.
    "network.events",

    -- The prop the screen is bound to. The screen half is `Open77.media`, which
    -- is gated by this same capability because a screen with no prop has nothing
    -- to be a screen on.
    "world.props",

    -- The reveal's own effects, and the reason this is not optional: the countdown
    -- is the PAGE's, but the two flare columns, the firework burst and the two
    -- race sounds are the game's, and without this capability every one of them is
    -- refused -- `permission_denied:world.effects`, once per cue, in the log and
    -- nowhere else. A curtain that counts 3-2-1 and parts in silence is a reveal
    -- that half happened, which is what "the reveal does nothing" looks like from
    -- the driver's seat. Declared here because the effects are played on the
    -- client (`playRevealCue` in client/main.lua) and the manifest is what grants
    -- them.
    "world.effects",

    -- `RegisterKeyMapping` / `Open77.input.keyFor`: the panel's toggle and the name
-- of the key it is bound to. One capability for both, because a resource that may
-- register a key is a resource that may ask what it ended up bound to -- the
-- alternative is a hint naming a key the player rebound away from.
    "input.actions",

    -- `Open77.webui.blocklist` / `.blocklistState`: the operator's ad-blocklist
    -- layer, and the receipt that says whether the browser host took it. Its own
    -- name rather than part of an existing group, because it is the only verb in
    -- the client that changes what a page is allowed to reach -- a resource that
    -- may draw a page is not automatically a resource that may widen what a
    -- client refuses, and the push can only ever ADD to the compiled list.
    "webui.policy",

    -- The live list, kept in this resource's own `data/` directory
    -- (`Open77.io.readJson` / `writeJson`). The host confines both to that
    -- directory, so this grants the resource its own state and nothing else --
    -- without them every `media.adblock.*` command still works, and says so,
    -- but the change is lost when the resource restarts.
    "filesystem.read",
    "filesystem.write",
}
