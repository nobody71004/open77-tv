-- =============================================================================
-- open77_media -- tests/clock_test.lua
-- =============================================================================
-- Pins the arithmetic behind multi-screen playback: where a set is in its
-- programme, and what each transition does to that.
--
-- Why this is a suite and not four lines of confidence: every failure mode here
-- is invisible in a log and obvious only to a person looking at two screens. A
-- resume that restarts the film reads as "the big screen jumped back to the
-- start"; a pause that freezes zero reads as "I paused it and it went back";
-- a URL change that keeps the old position reads as "the new film opens in the
-- middle". None of them raises anything. The one instrument that can see them is
-- somebody standing in front of a set, and by then the defect is in the world.
--
-- The clock is injected, not read: every case below states the time it wants.
-- That is the whole reason `shared/clock.lua` takes `nowMs` instead of owning a
-- counter -- the arithmetic is the part worth pinning, and a module that read a
-- real clock could only be tested by sleeping.
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

local function near(left, right, tolerance)
    return math.abs((tonumber(left) or 0) - (tonumber(right) or 0)) <= (tolerance or 1.0e-6)
end

-- =============================================================================
-- The module under test
-- =============================================================================
-- Loaded the way the resource loads it: as a shared script with no arguments.
-- `tools/lua-test/run.lua` composes the same files the manifest lists, so this
-- suite runs against the shipped bytes rather than against a second copy.

local clock = assert(Open77MediaClock, "shared/clock.lua did not publish Open77MediaClock")

local T0 = 1000000 -- an arbitrary monotonic origin; nothing here reads a real clock
local function at(seconds) return T0 + math.floor(seconds * 1000) end

-- =============================================================================
-- A programme that has just started
-- =============================================================================

do
    local entry = {}
    clock.Start(entry, at(0))
    check(near(clock.Position(entry, at(0)), 0.0), "a fresh programme starts at zero")
    check(near(clock.Position(entry, at(30)), 30.0), "and is 30s in after 30s")
    check(near(clock.Position(entry, at(3600)), 3600.0),
        "an hour in, it is an hour in -- this is the joiner a set used to greet at 0")
end

-- =============================================================================
-- Pause freezes, resume continues
-- =============================================================================

do
    local entry = {}
    clock.Start(entry, at(0))

    clock.Pause(entry, at(40))
    check(near(entry.heldAt, 40.0), "pausing holds the position it stopped at")
    check(near(clock.Position(entry, at(40)), 40.0), "a paused set reports where it stopped")
    check(near(clock.Position(entry, at(400)), 40.0),
        "and still reports it ten minutes later -- a pause is the absence of progress")

    -- The one that matters most: a resume must continue, not restart.
    clock.Resume(entry, at(1000))
    check(near(clock.Position(entry, at(1000)), 40.0), "resuming continues from the held position")
    check(near(clock.Position(entry, at(1010)), 50.0),
        "and advances from there, so the 60s the set sat paused are not skipped")
end

-- =============================================================================
-- A new link is a new programme
-- =============================================================================

do
    local entry = {}
    clock.Start(entry, at(0))
    check(near(clock.Position(entry, at(500)), 500.0), "the first programme is 500s in")

    clock.Start(entry, at(500))
    check(near(clock.Position(entry, at(500)), 0.0),
        "a new link starts at zero -- the position in the old film means nothing")
    check(near(clock.Position(entry, at(505)), 5.0), "and then advances normally")

    -- A link set while paused starts playing, which is the server's rule and the
    -- page's: the entry must not still be holding the old position.
    local paused = {}
    clock.Start(paused, at(0))
    clock.Pause(paused, at(20))
    check(paused.paused == true, "the set is paused")
    clock.Start(paused, at(30))
    check(paused.paused == true,
        "starting a programme does not itself unpause: the caller clears the flag, " ..
        "and `SetPaused(entry, false, now)` is what continues it")
end

-- =============================================================================
-- A second set handed a link somebody is already playing
-- =============================================================================
-- The pair a watch party makes: the film goes on one screen, and the screen
-- beside it is given the same link minutes later. `Start` would open a second
-- copy at zero -- two screens five minutes apart with both clocks correct -- so
-- a set in that position joins the programme instead.

do
    local first = {}
    clock.Start(first, at(0))
    check(near(clock.Position(first, at(300)), 300.0), "the first set is 300s in")

    -- The joining set is created and handed the link five minutes late.
    local second = {}
    clock.Start(second, at(300))
    check(near(clock.Position(second, at(300)), 0.0),
        "starting its own programme would open the film from the beginning")

    clock.Join(second, clock.Position(first, at(300)), at(300))
    check(near(clock.Position(second, at(300)), 300.0),
        "joining puts the second set where the first one is")
    check(near(clock.Position(second, at(310)), 310.0),
        "and it advances from there, at the same rate")
    check(near(clock.Position(second, at(900)), clock.Position(first, at(900))),
        "ten minutes later the two still agree -- this is the whole point")

    -- A paused set has no advancing position, so the position handed in is the
    -- held one. A joiner must land there, not at the pause's original origin.
    local paused = {}
    clock.Start(paused, at(0))
    clock.Pause(paused, at(50))
    local joiner = {}
    clock.Join(joiner, clock.Position(paused, at(400)), at(400))
    check(near(clock.Position(joiner, at(400)), 50.0),
        "a joiner lands on the held position of a paused set")

    -- Defensive: the caller's number comes from a live entry, and a clock that
    -- went backwards must not put a joiner before the start of the film.
    local behind = {}
    clock.Join(behind, -12.0, at(0))
    check(near(clock.Position(behind, at(0)), 0.0), "a negative join position clamps to zero")

    local timeless = {}
    clock.Join(timeless, 40.0, nil)
    check(near(clock.Position(timeless, at(0)), 40.0),
        "a join with no clock still reports the position it was given")
end

-- =============================================================================
-- SetPaused is the door every caller goes through
-- =============================================================================

do
    -- The panel sends `paused: false` on every state it re-states. An unpause
    -- that was not a pause must not move the playhead, or each of those would
    -- restart the film.
    local entry = {}
    clock.Start(entry, at(0))
    clock.SetPaused(entry, false, at(120))
    check(near(clock.Position(entry, at(120)), 120.0),
        "clearing a pause that was never set leaves the position alone")

    clock.SetPaused(entry, true, at(120))
    check(near(clock.Position(entry, at(200)), 120.0), "setting it freezes where it was")

    -- A repeat of the same pause is not a second pause.
    clock.SetPaused(entry, true, at(300))
    check(near(clock.Position(entry, at(400)), 120.0),
        "pausing an already-paused set does not move the held position")

    clock.SetPaused(entry, false, at(400))
    check(near(clock.Position(entry, at(410)), 130.0), "and it continues from the hold")
end

-- =============================================================================
-- An entry that never had a programme
-- =============================================================================
-- A set created before the first state arrives, or a record built by an older
-- server that has no `startedAt` at all. The answer has to be a number, because
-- the page hands it straight to `seekTo`.

do
    local bare = {}
    check(near(clock.Position(bare, at(10)), 0.0),
        "an entry with no programme reports zero rather than nil")
    check(near(clock.Position(nil, at(10)), 0.0), "and so does no entry at all")

    clock.SetPaused(bare, false, at(10))
    check(near(clock.Position(bare, at(20)), 10.0),
        "clearing a pause on an entry that never started starts its clock")
end

-- =============================================================================
-- A clock that went backwards
-- =============================================================================
-- A world reload resets the monotonic clock under a live entry. A negative
-- position is not a state any player can be in, and a page seeking to it would
-- ask its player for a time before the start of the film.

do
    local entry = {}
    clock.Start(entry, at(600))
    check(near(clock.Position(entry, at(500)), 0.0),
        "a now that precedes startedAt clamps to zero, never negative")
    check(clock.Position(entry, at(500)) >= 0.0, "and the answer is never negative")
end

-- =============================================================================
-- Published the way every other suite in this resource publishes: the runner
-- clears `TestResult` before loading a suite and reads it afterwards, so the
-- global IS the result. Returning a table instead is a suite that runs every
-- assertion and then reports nothing -- which is what this did until the second
-- suite in the set caught it as "published no TestResult".

TestResult = {
    passed = passed,
    failed = #failures,
    failures = failures,
}
