-- =============================================================================
-- open77_media -- tests/records_test.lua
-- =============================================================================
-- The catalogue is the one part of this feature that cannot report its own
-- mistakes. A record naming a prop that does not exist builds a television whose
-- prop never spawns: the screen is bound correctly, projects correctly, and is
-- simply never seen, because the entity it hangs off was refused at creation.
-- Nothing in the engine, the server or the client says so -- the failure is a
-- rectangle that never appears.
--
-- So the checks below are about the ways a catalogue goes wrong silently:
--
--   * a model alias that is a typo, checked against the alias table the props
--     commands already publish (`Open77AdminConfig.props.models`, mirrored from
--     the client's own `kModelAliases` in Props.cpp);
--   * a duplicate id, where the second record overwrites the first and the menu
--     silently lists seven records out of eight;
--   * a quad whose axes are parallel, which collapses the screen to a line;
--   * an aspect ratio that does not match the surface the client creates, which
--     stretches every pixel on that television;
--   * a width or height of zero or negative, which the native side refuses --
--     correctly, but with the screen simply absent.
--
-- Standalone:  lua tools/lua-test/run.lua <repo-root>
-- =============================================================================

local passed = 0
local failures = {}
local function check(condition, message)
    if condition then
        passed = passed + 1
    else
        failures[#failures + 1] = message or "assertion failed"
    end
end

local catalogue = Open77MediaCatalogue()

-- =============================================================================
-- Shape
-- =============================================================================

check(type(Open77MediaRecords) == "table", "Open77MediaRecords is a table")
check(type(catalogue) == "table" and #catalogue > 0, "the catalogue is not empty")

local required = { "id", "label", "blurb", "model" }
for index, record in ipairs(catalogue) do
    for _, field in ipairs(required) do
        check(type(record[field]) == "string" and #record[field] > 0,
            string.format("record %d: '%s' is a non-empty string", index, field))
    end
    check(type(record.quad) == "table", string.format("record %d has a quad", index))
end

-- =============================================================================
-- Ids
-- =============================================================================

local seen = {}
for _, record in ipairs(catalogue) do
    check(seen[record.id] == nil, "duplicate record id: " .. tostring(record.id))
    seen[record.id] = true
    check(Open77MediaRecord(record.id) == record,
        "Open77MediaRecord finds " .. tostring(record.id))
end
check(Open77MediaRecord("no.such.record") == nil, "an unknown id returns nil")

-- =============================================================================
-- Models exist in the props alias table
-- =============================================================================
-- This is the cross-check worth having. The alias list in the admin config is
-- maintained by hand alongside the client's C++ table, and a television whose
-- model is not in that list is either a typo here or a missing alias there --
-- both of which are real bugs, and neither of which the game reports.

local models = {}
if type(Open77AdminConfig) == "table" and type(Open77AdminConfig.props) == "table"
    and type(Open77AdminConfig.props.models) == "table" then
    for _, name in ipairs(Open77AdminConfig.props.models) do models[name] = true end
    check(next(models) ~= nil, "the props alias table is not empty")
    for _, record in ipairs(catalogue) do
        check(models[record.model] == true,
            string.format("record '%s' names a prop alias that exists: %s",
                record.id, tostring(record.model)))
    end
else
    -- Not a silent pass. If the alias table is not loadable the check above did
    -- not run, and reporting that is the difference between a suite that proves
    -- something and one that proves it did nothing.
    check(false, "Open77AdminConfig.props.models is unavailable: the model check did not run")
end

-- =============================================================================
-- Quad geometry
-- =============================================================================

local function finite(value)
    return type(value) == "number" and value == value
        and value ~= math.huge and value ~= -math.huge
end

local function axisIsUsable(record, name)
    local axis = record.quad[name]
    if type(axis) ~= "table" or #axis ~= 3 then
        check(false, string.format("record '%s': quad.%s is three numbers", record.id, name))
        return nil
    end
    for component = 1, 3 do
        check(finite(axis[component]),
            string.format("record '%s': quad.%s[%d] is finite", record.id, name, component))
    end
    return axis
end

for _, record in ipairs(catalogue) do
    local quad = record.quad
    check(finite(quad.width) and quad.width >= 0.05 and quad.width <= 5.0,
        string.format("record '%s': width is 0.05..5 m (got %s)",
            record.id, tostring(quad.width)))
    check(finite(quad.height) and quad.height >= 0.05 and quad.height <= 5.0,
        string.format("record '%s': height is 0.05..5 m (got %s)",
            record.id, tostring(quad.height)))

    local offset = axisIsUsable(record, "offset")
    local right = axisIsUsable(record, "right")
    local up = axisIsUsable(record, "up")

    if offset ~= nil then
        for component = 1, 3 do
            check(math.abs(offset[component]) <= 10.0,
                string.format("record '%s': quad.offset[%d] is within 10 m of the prop",
                    record.id, component))
        end
    end

    if right ~= nil and up ~= nil then
        -- Neither axis needs to be unit length -- the native side normalises
        -- them, deliberately, so a record can write a raw direction for a tilted
        -- panel. What must not happen is the two being parallel: a quad with a
        -- zero cross product has no area, and the screen renders as a line.
        local cx = right[2] * up[3] - right[3] * up[2]
        local cy = right[3] * up[1] - right[1] * up[3]
        local cz = right[1] * up[2] - right[2] * up[1]
        local magnitude = math.sqrt(cx * cx + cy * cy + cz * cz)
        local rightLength = math.sqrt(right[1] ^ 2 + right[2] ^ 2 + right[3] ^ 2)
        local upLength = math.sqrt(up[1] ^ 2 + up[2] ^ 2 + up[3] ^ 2)
        check(rightLength > 0.001 and upLength > 0.001,
            string.format("record '%s': quad axes are non-zero", record.id))
        -- Sine of the angle between them, so the test is scale-free.
        check(magnitude >= 0.05 * rightLength * upLength,
            string.format("record '%s': quad axes are not parallel (sine=%.4f)",
                record.id, magnitude / math.max(1e-9, rightLength * upLength)))
    end

    -- The surface every screen creates is 1280x720. A record whose screen is not
    -- 16:9 is stretched, and the fix is a different surface size for that record
    -- rather than a squashed quad -- so it is worth catching here rather than
    -- being noticed as \"the picture looks wrong on that one\".
    if finite(quad.width) and finite(quad.height) and quad.height > 0 then
        local aspect = quad.width / quad.height
        check(math.abs(aspect - (16 / 9)) <= 0.02,
            string.format("record '%s': quad is 16:9 (got %.3f)", record.id, aspect))
    end
end

-- =============================================================================
-- Identity
-- =============================================================================
-- The returned array is rebuilt per call, and the quads are per-record tables.
-- The server deep-copies a quad before handing it to a screen precisely so one
-- television tuning itself cannot move every other television using the same
-- record; if the catalogue shared one quad table between records that copy would
-- be hiding a different bug rather than fixing one.

local first = Open77MediaCatalogue()
local second = Open77MediaCatalogue()
check(first ~= second, "the catalogue returns a new array per call")
check(#first == #second, "the catalogue is the same length every call")
check(first[1].id == second[1].id, "the catalogue order is stable")

local quadOwners = {}
local shared = false
for _, record in ipairs(catalogue) do
    if quadOwners[record.quad] ~= nil then shared = true end
    quadOwners[record.quad] = record.id
end
check(not shared, "no two records share one quad table")

TestResult = {
    passed = passed,
    failed = #failures,
    failures = failures,
}
