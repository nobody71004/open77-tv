-- =============================================================================
-- open77_media -- tests/placement_test.lua
-- =============================================================================
-- Pins the arithmetic behind the placement controls: which way "left" is, what a
-- turn does to a heading, and what happens to a nudge that asks for something
-- silly.
--
-- Why this is a suite and not three lines of confidence: a nudge that moves a set
-- the wrong way is not a crash, not a log line and not an exception anywhere. It
-- is a cabinet that slides right while the operator holds LEFT, on a set that may
-- be three kilometres away, and the only instrument that can see it is a person
-- standing in front of it. The convention it depends on -- yaw 0 faces +Y and
-- grows counter-clockwise -- is the engine's, and it is the same one
-- `facingPlacement` in server/main.lua uses to put a set down in front of the
-- player, so a disagreement between the two would be visible as a television that
-- spawns behind you and then moves the wrong way.
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

local TOLERANCE = 1.0e-6
local function near(left, right, tolerance)
    return math.abs((tonumber(left) or 0) - (tonumber(right) or 0)) <= (tolerance or TOLERANCE)
end

local function at(x, y, z, bucket)
    return { x = x, y = y, z = z, bucket = bucket }
end

-- =============================================================================
-- Shape
-- =============================================================================

check(type(Open77MediaPlacement) == "table", "the placement module is published")
check(Open77MediaPlacement.Step > 0, "the default step is positive")
check(Open77MediaPlacement.RotationStep > 0, "the default turn is positive")
check(#Open77MediaPlacement.Directions == 6, "six nudge directions are offered")
for _, direction in ipairs({ "left", "right", "forward", "back", "up", "down" }) do
    check(Open77MediaPlacement.IsDirection(direction), direction .. " is a known direction")
end
for _, junk in ipairs({ "north", "", "Forward", nil, 3 }) do
    check(not Open77MediaPlacement.IsDirection(junk),
        tostring(junk) .. " is not a direction")
end
check(Open77MediaPlacement.IsTurn("left") and Open77MediaPlacement.IsTurn("right"),
    "left and right are turns")
check(not Open77MediaPlacement.IsTurn("up"), "up is not a turn")

-- =============================================================================
-- Forward is the set's own forward, on the engine's heading convention
-- =============================================================================
-- yaw 0 faces +Y, and yaw grows counter-clockwise, so:
--
--     yaw   0   faces +Y        yaw  90   faces -X
--     yaw 180   faces -Y        yaw 270   faces +X
--
-- Getting this backwards is the failure this suite exists for, so all four
-- quadrants are pinned rather than just the one the spawn path happens to use.

local forwardCases = {
    { yaw = 0, x = 0, y = 1, label = "north" },
    { yaw = 90, x = -1, y = 0, label = "west" },
    { yaw = 180, x = 0, y = -1, label = "south" },
    { yaw = 270, x = 1, y = 0, label = "east" },
}
for _, case in ipairs(forwardCases) do
    local moved = Open77MediaPlacement.Nudge(at(0, 0, 0), case.yaw, "forward", 1.0)
    check(moved ~= nil, string.format("a nudge at yaw %d is accepted", case.yaw))
    if moved ~= nil then
        check(near(moved.x, case.x) and near(moved.y, case.y) and near(moved.z, 0),
            string.format("forward at yaw %d (%s) is %.2f,%.2f (got %.2f,%.2f)",
                case.yaw, case.label, case.x, case.y, moved.x, moved.y))
    end
end

-- Right is forward turned a quarter turn clockwise: at yaw 0 the set's right is
-- +X, at yaw 90 it is +Y, and so on. A set turned to face a room must still move
-- where its own buttons say.
local rightCases = {
    { yaw = 0, x = 1, y = 0 },
    { yaw = 90, x = 0, y = 1 },
    { yaw = 180, x = -1, y = 0 },
    { yaw = 270, x = 0, y = -1 },
}
for _, case in ipairs(rightCases) do
    local moved = Open77MediaPlacement.Nudge(at(0, 0, 0), case.yaw, "right", 1.0)
    check(moved ~= nil and near(moved.x, case.x) and near(moved.y, case.y),
        string.format("right at yaw %d is %.2f,%.2f (got %s,%s)", case.yaw, case.x, case.y,
            moved and tostring(moved.x) or "nil", moved and tostring(moved.y) or "nil"))
end

-- And the two are opposites, which is what makes the pair usable: a left press
-- after a right press returns the set to where it was.
for _, yaw in ipairs({ 0, 17.5, 90, 213 }) do
    local start = at(100.0, -50.0, 3.0)
    local right = Open77MediaPlacement.Nudge(start, yaw, "right", 0.75)
    local back = Open77MediaPlacement.Nudge(right, yaw, "left", 0.75)
    check(back ~= nil and near(back.x, start.x, 1.0e-4) and near(back.y, start.y, 1.0e-4)
            and near(back.z, start.z, 1.0e-4),
        string.format("right then left is where it started at yaw %s", tostring(yaw)))
    local forward = Open77MediaPlacement.Nudge(start, yaw, "forward", 0.75)
    local again = Open77MediaPlacement.Nudge(forward, yaw, "back", 0.75)
    check(again ~= nil and near(again.x, start.x, 1.0e-4) and near(again.y, start.y, 1.0e-4),
        string.format("forward then back is where it started at yaw %s", tostring(yaw)))
end

-- =============================================================================
-- Height is the world's z, and only up/down touch it
-- =============================================================================

local lifted = Open77MediaPlacement.Nudge(at(4.0, 5.0, 6.0), 45.0, "up", 2.0)
check(lifted ~= nil and near(lifted.x, 4.0) and near(lifted.y, 5.0) and near(lifted.z, 8.0),
    "up raises the set without moving it sideways")
local dropped = Open77MediaPlacement.Nudge(at(4.0, 5.0, 6.0), 45.0, "down", 2.0)
check(dropped ~= nil and near(dropped.x, 4.0) and near(dropped.y, 5.0) and near(dropped.z, 4.0),
    "down lowers the set without moving it sideways")

for _, direction in ipairs({ "left", "right", "forward", "back" }) do
    local moved = Open77MediaPlacement.Nudge(at(4.0, 5.0, 6.0), 231.0, direction, 3.0)
    check(moved ~= nil and near(moved.z, 6.0),
        string.format("a sideways nudge (%s) does not change the height", direction))
end

-- =============================================================================
-- Distance: defaulted, honoured, clamped, and refused
-- =============================================================================

local defaulted = Open77MediaPlacement.Nudge(at(0, 0, 0), 0, "forward")
check(defaulted ~= nil and near(defaulted.y, Open77MediaPlacement.Step),
    "a nudge with no distance uses the default step")

local explicit = Open77MediaPlacement.Nudge(at(0, 0, 0), 0, "forward", 12.5)
check(explicit ~= nil and near(explicit.y, 12.5), "an explicit distance is applied")

local clamped = Open77MediaPlacement.Nudge(at(0, 0, 0), 0, "forward",
    Open77MediaPlacement.MaximumStep * 100)
check(clamped ~= nil and near(clamped.y, Open77MediaPlacement.MaximumStep),
    string.format("a distance past the ceiling is clamped to %.1f m",
        Open77MediaPlacement.MaximumStep))

for _, bad in ipairs({ 0, -1, -0.25 }) do
    local moved, reason = Open77MediaPlacement.Nudge(at(0, 0, 0), 0, "forward", bad)
    check(moved == nil and reason == "distance_must_be_positive",
        string.format("a distance of %s is refused (%s)", tostring(bad), tostring(reason)))
end

local nowhere, noPosition = Open77MediaPlacement.Nudge(nil, 0, "forward")
check(nowhere == nil and noPosition == "no_position", "a nudge with no position is refused")
local sideways, unknown = Open77MediaPlacement.Nudge(at(0, 0, 0), 0, "sideways")
check(sideways == nil and unknown == "unknown_direction",
    "an invented direction is refused by name")

local tooHigh = Open77MediaPlacement.Nudge(at(0, 0, Open77MediaPlacement.MaximumHeight), 0, "up")
check(tooHigh == nil, "a set cannot be raised out of the world")
local tooLow = Open77MediaPlacement.Nudge(at(0, 0, Open77MediaPlacement.MinimumHeight), 0, "down")
check(tooLow == nil, "a set cannot be lowered out of the world")

-- =============================================================================
-- The caller's table is not the one that moves
-- =============================================================================
-- The server applies a nudge by patching the prop's transform and only then
-- records the new position. If this returned the caller's own table mutated, a
-- refused `setTransform` would leave the entry -- the only view `media.list` has
-- -- reporting a set standing where the prop never went.

local original = at(1.0, 2.0, 3.0, 42)
local moved = Open77MediaPlacement.Nudge(original, 0, "forward", 1.0)
check(near(original.x, 1.0) and near(original.y, 2.0) and near(original.z, 3.0),
    "the caller's position is untouched")
check(moved ~= original, "the nudge returns a new table")
check(moved.bucket == 42, "the routing bucket is carried through a nudge")
check(Open77MediaPlacement.Nudge(at(1, 2, 3, nil), 0, "up", 1.0).bucket == nil,
    "a position with no bucket stays without one")

-- =============================================================================
-- Turning
-- =============================================================================

local left = Open77MediaPlacement.Turn(10.0, "left", 15.0)
check(left ~= nil and near(left, 25.0), "turning left adds degrees")
local right = Open77MediaPlacement.Turn(10.0, "right", 15.0)
check(right ~= nil and near(right, 355.0),
    string.format("turning right subtracts and wraps (got %s)", tostring(right)))

local defaultTurn = Open77MediaPlacement.Turn(0.0, "left")
check(defaultTurn ~= nil and near(defaultTurn, Open77MediaPlacement.RotationStep),
    "a turn with no angle uses the default")

local wrapped = Open77MediaPlacement.Turn(350.0, "left", 20.0)
check(wrapped ~= nil and near(wrapped, 10.0), "a turn past north wraps rather than refusing")
local negative = Open77MediaPlacement.Turn(0.0, "right", 30.0)
check(negative ~= nil and near(negative, 330.0), "a heading never goes negative")

local clampedTurn = Open77MediaPlacement.Turn(0.0, "left", Open77MediaPlacement.MaximumTurn * 10)
check(clampedTurn ~= nil and near(clampedTurn, Open77MediaPlacement.MaximumTurn),
    "a turn past the ceiling is clamped")

for _, bad in ipairs({ 0, -5 }) do
    local turned, reason = Open77MediaPlacement.Turn(0.0, "left", bad)
    check(turned == nil and reason == "degrees_must_be_positive",
        string.format("a turn of %s degrees is refused", tostring(bad)))
end
local spin, spinReason = Open77MediaPlacement.Turn(0.0, "sideways")
check(spin == nil and spinReason == "unknown_turn", "an invented turn is refused by name")

check(Open77MediaPlacement.Wrap(360.0) == 0.0, "a full turn comes back as zero")
check(near(Open77MediaPlacement.Wrap(-10.0), 350.0), "a negative heading wraps forward")
check(near(Open77MediaPlacement.Wrap(725.0), 5.0), "a heading beyond two turns wraps")

-- Four quarter turns on the spot return the heading to where it started, which
-- is the property the menu's TURN button relies on when it is held.
local heading = 37.0
for _ = 1, 4 do
    heading = Open77MediaPlacement.Turn(heading, "left", 90.0)
end
check(near(heading, 37.0), "four quarter turns return the heading")

TestResult = {
    passed = passed,
    failed = #failures,
    failures = failures,
}
