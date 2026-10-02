-- =============================================================================
-- open77_media -- shared/clock.lua
-- =============================================================================
-- Where in the programme a set is.
--
-- A television has two halves that must agree about the picture, and only one of
-- them can own the answer. The server already owns `url`, `paused`, `volume` and
-- the curtain -- `payload` in server/main.lua is the single wire record every
-- client draws from -- so it owns this too. Each set plays its link in its own
-- browser, and a player starts at zero the moment it is built, so two clients
-- watching one link start at their own build times and a client that joins an
-- hour into a film starts at the beginning. That is the whole of "the screen
-- beside it is behind by quite a bit": nothing in the wire record carried a time,
-- so there was nothing for a late player to catch up to.
--
-- The arithmetic is a pure function of an injected `nowMs` rather than a counter
-- this module advances. A counter would need a timer to advance it, and a timer
-- that stops -- a paused resource, a stalled world, a client that was not in the
-- room -- is a clock that silently stops with it, which is a worse failure than
-- the one this fixes. `GetGameTimer` on the server and `Open77.time.monotonic`
-- on the client are the same monotonic millisecond clock, so the position is
-- derived on demand, from the time the programme started, and never drifts.
--
-- Two fields on an entry, and every transition writes both through these
-- functions so no other file has to know what they mean:
--
--   * `startedAt` -- the monotonic instant the current programme began playing
--     FROM ZERO. Set at spawn, and again whenever the URL changes, because a new
--     link is a new programme and the position in the old one means nothing.
--   * `heldAt`    -- the position frozen by a pause. Resuming rewinds `startedAt`
--     by this much so playback continues from where it stopped rather than
--     restarting, and so a client that joins while paused still knows where the
--     picture is.
--
-- Two ways a programme begins, and the difference is who else is watching it:
-- `Start` for the set that puts a link on the wall, `Join` for a set that is
-- handed a link another live set is already playing. Both write the same two
-- fields, so nothing downstream has to know which one happened.
--
-- Standalone:  lua tools/lua-test/run.lua <repo-root>   (tests/clock_test.lua)
-- =============================================================================

Open77MediaClock = {}

---Seconds into the current programme.
---
---Paused sets answer from `heldAt` and ignore the clock entirely: a pause is the
---absence of progress, so a client sampling a paused set must not see the number
---creep while the picture is frozen.
---
---@param entry table the media entry
---@param nowMs number monotonic milliseconds
---@return number seconds, never negative
function Open77MediaClock.Position(entry, nowMs)
    if entry == nil then return 0.0 end

    local held = tonumber(entry.heldAt) or 0.0
    if held < 0.0 then held = 0.0 end

    if entry.paused or entry.startedAt == nil then return held end

    local startedAt = tonumber(entry.startedAt)
    local now = tonumber(nowMs)
    if startedAt == nil or now == nil then return held end

    -- A clock that went backwards is a world that reloaded under a live entry.
    -- Clamping to zero is the honest answer: the film is at its start, not at a
    -- negative offset that a page would hand straight to `seekTo`.
    local elapsed = (now - startedAt) / 1000.0
    if elapsed < 0.0 then return 0.0 end
    return elapsed
end

---A new programme, starting now from zero.
---
---Called at spawn and on every URL change -- including the change to an empty
---URL, which is a set showing its idle screen and is just as much a programme
---that began at a known instant.
---
---@param entry table the media entry
---@param nowMs number monotonic milliseconds
function Open77MediaClock.Start(entry, nowMs)
    if entry == nil then return end
    entry.startedAt = tonumber(nowMs)
    entry.heldAt = 0.0
end

---Joins a programme that is already under way, at the position given.
---
---This is the second half of "two screens showing one link stay together", and the
---half the first version of this module was missing. `Start` gives every set its
---own programme, which is right for a set that is the only one showing a link and
---wrong for the pair a watch party actually makes: link X goes on the screen in
---front of the player at 20:00, link X goes on the screen beside it at 20:05, and
---each `Start` is a film opening from zero five minutes apart. Measured in this
---world: two cinema screens on one link, one plainly behind the other, with both
---clocks working exactly as designed.
---
---So a set that is handed a link another live set is already playing does not
---start a programme -- it joins the one that exists. The caller passes the
---position it read from that set; this function only places the origin, and it
---stays arithmetic so the suite can pin it without a world.
---
---@param entry table the media entry
---@param positionSeconds number seconds into the programme the joiner wants
---@param nowMs number monotonic milliseconds
function Open77MediaClock.Join(entry, positionSeconds, nowMs)
    if entry == nil then return end

    local position = tonumber(positionSeconds) or 0.0
    if position < 0.0 then position = 0.0 end

    local now = tonumber(nowMs)
    if now == nil then
        entry.startedAt = nil
        entry.heldAt = position
        return
    end

    -- The same trick `Resume` uses: a programme that is `position` seconds in is
    -- a programme that began `position` seconds ago. Tracking the position
    -- instead of the origin is what actually matters, so both are written -- a
    -- paused joiner answers from `heldAt` and a playing one from the origin.
    entry.startedAt = now - position * 1000.0
    entry.heldAt = position
end

---Stops the clock where it stands.
---
---@param entry table the media entry
---@param nowMs number monotonic milliseconds
function Open77MediaClock.Pause(entry, nowMs)
    if entry == nil then return end
    -- Read the position BEFORE the flag flips: `Position` answers from `heldAt`
    -- the moment `paused` is true, so pausing first would freeze a zero.
    entry.heldAt = Open77MediaClock.Position(entry, nowMs)
    entry.paused = true
end

---Starts the clock again from where it stopped.
---
---@param entry table the media entry
---@param nowMs number monotonic milliseconds
function Open77MediaClock.Resume(entry, nowMs)
    if entry == nil then return end
    entry.paused = false
    local now = tonumber(nowMs)
    local held = tonumber(entry.heldAt) or 0.0
    if now == nil then
        entry.startedAt = nil
        return
    end
    -- The whole point of the rewind: the programme did not restart, it continued.
    entry.startedAt = now - held * 1000.0
end

---Applies a boolean pause state, whichever way it was asked for.
---
---The panel, the console and a reissued record all reach the same two fields, and
---sending a pause that is already in effect must not rewind or restart the film.
---
---@param entry table the media entry
---@param paused boolean
---@param nowMs number monotonic milliseconds
function Open77MediaClock.SetPaused(entry, paused, nowMs)
    if entry == nil then return end
    if paused == true then
        if entry.paused ~= true then Open77MediaClock.Pause(entry, nowMs) end
        return
    end
    if entry.paused == true then
        Open77MediaClock.Resume(entry, nowMs)
        return
    end
    -- Not paused, and was not paused: an entry that never held a position still
    -- needs one, or a client reading `elapsed` gets nil for a set that is plainly
    -- playing.
    if entry.startedAt == nil then Open77MediaClock.Start(entry, nowMs) end
end

return Open77MediaClock
