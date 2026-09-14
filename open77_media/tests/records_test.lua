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
--   * an aspect ratio that is not a shape a screen could be, and an id whose
--     `NxM` claim disagrees with the rectangle it is attached to -- the surface is
--     sized from the rectangle, so a wrong number becomes a stretched picture;
--   * a screen rectangle that is not inside the cabinet of the television it is
--     attached to, which is what "the two meshes share an origin" actually means
--     and the only thing keeping the pairing honest;
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

    -- The surface a screen is drawn on is derived from this rectangle
    -- (`Open77MediaSurfaceFor`), so the aspect is no longer pinned to 16:9 -- it
    -- just has to be a ratio a screen could plausibly be. The widest record in
    -- the catalogue is 9:21 at 0.43 and the squarest is 1.0, so the band below is
    -- wide enough to admit every real screen in the game and narrow enough to
    -- catch a transposed width and height (which would be 2.33 where 0.43 belongs).
    if finite(quad.width) and finite(quad.height) and quad.height > 0 then
        local aspect = quad.width / quad.height
        check(aspect >= 0.40 and aspect <= 2.50,
            string.format("record '%s': quad aspect is a real screen shape (got %.3f)",
                record.id, aspect))
    end
end

-- =============================================================================
-- The id's shape claim
-- =============================================================================
-- Half the catalogue names its shape -- `panel.21x9`, `tv.screen.16x9`,
-- `monitor.4x3` -- and that name is what a person picks a screen by. So the name
-- is treated as a claim about the measured rectangle: where the id ends in a
-- `NxM` token, the quad must be that shape.
--
-- The tolerance is 5% because the game's own authored screens are approximate:
-- `television_a_16x9_screen_a` measures 1.16 x 0.66 m, which is 1.758 rather than
-- 1.778. Five percent still fails a swapped width and height, or a number typed
-- into the wrong record, which is what this is for.
for _, record in ipairs(catalogue) do
    local shapeWidth, shapeHeight =
        string.match(record.id, "([0-9]+)x([0-9]+)$")
    if shapeWidth then
        local claimed = tonumber(shapeWidth) / tonumber(shapeHeight)
        local measured = record.quad.width / record.quad.height
        check(math.abs(measured - claimed) <= 0.05 * claimed,
            string.format("record '%s': quad matches the %sx%s its id claims (measured %.3f, claimed %.3f)",
                record.id, shapeWidth, shapeHeight, measured, claimed))
    end
end

-- =============================================================================
-- A screen inside its own television
-- =============================================================================
-- For a set whose screen is a separate mesh (`television_a_16x9` plus
-- `television_a_16x9_screen_a`), the rectangle is taken from the screen mesh's
-- box and applied to the BODY's prop. That is only valid while the two meshes
-- share an origin, and sharing an origin is visible as containment: the screen's
-- box sits strictly inside the body's.
--
-- The boxes are the ones measured out of the 2.31 archive (the method is in the
-- header of `shared/records.lua`). Pinning them here is deliberate: a re-authored
-- asset that moves a screen would otherwise paint outside the cabinet, and the
-- only symptom in game would be a rectangle floating in the air.

local televisions = {
    ["tv.16x9"] = {
        min = { -0.630500, -0.050500, -0.001689 },
        max = { 0.630500, 0.125500, 0.851043 },
    },
    ["tv.21x9"] = {
        min = { -0.841015, -0.051400, -0.001265 },
        max = { 0.840886, 0.125500, 0.851162 },
    },
    ["tv.neokitsch.16x9"] = {
        min = { -0.688592, -0.014968, -0.000348 },
        max = { 0.688591, 0.014968, 0.972867 },
    },
    ["tv.neokitsch.21x9"] = {
        min = { -0.539344, -0.015082, -0.000779 },
        max = { 0.539344, 0.015082, 0.636543 },
    },
}

local function insideEnvelope(record, box)
    local quad = record.quad
    for component = 1, 3 do
        -- The rectangle's own extent along this axis: `right` and `up` are unit
        -- axis vectors in this catalogue, so a component contributes its half
        -- extent on the axes where it is non-zero.
        local reach = math.abs(quad.right[component]) * quad.width * 0.5
            + math.abs(quad.up[component]) * quad.height * 0.5
        local low = quad.offset[component] - reach
        local high = quad.offset[component] + reach
        check(low >= box.min[component] - 0.002 and high <= box.max[component] + 0.002,
            string.format(
                "record '%s': screen lies inside the television body on axis %d (%.4f..%.4f within %.4f..%.4f)",
                record.id, component, low, high, box.min[component], box.max[component]))
    end
end

local sets = 0
for id, box in pairs(televisions) do
    local record = Open77MediaRecord(id)
    check(record ~= nil, "the catalogue still has " .. id)
    if record ~= nil then
        sets = sets + 1
        insideEnvelope(record, box)
    end
end
check(sets == 4, "the four television bodies with a separate screen mesh are all present")

-- =============================================================================
-- Which side of the panel carries the picture
-- =============================================================================
-- A rectangle has two sides and a projection does not distinguish them, so a
-- record that says nothing about it plays its video on the back of the cabinet
-- when the player walks round behind the set -- reported, exactly, as "tvs are
-- playing videos on both sides".
--
-- This is the test that keeps the declarations honest in both directions:
--
--   * the gated set is pinned by name, so a record that gains or loses a `faces`
--     declaration has to say so here rather than changing behaviour quietly;
--   * a declared front has to be a real direction (the native side reads
--     anything under 1e-4 as "not declared", so a near-zero one would be a
--     silent no-op rather than a refusal);
--   * it has to be the panel's NORMAL -- perpendicular to `right` and `up`. A
--     declaration that is not is a gate on a direction the panel does not face,
--     which hides the picture from the side it belongs on and shows it on the
--     side it does not;
--   * and it has to agree with the measurement that produced it.

local declaredFronts = {
    -- id                = the direction the asset says the picture looks along
    ["tv.16x9"] = { 0.0, 1.0, 0.0 },
    ["tv.21x9"] = { 0.0, 1.0, 0.0 },
    ["tv.large"] = { 0.0, 1.0, 0.0 },
    ["tv.screen.16x9"] = { 0.0, 1.0, 0.0 },
    ["tv.screen.21x9"] = { 0.0, 1.0, 0.0 },
}

local gated = 0
for _, record in ipairs(catalogue) do
    local faces = record.quad.faces
    local expected = declaredFronts[record.id]
    if expected == nil then
        check(faces == nil,
            string.format("record '%s' declares no front, and that is deliberate: " ..
                "the meshes that cannot say which side their glass is on are listed in " ..
                "shared/records.lua rather than guessed at", record.id))
    else
        gated = gated + 1
        check(type(faces) == "table" and #faces == 3,
            string.format("record '%s': quad.faces is three numbers", record.id))
        if type(faces) == "table" and #faces == 3 then
            for component = 1, 3 do
                check(finite(faces[component]),
                    string.format("record '%s': quad.faces[%d] is finite",
                        record.id, component))
                check(faces[component] == expected[component],
                    string.format("record '%s': quad.faces[%d] is %.1f (got %s)",
                        record.id, component, expected[component], tostring(faces[component])))
            end

            local length = math.sqrt(faces[1] ^ 2 + faces[2] ^ 2 + faces[3] ^ 2)
            check(length >= 0.001,
                string.format("record '%s': quad.faces is a direction, not a zero vector " ..
                    "(length %.6f; anything under 1e-4 reads as undeclared)", record.id, length))

            local dotRight = faces[1] * record.quad.right[1]
                + faces[2] * record.quad.right[2] + faces[3] * record.quad.right[3]
            local dotUp = faces[1] * record.quad.up[1]
                + faces[2] * record.quad.up[2] + faces[3] * record.quad.up[3]
            local rightLength = math.sqrt(record.quad.right[1] ^ 2 + record.quad.right[2] ^ 2
                + record.quad.right[3] ^ 2)
            local upLength = math.sqrt(record.quad.up[1] ^ 2 + record.quad.up[2] ^ 2
                + record.quad.up[3] ^ 2)
            check(math.abs(dotRight) <= 0.001 * length * rightLength,
                string.format("record '%s': quad.faces is perpendicular to `right` (dot %.6f)",
                    record.id, dotRight))
            check(math.abs(dotUp) <= 0.001 * length * upLength,
                string.format("record '%s': quad.faces is perpendicular to `up` (dot %.6f)",
                    record.id, dotUp))
        end
    end
end
check(gated == 5, string.format("exactly the five measured television records declare a front (got %d)", gated))

-- The measurement behind those declarations, as numbers, so the claim above can be
-- checked rather than believed. Every figure is from the 2.31 mesh data, the same
-- `quantizationOffset/Scale` read that produced every quad and every box above:
--
--   * a screen mesh inside a cabinet lies against one of its faces, so the
--     cabinet -- the thing that must not show the picture -- is behind the glass.
--     `television_a_16x9`: body Y -0.050500..+0.125500 with the screen plane at
--     Y 0.115394, 1 cm inside the +Y face.
--   * a bare screen mesh keeps the cabinet's origin, so the panel simply stands
--     in front of it: `television_a_16x9_screen_a`'s own box is Y 0.115000..
--     0.115788 around an origin at Y 0.
--   * `tv.large` has no separate screen mesh, so its rectangle IS the +Y face of
--     `tv_large_a` (body Y -0.115513..+0.111389), which is the face a wall display
--     is mounted by.
local pictureSide = {
    { id = "tv.16x9", bodyMin = -0.050500, bodyMax = 0.125500 },
    { id = "tv.21x9", bodyMin = -0.051400, bodyMax = 0.125500 },
    { id = "tv.large", bodyMin = -0.115513, bodyMax = 0.111389 },
    { id = "tv.screen.16x9", bodyMin = 0.000000, bodyMax = 0.115788 },
    { id = "tv.screen.21x9", bodyMin = 0.000000, bodyMax = 0.115788 },
}
for _, measured in ipairs(pictureSide) do
    local record = Open77MediaRecord(measured.id)
    check(record ~= nil, "the catalogue still has " .. measured.id)
    if record ~= nil and type(record.quad.offset) == "table" then
        local plane = record.quad.offset[2]
        check(math.abs(plane) > 0.01,
            string.format("record '%s': the picture plane is off the origin along Y (%.6f)",
                measured.id, plane))
        check(plane <= measured.bodyMax + 0.002 and plane >= measured.bodyMin - 0.002,
            string.format("record '%s': the picture plane is inside the body it was measured in (%.6f in %.6f..%.6f)",
                measured.id, plane, measured.bodyMin, measured.bodyMax))
        -- Close to the +Y face: the cabinet is behind the glass, so +Y is the side
        -- the picture faces -- which is what the record declares two blocks up.
        check(measured.bodyMax - plane >= -0.002 and measured.bodyMax - plane <= 0.02,
            string.format("record '%s': the glass is against the body's +Y face (%.6f from it), so the declared +Y is the side the picture is on",
                measured.id, measured.bodyMax - plane))
        check(plane - measured.bodyMin >= 0.05,
            string.format("record '%s': there is body behind the glass for the gate to hide (%.6f m)",
                measured.id, plane - measured.bodyMin))
    end
end

-- =============================================================================
-- The surface derived from the rectangle
-- =============================================================================

for _, record in ipairs(catalogue) do
    local width, height = Open77MediaSurfaceFor(record.quad)
    check(math.floor(width) == width and math.floor(height) == height,
        string.format("record '%s': surface dimensions are whole numbers", record.id))
    check(width % 2 == 0 and height % 2 == 0,
        string.format("record '%s': surface dimensions are even", record.id))
    check(math.max(width, height) == Open77MediaSurfaceLongSide,
        string.format("record '%s': the surface's long side is %d",
            record.id, Open77MediaSurfaceLongSide))
    check(math.min(width, height) > Open77MediaSurfaceShortSideMinimum,
        string.format("record '%s': the short side is above the floor, so no record in the catalogue is clamping",
            record.id))

    -- The short side is rounded to the nearest pixel and then down to an even
    -- one, so the only error is sub-pixel: the aspect it would need to be exact,
    -- measured in short-side pixels, is within one and a half of what it is.
    -- Stating it in pixels rather than as an aspect tolerance is the difference
    -- between a check a wrong number fails and one it can hide inside.
    local quad = record.quad.width / record.quad.height
    local short = math.min(width, height)
    -- Landscape keeps the long side as the width and divides; portrait keeps it as
    -- the height and multiplies.
    local exact = quad >= 1 and (Open77MediaSurfaceLongSide / quad)
        or (Open77MediaSurfaceLongSide * quad)
    check(math.abs(short - exact) <= 1.5,
        string.format("record '%s': the surface keeps the rectangle's aspect to within a pixel (%d vs %.1f)",
            record.id, short, exact))
end

-- Degenerate input, which the native side refuses anyway: the helper answers the
-- default rather than a division by zero.
local fallbackWidth, fallbackHeight = Open77MediaSurfaceFor({ width = 0, height = 0 })
check(fallbackWidth > 0 and fallbackHeight > 0,
    "a degenerate quad still yields a usable surface")
local extremeWidth, extremeHeight = Open77MediaSurfaceFor({ width = 20.0, height = 1.0 })
check(extremeWidth == Open77MediaSurfaceLongSide
        and extremeHeight == Open77MediaSurfaceShortSideMinimum,
    string.format("a 20:1 rectangle is floored rather than drawn as a line (got %dx%d)",
        extremeWidth, extremeHeight))

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
