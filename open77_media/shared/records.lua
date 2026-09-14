-- =============================================================================
-- open77_media -- shared/records.lua
-- =============================================================================
-- The television catalogue: for each record, the prop that is the screen and the
-- rectangle on that prop where the picture goes.
--
-- -----------------------------------------------------------------------------
-- WHERE THESE NUMBERS COME FROM (read this before changing one)
-- -----------------------------------------------------------------------------
-- Every `model` below is a mesh the game itself ships, taken from the 2.31
-- cooked-archive inventory (`docs/generated/props-2.31.csv`), and every `quad`
-- is that mesh's own measured bounding box -- not an estimate, and not a guess
-- at "about this big for a TV".
--
-- Measured with the same tooling that builds the prop hosts:
--
--   WolvenKit.CLI extract  -gp <game> -r '.*(television_|screen_device_|...)' \
--                          -o <work> -q
--   WolvenKit.CLI convert serialize <work> -o <json> -q
--   # then read Data.RootChunk.boundingBox of each *.mesh.json
--
-- The boxes are in the mesh's own local frame, which is also the frame the
-- spawned prop inherits: a prop entity is a host built *around* the target mesh,
-- so the mesh's local frame is the prop's local frame.
--
-- Two shapes occur, and they are quoted differently:
--
--   * a SCREEN mesh. The mesh is the display, so the rectangle is the whole
--     mesh, and `offset` is where that geometry sits inside its own origin (which
--     for the television screens is emphatically not zero -- the plane stands
--     0.42 m up the television's body).
--   * a BODY mesh with a separate screen mesh beside it (`television_a_16x9` and
--     `television_a_16x9_screen_a`). The record spawns the body, and the
--     rectangle is the *screen mesh's* box. That is only valid because the two
--     meshes share an origin: the screen's box sits strictly inside the body's
--     (X +/-0.58 inside +/-0.63; Z 0.09..0.75 inside -0.002..0.851), which is what
--     placing them at one transform means. Each such pair below is asserted in
--     the suite, so a re-authored asset that moves a screen is caught rather than
--     quietly painting outside the television.
--   * a HOUSING with no separate screen mesh (`tv_large_a`, the device panels,
--     the surveillance monitor). There is no measured screen submesh, so the
--     rectangle is the front face of the box -- the page covers the face. Said
--     plainly rather than dressed up as a measurement of the screen.
--
-- -----------------------------------------------------------------------------
-- THE QUAD IS IN THE PROP'S LOCAL FRAME, IN METRES
-- -----------------------------------------------------------------------------
--   offset  centre of the screen, from the prop's own origin
--   right   the screen's horizontal axis in the prop's local space
--   up      the screen's vertical axis in the prop's local space
--   width   full width of the screen, not a half-extent
--   height  full height
--
-- `right` and `up` are rotated by the prop's world orientation before use and are
-- NOT required to be unit length or exactly perpendicular -- the native side
-- normalises them and squares `up` against `right` (client/src/api/MediaScreens.cpp,
-- `ScreenCorners`). Every record here is an axis-aligned box face, so the axes
-- below are unit vectors; a hand-authored tilted panel can write a raw direction
-- instead.
--
-- -----------------------------------------------------------------------------
-- `faces`: WHICH SIDE OF THE PANEL CARRIES THE PICTURE
-- -----------------------------------------------------------------------------
-- A rectangle has two sides and a projection does not distinguish them, so
-- without this a television seen from behind played its video on the back of the
-- cabinet -- reported, exactly, as "tvs are playing videos on both sides".
--
-- `faces` is the direction the display looks along in the prop's OWN local
-- frame. It is optional:
--
--   * declared    the screen is drawn only from that side (one dot product per
--                 frame), and `media.list` reports `facing.source=declared`
--   * absent      drawn from BOTH sides, exactly as before this existed, and
--                 `media.list` reports `facing.source=undeclared`
--
-- Absent is the safe default on purpose. A screen that vanishes because a record
-- has not been measured yet is a worse failure than a screen seen through its own
-- cabinet, so anything genuinely ambiguous below is left undeclared rather than
-- guessed at -- and says so where it is listed.
--
-- It is a direction and not a rule. The obvious alternatives -- the sign of
-- `offset`, the cross product of `right` and `up`, a triangle winding -- each
-- bake in a handedness assumption, and this file has already been wrong once that
-- way: an earlier pass read the screen mesh's UVs, concluded a picture
-- orientation from them, and shipped a half-turn (see client/src/api/ScreenQuad.hpp
-- for the full account). A direction is one number that can be checked against the
-- mesh and corrected in one place.
--
-- What the declared fronts below rest on. Both facts are in the assets, and both
-- were re-measured for this field (WolvenKit `convert serialize` of the 2.31
-- meshes, reading `renderResourceBlob.header.quantizationOffset/Scale`, which is
-- the same measurement the quad boxes above came from):
--
--   * the screen mesh sits INSIDE the cabinet against one of its faces, so the
--     cabinet is behind the picture and the room is on the other side. For
--     `television_a_16x9`: body Y -0.0505..+0.1255, screen plane at Y 0.1150..
--     0.1158 -- 1 cm inside the +Y face. `television_a_21x9` is the same. So the
--     picture faces +Y and the standing room is +Y of the set.
--   * a bare screen mesh keeps the cabinet's own origin, so the panel is in front
--     of the origin without a body being drawn: `television_a_16x9_screen_a`'s own
--     box is Y 0.1150..0.1158 around an origin at Y 0, and the same is true of
--     `tv_large_a` along +Y. The origin is where the body would be, i.e. behind
--     the picture.
--
-- The two agree with the placement path, which is why the sets are drawn the way
-- players expect: `facingPlacement` in server/main.lua sets the prop down ahead of
-- the caller at `heading + 180`, and yaw 0 faces +Y (the engine's own convention,
-- docs/research/movement-and-heading.md) -- i.e. the prop is turned so the face
-- the picture is on looks back at the person who spawned it.
--
-- -----------------------------------------------------------------------------
-- ASPECT RATIO, AND WHY IT IS NOT 16:9 EVERYWHERE ANY MORE
-- -----------------------------------------------------------------------------
-- A screen is one CEF surface, and the page is mapped 1:1 onto the quad with UVs
-- 0..1. So a surface whose aspect differs from the quad's aspect stretches the
-- picture -- an earlier version of this file forced every record to 16:9 and had
-- nothing but 16:9 panels to put in it, which is why the catalogue was full of
-- signage and vending machines.
--
-- The game's real televisions are not one shape: the 16x9 and the 21x9 sets are
-- genuinely 1.76 and 2.39, the monitors are 4:3-ish and 2.35 ultrawide, the
-- panels include 4:3, 3:4, 16:9, 9:16, 9:21 and 2:1, and the frames include
-- squares and 1:2.3 portraits. The surface is therefore derived from the quad by
-- `Open77MediaSurfaceFor` below, which the client calls with the rectangle it was
-- actually sent -- so a quad retuned in game with `media.quad` also retunes the
-- surface it is mapped onto.
--
-- A 21:9 surface showing a 16:9 video letterboxes it with black bars at the
-- sides. That is the correct picture on a 21:9 screen; squashing the video to
-- fill it is not.
--
-- -----------------------------------------------------------------------------
-- THE BRIDGE REPORTS WHAT HAPPENED
-- -----------------------------------------------------------------------------
-- A wrong quad is diagnosable rather than mysterious:
--
--   Open77.media.list()      -- per screen: prop, surface, drawn, reason, distance
--   media.list               -- the same, from the server console
--
-- `reason` is the gate that refused the screen on the last game tick
-- (`prop_not_projected`, `behind_camera`, `occluded`, `out_of_range`, `drawn`),
-- so "the television is blank" always has an answer.
-- =============================================================================

Open77MediaRecords = {}

---@class Open77MediaRecord
---@field id string          shorthand used by commands and the menu
---@field label string       what the record calls itself
---@field model string       prop alias from the props catalogue
---@field quad table         screen rectangle in the prop's local frame
---@field quad.faces number[] optional front direction; absent = drawn both sides
---@field blurb string       one line for the menu

-- -----------------------------------------------------------------------------
-- Televisions: the set itself, with the page on its own screen
-- -----------------------------------------------------------------------------
-- The 3.4 cm bezel, the stand, the 21:9 option -- these are the meshes the game
-- puts in Night City apartments and bars, and the screen rectangle is the screen
-- mesh's measured box inside the body's own frame.
local records = {
    {
        id = "tv.16x9",
        label = "Television, 16:9",
        model = "electronics.tv.16x9",
        blurb = "The game's standard stand-up television. 1.16 x 0.66 m screen.",
        quad = {
            -- television_a_16x9_screen_a: X +/-0.58, Z 0.09..0.75, plane at
            -- Y 0.1154 -- 1 cm inside the body's front face at Y 0.1255.
            offset = { 0.0, 0.115394, 0.42 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.16,
            height = 0.66,
            -- The cabinet is behind the glass, so the picture faces +Y: the
            -- screen plane (Y 0.1150..0.1158) is 1 cm inside the body's +Y face
            -- (body Y -0.0505..+0.1255, re-measured for this field).
            faces = { 0.0, 1.0, 0.0 },
        },
    },
    {
        id = "tv.21x9",
        label = "Television, 21:9",
        model = "electronics.tv.21x9",
        blurb = "The same set in ultrawide. 1.58 x 0.66 m screen.",
        quad = {
            -- television_a_21x9_screen_a: X +/-0.79, Z 0.09..0.75.
            offset = { 0.0, 0.115394, 0.42 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.58,
            height = 0.66,
            -- Same cabinet as the 16:9 set (body Y -0.0514..+0.1255), same
            -- screen plane, same side: the picture faces +Y.
            faces = { 0.0, 1.0, 0.0 },
        },
    },
    {
        id = "tv.neokitsch.16x9",
        label = "Television, neokitsch 16:9",
        model = "electronics.tv.neokitsch.16x9",
        blurb = "Flat wall panel with a heavy frame. 1.34 x 0.77 m screen.",
        quad = {
            -- television_neokitsch_16x9_screen_a: X +/-0.670752, Z 0.1816..0.9531.
            offset = { 0.0, 0.000005, 0.567307 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.3415,
            height = 0.7715,
        },
    },
    {
        id = "tv.neokitsch.21x9",
        label = "Television, neokitsch 21:9",
        model = "electronics.tv.neokitsch.21x9",
        blurb = "The framed panel in ultrawide. 1.04 x 0.43 m screen.",
        quad = {
            -- television_neokitsch_21x9_screen_a: X +/-0.520752, Z 0.1816..0.6161.
            offset = { 0.0, 0.000005, 0.398807 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.0415,
            height = 0.4345,
        },
    },
    {
        id = "tv.large",
        label = "Television, large wall display",
        model = "electronics.tv.large",
        blurb = "Big wall screen, no housing. Front face of the set.",
        quad = {
            -- tv_large_a: X +/-0.9593, Z 0..1.0137, thickness 0.227 m. No separate
            -- screen mesh ships, so this is the measured FRONT FACE (Y max) at
            -- the box's own X/Z extents.
            offset = { 0.000013, 0.111389, 0.506419 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.9186,
            height = 1.0146,
            -- The rectangle IS the +Y face (body Y -0.1155..+0.1114), and the set
            -- is mounted on that face: the wall is behind the picture.
            faces = { 0.0, 1.0, 0.0 },
        },
    },

    -- -------------------------------------------------------------------------
    -- Bare television screens: the display with no cabinet
    -- -------------------------------------------------------------------------
    -- These are the same quads as the sets above, on the screen mesh alone. They
    -- are what to put on a wall, inside a shelving unit, or behind a bar.
    {
        id = "tv.screen.16x9",
        label = "TV screen, 16:9",
        model = "electronics.tv.screen.16x9",
        blurb = "A bare 16:9 panel, no cabinet. 1.16 x 0.66 m.",
        quad = {
            offset = { 0.0, 0.115394, 0.42 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.16,
            height = 0.66,
            -- The panel mesh keeps the cabinet's origin, so its own box is at
            -- Y 0.1150..0.1158: the origin (where the body would be) is behind
            -- the picture, which is why the picture faces +Y.
            faces = { 0.0, 1.0, 0.0 },
        },
    },
    {
        id = "tv.screen.21x9",
        label = "TV screen, 21:9",
        model = "electronics.tv.screen.21x9",
        blurb = "A bare ultrawide panel. 1.58 x 0.66 m.",
        quad = {
            offset = { 0.0, 0.115394, 0.42 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.58,
            height = 0.66,
            -- As `tv.screen.16x9`: the origin is behind the panel, so it faces +Y.
            faces = { 0.0, 1.0, 0.0 },
        },
    },
    {
        id = "tv.screen.neokitsch.16x9",
        label = "TV screen, neokitsch 16:9",
        model = "electronics.tv.screen.neokitsch.16x9",
        blurb = "Bare panel from the framed set, 16:9. 1.34 x 0.77 m.",
        quad = {
            offset = { 0.0, 0.000005, 0.567307 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.3415,
            height = 0.7715,
        },
    },
    {
        id = "tv.screen.neokitsch.21x9",
        label = "TV screen, neokitsch 21:9",
        model = "electronics.tv.screen.neokitsch.21x9",
        blurb = "Bare panel from the framed set, ultrawide. 1.04 x 0.43 m.",
        quad = {
            offset = { 0.0, 0.000005, 0.398807 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.0415,
            height = 0.4345,
        },
    },

    -- -------------------------------------------------------------------------
    -- Monitors: the screen meshes the game's monitor props draw
    -- -------------------------------------------------------------------------
    -- Each monitor family has a landscape mesh and its own portrait mesh; the
    -- portrait one is a separate asset, not a rotation. Both are full screens, so
    -- the rectangle is the whole mesh -- and the mesh's normal is X, not Y, which
    -- is why `right` is (0,1,0) below. That is the mesh talking, not a preference.
    {
        id = "monitor.a",
        label = "Monitor, small",
        model = "electronics.monitor.screen.a",
        blurb = "Compact 4:3-ish screen. 0.53 x 0.39 m.",
        quad = {
            offset = { -0.070891, -0.000470, -0.000112 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.5275,
            height = 0.3948,
        },
    },
    {
        id = "monitor.a.vertical",
        label = "Monitor, small, portrait",
        model = "electronics.monitor.screen.a.vertical",
        blurb = "The same panel stood on end. 0.40 x 0.53 m.",
        quad = {
            offset = { -0.070897, 0.000000, 0.000116 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.3950,
            height = 0.5282,
        },
    },
    {
        id = "monitor.b",
        label = "Monitor, medium",
        model = "electronics.monitor.screen.b",
        blurb = "The office monitor. 0.81 x 0.60 m.",
        quad = {
            offset = { -0.107116, -0.000691, -0.000266 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.8062,
            height = 0.5966,
        },
    },
    {
        id = "monitor.b.vertical",
        label = "Monitor, medium, portrait",
        model = "electronics.monitor.screen.b.vertical",
        blurb = "The office monitor, portrait. 0.60 x 0.81 m.",
        quad = {
            offset = { -0.107116, 0.000000, 0.000176 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.5972,
            height = 0.8072,
        },
    },
    {
        id = "monitor.c",
        label = "Monitor, ultrawide",
        model = "electronics.monitor.screen.c",
        blurb = "Ultrawide wall monitor. 1.40 x 0.60 m.",
        quad = {
            offset = { -0.104362, 0.000000, -0.001979 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.4031,
            height = 0.5970,
        },
    },
    {
        id = "monitor.c.vertical",
        label = "Monitor, ultrawide, portrait",
        model = "electronics.monitor.screen.c.vertical",
        blurb = "Ultrawide turned on end. 0.60 x 1.40 m.",
        quad = {
            offset = { -0.104362, 0.001979, 0.000000 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.5970,
            height = 1.4031,
        },
    },
    {
        id = "monitor.d",
        label = "Monitor, wide",
        model = "electronics.monitor.screen.d",
        blurb = "Wide monitor for a desk or a wall. 0.98 x 0.42 m.",
        quad = {
            offset = { -0.072952, 0.000000, -0.001333 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.9808,
            height = 0.4173,
        },
    },
    {
        id = "monitor.d.vertical",
        label = "Monitor, wide, portrait",
        model = "electronics.monitor.screen.d.vertical",
        blurb = "The wide monitor, portrait. 0.42 x 0.98 m.",
        quad = {
            offset = { -0.072952, 0.001333, 0.000000 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.4173,
            height = 0.9808,
        },
    },

    -- -------------------------------------------------------------------------
    -- Device panels: a monitor with its housing
    -- -------------------------------------------------------------------------
    -- `monitor_device_*` is the whole monitor (screen plus bezel plus 7-11 cm of
    -- depth), not a bare screen. The page goes on the measured front face -- X at
    -- the box's maximum -- because a rectangle at the mesh's centre would be
    -- buried inside the housing and never seen.
    {
        id = "device.a",
        label = "Device panel, small",
        model = "electronics.monitor.device.a",
        blurb = "Small housed monitor. Front face 0.54 x 0.40 m.",
        quad = {
            offset = { 0.002874, 0.000081, -0.000122 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.5403,
            height = 0.4030,
        },
    },
    {
        id = "device.b",
        label = "Device panel, medium",
        model = "electronics.monitor.device.b",
        blurb = "Housed monitor. Front face 0.82 x 0.60 m.",
        quad = {
            offset = { 0.002971, 0.000000, -0.000329 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.8161,
            height = 0.6043,
        },
    },
    {
        id = "device.c",
        label = "Device panel, ultrawide",
        model = "electronics.monitor.device.c",
        blurb = "Housed ultrawide. Front face 1.42 x 0.60 m.",
        quad = {
            offset = { 0.002268, -0.000694, -0.000329 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.4159,
            height = 0.6043,
        },
    },
    {
        id = "device.d",
        label = "Device panel, wide",
        model = "electronics.monitor.device.d",
        blurb = "Housed wide monitor. Front face 0.99 x 0.43 m.",
        quad = {
            offset = { 0.006129, -0.000542, 0.002458 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.9929,
            height = 0.4271,
        },
    },
    {
        id = "device.e",
        label = "Device panel, large",
        model = "electronics.monitor.device.e",
        blurb = "Large housed display. Front face 1.42 x 0.79 m.",
        quad = {
            offset = { 0.002268, 0.000000, -0.003122 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.4154,
            height = 0.7897,
        },
    },

    -- -------------------------------------------------------------------------
    -- Panels: screens with no housing at all
    -- -------------------------------------------------------------------------
    -- Flat, zero thickness, lying exactly on their own X=0 plane. These are what
    -- the game uses for a wall display, an advertising face or an ATM -- and the
    -- shapes are the game's own 21x9, 16x9, 4x3, 3x4, 9x16, 9x21 and 2:1.
    {
        id = "panel.21x9",
        label = "Panel, 21:9",
        model = "electronics.screen.21x9",
        blurb = "Bare ultrawide panel. 0.63 x 0.27 m.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.6251,
            height = 0.2679,
        },
    },
    {
        id = "panel.16x9",
        label = "Panel, 16:9",
        model = "electronics.screen.16x9",
        blurb = "Bare 16:9 panel. 0.49 x 0.28 m.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.4901,
            height = 0.2757,
        },
    },
    {
        id = "panel.4x3",
        label = "Panel, 4:3",
        model = "electronics.screen.4x3",
        blurb = "Bare 4:3 panel. 0.29 x 0.22 m.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.2909,
            height = 0.2181,
        },
    },
    {
        id = "panel.9x21",
        label = "Panel, 9:21 portrait",
        model = "electronics.screen.9x21",
        blurb = "Bare ultrawide stood on end. 0.27 x 0.63 m.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.2679,
            height = 0.6251,
        },
    },
    {
        id = "panel.3x4",
        label = "Panel, 3:4 portrait",
        model = "electronics.screen.3x4",
        blurb = "Bare portrait panel. 0.22 x 0.29 m.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.2181,
            height = 0.2909,
        },
    },
    {
        id = "panel.9x16",
        label = "Panel, 9:16 portrait",
        model = "electronics.screen.9x16",
        blurb = "Bare portrait 16:9. 0.28 x 0.49 m.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.2757,
            height = 0.4901,
        },
    },
    {
        id = "panel.2x1",
        label = "Panel, 2:1",
        model = "electronics.screen.2x1",
        blurb = "Plainest screen in the game. 0.80 x 0.40 m.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.8000,
            height = 0.4000,
        },
    },

    -- -------------------------------------------------------------------------
    -- Frames: the game's picture frames, which is how Night City advertises
    -- -------------------------------------------------------------------------
    -- Zero thickness, lying on their own Y=0 plane, and genuinely assorted: two
    -- squares, a 21:9 and three portraits.
    {
        id = "frame.a",
        label = "Frame, square",
        model = "electronics.frame.a",
        blurb = "Square frame, 1.02 m. A poster face.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.0194,
            height = 1.0208,
        },
    },
    {
        id = "frame.ab",
        label = "Frame, tall",
        model = "electronics.frame.ab",
        blurb = "Tall portrait frame, 0.87 x 2.00 m. A full-length ad.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.8682,
            height = 2.0036,
        },
    },
    {
        id = "frame.ac",
        label = "Frame, narrow tall",
        model = "electronics.frame.ac",
        blurb = "Narrow portrait frame, 0.49 x 1.13 m. A pillar sign.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.4876,
            height = 1.1332,
        },
    },
    {
        id = "frame.ad",
        label = "Frame, ultrawide",
        model = "electronics.frame.ad",
        blurb = "Wide frame, 1.91 x 0.81 m. Billboard shaped.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.9057,
            height = 0.8127,
        },
    },
    {
        id = "frame.ae",
        label = "Frame, large square",
        model = "electronics.frame.ae",
        blurb = "Large square frame, 1.17 m. The one for a shop window.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 1.1705,
            height = 1.1705,
        },
    },
    {
        id = "frame.af",
        label = "Frame, portrait",
        model = "electronics.frame.af",
        blurb = "Portrait frame, 0.83 x 1.17 m.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.8299,
            height = 1.1715,
        },
    },

    -- -------------------------------------------------------------------------
    -- One special case
    -- -------------------------------------------------------------------------
    {
        id = "surveillance",
        label = "Security monitor",
        model = "electronics.monitor.surveillance",
        blurb = "Housed security monitor. Front face 0.55 x 0.38 m.",
        quad = {
            -- hardware_surveillance_monitor_c: 0.3377 m of depth along X with the
            -- front at X = 0.168836, so the page sits on that face.
            offset = { 0.168836, 0.000000, 0.188592 },
            right = { 0.0, 1.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.5517,
            height = 0.3774,
        },
    },
}

-- -----------------------------------------------------------------------------
-- Deliberately NOT in the catalogue, and why
-- -----------------------------------------------------------------------------
-- The inventory has more screen-shaped meshes than appear above. They are
-- excluded because a page on them would be wrong, not because they are hard:
--
--   arasaka_monitor_a_screen_a   a 2.0 x 1.5 m wall of four monitors, not one
--                                screen; one page across it would span the gaps
--   int_nkt_apartment_a_tv_panel a 10.4 m apartment wall the screen is inset into
--   monitor_b_screen_b           byte-identical box to monitor_b_screen, already
--                                offered as `monitor.b`
--   baron_monitor_*_screen       byte-identical to the monitor_*_screen meshes
--   q304_blackwall_*_monitor_*   byte-identical to monitor_a/monitor_c screens
--   stadium_black_market_*       byte-identical to monitor_a_screen
--   q110_tv_screen               byte-identical to television_a_16x9_screen_a
--   emissive_cube_tv_screen      a 1 m cube used as a light, not a face
--   decoset_* / tv_frame_a       set dressing and a bracket: no screen
--   tv_stand_base/mount/stick    furniture. `furniture.tv_stand` puts a
--                                television on it; the stand itself shows
--                                nothing
-- -----------------------------------------------------------------------------
-- WHICH RECORDS DECLARE A FRONT, AND WHICH DO NOT
-- -----------------------------------------------------------------------------
-- Declared (`faces`, picture only from that side -- the television family, all
-- of it measured against the mesh as described above):
--
--   tv.16x9   tv.21x9   tv.large   tv.screen.16x9   tv.screen.21x9
--
-- Undeclared, deliberately, and why -- each of these still draws from both
-- sides, and `media.list` reports `facing.source=undeclared` for it:
--
--   tv.neokitsch.16x9 / .21x9 and their bare screens
--       the glass is a zero-thickness plane lying exactly on the frame's own
--       mid-plane (screen mesh Y 0.0000, frame body Y -0.0150..+0.0150), so the
--       asset itself does not say which side the picture is on. Declaring one
--       would be a coin flip that a wrong answer turns into a blank set.
--   monitor.*
--       the same arithmetic says the glass is 7-11 cm to -X of the mesh origin
--       (monitor_a_screen X -0.0754..-0.0663 around an origin at X 0), i.e. the
--       picture faces -X -- but a bare screen mesh is a 9 mm slab whose two
--       faces are the same surface, so there is nothing to hide from behind and
--       no gain in gating it. Left undeclared rather than half-argued.
--   device.*
--       ambiguous, and worth saying so: the quad is the measured FRONT FACE of
--       the housing (monitor_device_a X -0.0714..+0.0029, page on +X), while the
--       monitor_device_*_screen_a mesh sits at the housing's -X end. The two
--       disagree, and until that is settled against the asset in game, gating
--       this family would risk blanking a monitor rather than fixing one.
--   panel.*, frame.*
--       zero-thickness, centred on their own origin, no housing: both sides are
--       the same surface.
--   surveillance
--       housing is symmetric about its origin (X -0.1688..+0.1688).

for _, record in ipairs(records) do
    Open77MediaRecords[record.id] = record
end

---The catalogue in a stable order, which is the order the menu lists it in.
---@return Open77MediaRecord[]
function Open77MediaCatalogue()
    local out = {}
    for _, record in ipairs(records) do out[#out + 1] = record end
    return out
end

---One record by id, or nil.
---@param id string
---@return Open77MediaRecord|nil
function Open77MediaRecord(id)
    return Open77MediaRecords[tostring(id)]
end

-- -----------------------------------------------------------------------------
-- Surface size for a screen rectangle
-- -----------------------------------------------------------------------------
-- The page on a television is a CEF surface mapped 1:1 onto the quad with UVs
-- 0..1, so a surface whose aspect differs from the quad's stretches the picture.
-- The aspect that is right is the rectangle's own, which is why this is derived
-- from the quad rather than stored per record: the server is free to retune a
-- quad with `media.quad`, and the surface then follows the rectangle it is
-- actually drawn on instead of the one the catalogue shipped with.
--
-- The long side is 1280 -- the same surface the game's own menu pages use, and
-- comfortably inside the host's maximum surface dimension. The short side keeps
-- the aspect to within one pixel of exact at every ratio in the catalogue (the
-- widest is 9:21 at 0.43, the squarest is 1.0), and is floored so that a hand
-- authored 20:1 billboard still gets a rectangle rather than a line.
Open77MediaSurfaceLongSide = 1280
Open77MediaSurfaceShortSideMinimum = 160

---The CEF surface size a screen rectangle should be drawn on.
---@param quad table  the rectangle as it is on the wire
---@return integer width, integer height
function Open77MediaSurfaceFor(quad)
    local width = tonumber(quad and quad.width) or 0
    local height = tonumber(quad and quad.height) or 0
    if not (width > 0) or not (height > 0) then
        -- A degenerate quad is refused by the native side anyway; returning the
        -- default keeps the caller from having to special-case it.
        return Open77MediaSurfaceLongSide, 720
    end

    local aspect = width / height
    local long = Open77MediaSurfaceLongSide
    local short = math.floor(long / (aspect > 1 and aspect or (1 / aspect)) + 0.5)
    if short < Open77MediaSurfaceShortSideMinimum then
        short = Open77MediaSurfaceShortSideMinimum
    end
    -- Even numbers: the surface is uploaded as a texture, and an odd dimension
    -- buys nothing at these sizes.
    short = short - (short % 2)
    if aspect >= 1 then return long, short end
    return short, long
end
