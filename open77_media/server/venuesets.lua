-- =============================================================================
-- open77_media -- server/venuesets.lua
-- =============================================================================
-- Where a venue screen stands, once an operator has moved it in game.
--
-- THE PROBLEM THIS ANSWERS. `MediaVenueSets` in `server/config.lua` is a list of
-- screens that are part of the world: they are spawned at start, at the
-- coordinates written in that file. Those coordinates are a survey, taken once,
-- and the world is the only honest judge of them -- the drive-in screen shipped
-- a few metres off and the only way to find that out was to stand in front of
-- it. Before this store, lining one up meant `media.move` / `media.rotate` until
-- it looked right and then copying the numbers back into a Lua file by hand, or
-- losing the work at the next restart.
--
-- SO: the file is the seed, the store is what an operator last said, and the
-- store wins. One owner at a time, exactly as `data/adblock.json` is the live
-- ad-blocklist and `server/config.lua` is only what a fresh install starts with.
-- A placement saved here is read at start and used instead of the config row's
-- x/y/z/yaw; the rest of the row (record, url) still comes from the config,
-- because those are not things a position capture can change.
--
-- KEYED BY RECORD, and that is the one design choice worth stating. A venue set
-- is identified by its record (`cinema.150ft`), so `pinHere` does not
-- have to trust an index the panel sent: the server reads the record off the
-- entry it already holds. Two venue rows with the same record would share a
-- saved placement -- the load says so out loud rather than pretending otherwise.
--
-- THE PURE HALF. Nothing in this file touches the engine: it validates, reads a
-- hand-editable JSON value and writes the shape back out. The wiring -- reading
-- the file, the panel action, the write -- is `server/main.lua`, so this can be
-- tested with no host, no prop and no player.
-- =============================================================================

MediaVenueSetsStore = {}

local Store = MediaVenueSetsStore

Store.VERSION = 1
Store.FILE = "venue-sets.json"

local function finite(value)
    local number = tonumber(value)
    if number == nil then return nil end
    if number ~= number then return nil end
    if number == math.huge or number == -math.huge then return nil end
    return number
end

--- A finite number, or nil. Exported because the loader and the writer have to
--- agree about what a coordinate is, and two copies of that rule is how a store
--- ends up writing a number it will refuse to read back.
function Store.Finite(value)
    return finite(value)
end

---One placement, cleaned, or nil and why.
---
---`yaw` is wrapped into 0..360 rather than refused outside it: a heading is an
---angle, 400 degrees is the same wall as 40, and the panel's own turn already
---wraps. A coordinate is refused rather than clamped -- a screen clamped to the
---world's edge is a screen in the sea.
---@param raw any
---@return table|nil placement { x, y, z, yaw, bucket }
---@return string|nil why
function Store.Placement(raw)
    if type(raw) ~= "table" then return nil, "placement_must_be_a_table" end

    local x, y, z = finite(raw.x), finite(raw.y), finite(raw.z)
    if x == nil or y == nil or z == nil then
        return nil, "placement_needs_finite_x_y_z"
    end

    local yaw = finite(raw.yaw or raw.heading)
    if yaw == nil then return nil, "placement_needs_a_finite_yaw" end
    yaw = yaw % 360.0

    local bucket = finite(raw.bucket)
    if bucket ~= nil then bucket = math.floor(bucket) end

    return { x = x, y = y, z = z, yaw = yaw, bucket = bucket or 0 }
end

---Reads a stored value: `{ version, placements = { [record] = placement } }`.
---
---Tolerant in the one direction that matters: a file edited by hand, or written
---by a future version, must not stop the world's screens from coming up. A row
---this build cannot read is named and skipped, and the venue set it named falls
---back to its config coordinates -- a screen at the surveyed spot is recoverable;
---a venue with no screen because of one bad row is not.
---@param value any the decoded JSON
---@param known fun(record: string)|nil whether a record is in the catalogue
---@return table placements record -> placement
---@return string[] problems
function Store.Load(value, known)
    local placements, problems = {}, {}
    if value == nil then return placements, problems end
    if type(value) ~= "table" then
        problems[#problems + 1] = "store_is_not_a_table"
        return placements, problems
    end
    if value.version ~= nil and tonumber(value.version) ~= Store.VERSION then
        problems[#problems + 1] = "store_version_" .. tostring(value.version) ..
            "_is_not_" .. tostring(Store.VERSION) .. ": read as v" .. tostring(Store.VERSION)
    end

    local rows = value.placements
    if type(rows) ~= "table" then
        if value.placements ~= nil then problems[#problems + 1] = "placements_is_not_a_table" end
        return placements, problems
    end

    local records = {}
    for record in pairs(rows) do records[#records + 1] = record end
    table.sort(records, function(a, b) return tostring(a) < tostring(b) end)

    for _, record in ipairs(records) do
        if type(record) ~= "string" or record == "" then
            problems[#problems + 1] = "placement key is not a record id; skipped"
        elseif type(known) == "function" and not known(record) then
            problems[#problems + 1] = record .. ": not in the catalogue; skipped"
        else
            local placement, why = Store.Placement(rows[record])
            if placement == nil then
                problems[#problems + 1] = record .. ": " .. tostring(why) .. "; skipped"
            else
                placements[record] = placement
            end
        end
    end
    return placements, problems
end

---The JSON to write: one version and one sorted map of placements.
---@param placements table record -> placement
---@return table
function Store.Snapshot(placements)
    local out, records = {}, {}
    for record in pairs(placements or {}) do records[#records + 1] = record end
    table.sort(records, function(a, b) return tostring(a) < tostring(b) end)
    for _, record in ipairs(records) do
        local placement = Store.Placement(placements[record])
        if placement ~= nil then
            out[tostring(record)] = {
                x = placement.x, y = placement.y, z = placement.z,
                yaw = placement.yaw, bucket = placement.bucket,
            }
        end
    end
    return { version = Store.VERSION, placements = out }
end

---The placement a venue row should start at: the saved one when there is one,
---and the row's own numbers when there is not.
---@param placements table record -> placement
---@param record string
---@param row table the config row
---@return table { x, y, z, yaw, bucket } and a string source ("saved" or "config")
function Store.For(placements, record, row)
    local saved = placements ~= nil and placements[record] or nil
    if saved ~= nil then
        return { x = saved.x, y = saved.y, z = saved.z, yaw = saved.yaw,
            bucket = saved.bucket or 0 }, "saved"
    end
    return {
        x = finite(row.x) or 0.0,
        y = finite(row.y) or 0.0,
        z = finite(row.z) or 0.0,
        yaw = finite(row.yaw) or 0.0,
        bucket = finite(row.bucket) and math.floor(finite(row.bucket)) or 0,
    }, "config"
end

return Store
