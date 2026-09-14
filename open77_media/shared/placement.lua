-- =============================================================================
-- open77_media -- shared/placement.lua
-- =============================================================================
-- Where "nudge this television" is decided, as arithmetic rather than as a
-- handler body.
--
-- Two reasons this is a file of its own rather than a few lines inside the
-- `media.move` command:
--
--   * the axes are the thing that goes silently wrong. A "left" that moves a set
--     to the right is not a crash and not a log line -- it is a prop that slides
--     the wrong way while the operator holds the button, and the only instrument
--     that can see it is a person. So it is pure, and it is tested
--     (`tests/placement_test.lua`) against the engine's own heading convention
--     rather than against this file's opinion of it.
--   * a nudge is *relative to the set*, not to the world. A television turned to
--     face a room has its own forward, and "forward" on the button has to mean
--     the way the cabinet is facing or the control is useless on any set that is
--     not axis-aligned.
--
-- -----------------------------------------------------------------------------
-- THE HEADING CONVENTION, WHICH IS THE ENGINE'S AND NOT A PREFERENCE
-- -----------------------------------------------------------------------------
-- yaw 0 faces +Y and increases counter-clockwise (docs/research/
-- movement-and-heading.md, and the same convention `Api::Movement`,
-- `facingPlacement` in server/main.lua and every other heading in this project
-- use). So in world components:
--
--     forward = (-sin(yaw), +cos(yaw), 0)
--     right   = (+cos(yaw), +sin(yaw), 0)     -- forward turned a quarter turn
--                                               clockwise, which is what "right"
--                                               means standing behind the set
--     up      = (0, 0, +1)                    -- z is up in this engine's world
--
-- `facingPlacement` in server/main.lua places a set at
-- `x - sin(heading) * distance, y + cos(heading) * distance`, which is this
-- forward vector, and the two are cross-checked in the placement suite: if that
-- helper and this module ever disagree about which way yaw 0 points, one of them
-- is wrong and the suite says so.
-- =============================================================================

Open77MediaPlacement = {}

---How far one nudge moves a set, in metres.
---
---A quarter of a metre: small enough to line a screen up against a wall or a
---counter by eye, large enough that crossing a room is a dozen presses rather
---than a hundred. The command and the menu both take a metres argument, so this
---is the default rather than the only option.
Open77MediaPlacement.Step = 0.25

---How far one rotate turns a set, in degrees. Fifteen degrees is the same
---compromise: a quarter turn takes six presses, and a set that is five degrees
---off the wall can be brought flush.
Open77MediaPlacement.RotationStep = 15.0

---The nudge directions, in the order a UI should offer them. Kept here rather
---than in the menu so the command, the menu and the tests cannot disagree about
---what may be asked for.
Open77MediaPlacement.Directions = { "left", "right", "forward", "back", "up", "down" }

---The ways a set can be turned.
Open77MediaPlacement.Turns = { "left", "right" }

---The largest nudge one request may apply, in metres, and the largest turn, in
---degrees. A bound rather than a validation nicety: the menu is one message
---away from a client, and "move it a kilometre" is a screen nobody can ever
---reach again. A hundred metres still crosses the biggest room in Night City,
---and a full turn is available one press at a time.
Open77MediaPlacement.MaximumStep = 100.0
Open77MediaPlacement.MaximumTurn = 180.0

---How high and low a set may be nudged, in world z. Wide on purpose -- a
---rooftop billboard is a legitimate set -- but finite, so a stuck key cannot
---push a television out of the world's own bounds.
Open77MediaPlacement.MinimumHeight = -500.0
Open77MediaPlacement.MaximumHeight = 1000.0

---A copy of a position, with the bucket carried through.
---
---The bucket is part of where a prop is (it selects the streaming bucket, not a
---coordinate), so a nudge that dropped it would move the set into another part
---of the world as far as the streaming system is concerned while leaving it on
---the same screen as far as the player is concerned.
---@param position table { x, y, z, bucket }
---@return table
local function copyPosition(position)
    return {
        x = tonumber(position.x) or 0.0,
        y = tonumber(position.y) or 0.0,
        z = tonumber(position.z) or 0.0,
        bucket = position.bucket,
    }
end

---True for a direction `Nudge` accepts.
---@param direction any
---@return boolean
function Open77MediaPlacement.IsDirection(direction)
    if type(direction) ~= "string" then return false end
    for _, known in ipairs(Open77MediaPlacement.Directions) do
        if known == direction then return true end
    end
    return false
end

---True for a turn `Turn` accepts.
---@param direction any
---@return boolean
function Open77MediaPlacement.IsTurn(direction)
    if type(direction) ~= "string" then return false end
    for _, known in ipairs(Open77MediaPlacement.Turns) do
        if known == direction then return true end
    end
    return false
end

---A heading brought back into 0..360.
---
---Turned rather than refused: a set rotated past north has simply been rotated,
---and refusing the eleventh press of a quarter-turn button because it left the
---range would be a control that stops working for a reason nobody can see.
---@param degrees number
---@return number
function Open77MediaPlacement.Wrap(degrees)
    local value = tonumber(degrees) or 0.0
    value = value % 360.0
    if value < 0.0 then value = value + 360.0 end
    -- `-0.0` and `360.0` both print as things a person would rather not read in
    -- `media.list`, and `%` can produce either.
    if value == 360.0 then value = 0.0 end
    return value
end

---Where a set ends up after being nudged along its own axes.
---
---`position` and the returned table are plain `{ x, y, z, bucket }`; the caller's
---table is never modified, so a refused `setTransform` leaves the entry the
---server reports pointing at where the set still is.
---@param position table { x, y, z, bucket }
---@param yaw number the set's own heading, degrees, 0 = +Y, counter-clockwise
---@param direction string one of `Directions`
---@param metres number|nil defaults to `Step`; clamped to `MaximumStep`
---@return table|nil moved position, or nil
---@return string|nil the reason when nil
function Open77MediaPlacement.Nudge(position, yaw, direction, metres)
    if type(position) ~= "table" then return nil, "no_position" end
    if not Open77MediaPlacement.IsDirection(direction) then return nil, "unknown_direction" end

    local distance = tonumber(metres) or Open77MediaPlacement.Step
    if distance <= 0.0 then return nil, "distance_must_be_positive" end
    if distance > Open77MediaPlacement.MaximumStep then
        distance = Open77MediaPlacement.MaximumStep
    end

    local out = copyPosition(position)
    if direction == "up" then
        out.z = out.z + distance
    elseif direction == "down" then
        out.z = out.z - distance
    else
        local radians = math.rad(tonumber(yaw) or 0.0)
        -- The set's own forward and right, as the engine defines heading. See
        -- the header: yaw 0 is +Y and yaw grows counter-clockwise.
        local forwardX, forwardY = -math.sin(radians), math.cos(radians)
        local rightX, rightY = math.cos(radians), math.sin(radians)
        if direction == "forward" then
            out.x = out.x + forwardX * distance
            out.y = out.y + forwardY * distance
        elseif direction == "back" then
            out.x = out.x - forwardX * distance
            out.y = out.y - forwardY * distance
        elseif direction == "right" then
            out.x = out.x + rightX * distance
            out.y = out.y + rightY * distance
        else -- left
            out.x = out.x - rightX * distance
            out.y = out.y - rightY * distance
        end
    end

    if out.z < Open77MediaPlacement.MinimumHeight or out.z > Open77MediaPlacement.MaximumHeight then
        return nil, "height_out_of_range"
    end
    return out, nil
end

---The heading a set has after being turned on the spot.
---
---"left" turns the set's own left, which is a positive rotation in a
---counter-clockwise yaw convention: a set turned to its left ends up facing
---further counter-clockwise than it was.
---@param yaw number
---@param direction string one of `Turns`
---@param degrees number|nil defaults to `RotationStep`; clamped to `MaximumTurn`
---@return number|nil heading, or nil
---@return string|nil the reason when nil
function Open77MediaPlacement.Turn(yaw, direction, degrees)
    if not Open77MediaPlacement.IsTurn(direction) then return nil, "unknown_turn" end

    local amount = tonumber(degrees) or Open77MediaPlacement.RotationStep
    if amount <= 0.0 then return nil, "degrees_must_be_positive" end
    if amount > Open77MediaPlacement.MaximumTurn then amount = Open77MediaPlacement.MaximumTurn end

    local value = tonumber(yaw) or 0.0
    if direction == "left" then
        value = value + amount
    else
        value = value - amount
    end
    return Open77MediaPlacement.Wrap(value), nil
end
