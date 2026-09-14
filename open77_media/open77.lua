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
shared_script "shared/records.lua"

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

-- Carried, not executed. The suite is run by `tools/lua-test/run.lua` (and by
-- the Lua test harness in the C# suite); listing it under `files` is what makes
-- the packaged resource include the bytes rather than shipping without them.
files { "tests/records_test.lua" }

permissions {
    -- Spawn/remove/URL requests from the menu, and the state pushes back down.
    "network.events",

    -- The prop the screen is bound to. The screen half is `Open77.media`, which
    -- is gated by this same capability because a screen with no prop has nothing
    -- to be a screen on.
    "world.props",
}
