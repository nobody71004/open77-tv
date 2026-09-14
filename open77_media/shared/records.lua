-- =============================================================================
-- open77_media -- shared/records.lua
-- =============================================================================
-- The television catalogue: for each record, the prop that is the television and
-- the rectangle on that prop that is its screen.
--
-- -----------------------------------------------------------------------------
-- WHY THE SCREEN RECTANGLE IS AUTHORED HERE
-- -----------------------------------------------------------------------------
-- The obvious alternative is to read the screen out of the prop's own geometry:
-- find the screen submesh, take its bounds. It cannot be done on this engine,
-- and the reason is worth knowing before someone tries. A vanilla prop `.ent` is
-- not spawnable here and `entMeshComponent::mesh` cannot be redirected once the
-- component is attached (docs/research/props-and-object-spawning.md), so the
-- props a screen can attach to are Open77-owned host entities whose component
-- graphs are baked at asset-build time. Reading a screen rectangle back out of
-- one would mean resolving a submesh by name per television, and a prop that
-- renamed that submesh would move the screen silently, with nothing to report.
--
-- Authored data cannot silently disagree with itself, so this is where a
-- 55-inch wall panel and a countertop monitor differ.
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
-- `right` and `up` are rotated by the prop's world orientation before use and
-- are NOT required to be unit length or exactly perpendicular -- the native side
-- normalises them and squares `up` against `right`. So a monitor tilted back on
-- a desk is `up = { 0, -0.34, 0.94 }` and nothing needs to be worked out by
-- hand.
--
-- -----------------------------------------------------------------------------
-- HONEST STATE OF THESE NUMBERS
-- -----------------------------------------------------------------------------
-- Every `model` below is an alias that already exists in the props catalogue
-- (`Api::Props::Catalog`), so none of them is invented and all of them spawn.
-- The QUADS, however, are authored estimates: the props hosts carry no published
-- screen dimension, and the only way to measure one is to bind a screen and look
-- at it in game. They are therefore expected to need a pass of tuning, which is
-- exactly why they are data here and not constants in C++.
--
-- The bridge reports what actually happened, so a wrong quad is diagnosable
-- rather than mysterious:
--
--   Open77.media.list()      -- per screen: prop, surface, drawn, reason, distance
--   media.list               -- the same, from the server console
--
-- `reason` is the gate that refused the screen on the last game tick
-- (`prop_not_projected`, `behind_camera`, `occluded`, `out_of_range`, `drawn`),
-- so "the television is blank" always has an answer.
--
-- -----------------------------------------------------------------------------
-- ASPECT RATIO
-- -----------------------------------------------------------------------------
-- Every record's width:height is 16:9, matching the surface each screen creates
-- (1280x720). The page is mapped 1:1 onto the quad with UVs 0..1, so a record
-- with a different aspect would stretch its picture -- the fix is to give that
-- record its own surface size, not to squash the quad.
-- =============================================================================

Open77MediaRecords = {}

---@class Open77MediaRecord
---@field id string          shorthand used by commands and the menu
---@field label string       what the record calls itself
---@field model string       prop alias from the props catalogue
---@field quad table         screen rectangle in the prop's local frame
---@field blurb string       one line for the menu

-- The sizes below are deliberately in the 0.2-0.8 m range rather than
-- television-sized, and that is not modesty. The props these records name are
-- device and signage meshes -- the props research measured one of the same
-- family (`vending_machine_device_a`) at 0.31 x 0.19 x 0.23 m -- so a 1.6 m quad
-- would hang its picture in the air around a screen the size of a book.
-- Oversizing is not a tuning detail here, it is the difference between a screen
-- that looks attached and one that looks broken, so the defaults stay near what
-- the props actually are and an operator who wants a billboard uses `media.quad`
-- (or a bigger prop).
--
-- `offset` is zero for every upright record: these are Open77-owned host
-- entities built around the target mesh, so the mesh's own centre IS the origin,
-- and the screen's centre is therefore the prop's centre. The arcade cabinet is
-- the one exception, because its screen is genuinely recessed and tilted.
local records = {
    {
        id = "monitor.desk",
        label = "Desk monitor",
        model = "electronics.monitor",
        blurb = "Standing screen. The default one to put on a table or a counter.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.50,
            height = 0.28,
        },
    },
    {
        id = "monitor.device",
        label = "Device panel",
        model = "electronics.monitor.device",
        blurb = "Small wall or fixture panel.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.30,
            height = 0.17,
        },
    },
    {
        id = "sign.board",
        label = "Sign board",
        model = "sign.rect.blank",
        blurb = "A blank sign face. The one that reads from across a street.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.60,
            height = 0.34,
        },
    },
    {
        id = "kiosk",
        label = "Kiosk face",
        model = "sign.kiosk_frame",
        blurb = "Framed kiosk panel.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.70,
            height = 0.39,
        },
    },
    {
        id = "arcade",
        label = "Arcade cabinet",
        model = "electronics.arcade",
        blurb = "Cabinet screen, recessed and tilted back like the real thing.",
        quad = {
            offset = { 0.0, 0.12, 0.35 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, -0.34, 0.94 },
            -- 16:9 exactly. Every surface is created at 1280x720 and mapped 1:1
            -- onto this rectangle, so a record whose aspect drifts stretches its
            -- picture rather than letterboxing it. The suite pins this.
            width = 0.48,
            height = 0.27,
        },
    },
    {
        id = "vending",
        label = "Vending machine",
        model = "electronics.vending_machine",
        blurb = "The lit panel above a vending machine's buttons.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.46,
            height = 0.25875,
        },
    },
    {
        id = "vending.small",
        label = "Vending machine, small",
        model = "electronics.vending_machine.small",
        blurb = "Compact machine, compact screen.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.30,
            height = 0.17,
        },
    },
    {
        id = "painting",
        label = "Framed panel",
        model = "decor.painting",
        blurb = "A framed panel on a wall. The discreet one.",
        quad = {
            offset = { 0.0, 0.0, 0.0 },
            right = { 1.0, 0.0, 0.0 },
            up = { 0.0, 0.0, 1.0 },
            width = 0.80,
            height = 0.45,
        },
    },
}

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
