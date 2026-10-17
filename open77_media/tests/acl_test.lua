-- =============================================================================
-- open77_media -- tests/acl_test.lua
-- =============================================================================
-- Pins the question the panel asks about a player: "may this id do it?"
--
-- Why this is a suite. The bug it exists for was invisible from inside the
-- resource: the ACE was granted, the ACL entry was right, the label was right,
-- and the gate asked a function this runtime does not have -- so every player,
-- the owner included, was told the television was admin-only. Nothing raised, no
-- log line said anything, and the only instrument that could see it was the
-- admin standing in front of a panel that refused him. What is pinned here is
-- therefore the ORDER (the host's ACL first, FiveM's name second), the REFUSAL
-- (a host with neither answers no rather than raising) and the console rule.
--
-- Standalone:  python tools/run-suite.py    (from the repository root)

local passed = 0
local failures = {}
local function check(condition, message)
    if condition then
        passed = passed + 1
    else
        failures[#failures + 1] = message or "assertion failed"
    end
end

-- =============================================================================
-- The module under test
-- =============================================================================
-- Loaded the way the resource loads it: as a shared script with no arguments.
-- The runner composes the same file the manifest lists.

local acl = assert(Open77MediaAcl, "shared/acl.lua did not publish Open77MediaAcl")

local ACE = "command.media.lock"

-- The two host shapes are installed and removed around each case, so no case can
-- pass on a stub another one left behind.
local function withHost(host, fn)
    local savedOpen77, savedFivem = rawget(_G, "Open77"), rawget(_G, "IsPlayerAceAllowed")
    rawset(_G, "Open77", host.Open77)
    rawset(_G, "IsPlayerAceAllowed", host.fivem)
    local ok, err = pcall(fn)
    rawset(_G, "Open77", savedOpen77)
    rawset(_G, "IsPlayerAceAllowed", savedFivem)
    if not ok then failures[#failures + 1] = "the case raised: " .. tostring(err) end
end

-- =============================================================================
-- The console
-- =============================================================================

do
    withHost({}, function()
        check(acl.Allowed(0, ACE) == true, "the console is an admin")
        check(acl.Allowed(-1, ACE) == true, "and so is anything below zero")
        check(acl.Allowed(nil, ACE) == true, "and no source at all, which is the console too")
    end)
end

-- =============================================================================
-- Open77's own ACL -- the gate this server actually has
-- =============================================================================

do
    local asked = {}
    withHost({ Open77 = { acl = { isAllowed = function(source, ace)
        asked[#asked + 1] = { source = source, ace = ace }
        return source == 7 and ace == ACE
    end } } }, function()
        check(acl.Allowed(7, ACE) == true, "the holder of the ACE may drive the panel")
        check(acl.Allowed(8, ACE) == false, "a player without it may not")
        check(#asked == 2, "and every question went to the host's ACL, once each")
        check(asked[1] ~= nil and asked[1].source == 7 and asked[1].ace == ACE,
            "with the player and the ACE the resource asks about, unchanged")
    end)
end

-- A host that ANSWERS instead of returning, e.g. nil. Anything but true is a no:
-- a permission system with three answers must not be read as two.
do
    withHost({ Open77 = { acl = { isAllowed = function() return nil end } } }, function()
        check(acl.Allowed(7, ACE) == false, "an answer that is not true is a refusal")
    end)
end

-- =============================================================================
-- The fallback: FiveM's name, on a FiveM-shaped host
-- =============================================================================

do
    local asked = {}
    withHost({ fivem = function(source, ace)
        asked[#asked + 1] = { source = source, ace = ace }
        return true
    end }, function()
        check(acl.Allowed(11, ACE) == true, "a FiveM host still answers through IsPlayerAceAllowed")
        check(#asked == 1 and asked[1].source == 11, "asked about the same player")
    end)
end

do
    withHost({ fivem = function() return false end }, function()
        check(acl.Allowed(11, ACE) == false, "and its refusal is a refusal")
    end)
end

do
    withHost({ fivem = function() error("boom") end }, function()
        check(acl.Allowed(11, ACE) == false, "a fallback that raises is a refusal, not a crash")
    end)
end

-- =============================================================================
-- A host with neither -- and one that raises
-- =============================================================================

do
    withHost({}, function()
        check(acl.Allowed(11, ACE) == false,
            "a host with neither API refuses: a panel nobody may use beats a dead resource")
    end)
end

-- `Open77.acl.isAllowed` RUNNING is a host error, not a refusal (a malformed id, a
-- backend that went away). It takes the fallback, and with no fallback it refuses;
-- what it must never do is raise out of the gate and kill the control handler.
do
    withHost({ Open77 = { acl = { isAllowed = function() error("no such player") end } } }, function()
        check(acl.Allowed(11, ACE) == false, "an ACL that raises refuses instead of raising")
    end)
    withHost({ Open77 = { acl = { isAllowed = function() error("no such player") end } },
        fivem = function() return true end }, function()
        check(acl.Allowed(11, ACE) == true, "and the other host's answer is still asked for")
    end)
end

-- A host with an `acl` table and no `isAllowed` -- half an API -- is the case the
-- pcall is INSIDE the call for: `pcall(Open77.acl.isAllowed, ...)` would index nil
-- before pcall ever ran.
do
    withHost({ Open77 = { acl = {} } }, function()
        check(acl.Allowed(11, ACE) == false, "half an ACL table refuses without raising")
    end)
end

-- =============================================================================
-- Nonsense arguments
-- =============================================================================

do
    withHost({ Open77 = { acl = { isAllowed = function() return true end } } }, function()
        check(acl.Allowed(11, nil) == false, "no ACE name is nobody's permission")
        check(acl.Allowed(11, "") == false, "and neither is an empty one")
        check(acl.Allowed(11, 42) == false, "nor a number where a name belongs")
        check(acl.Allowed(5, "command.media.lock") == true, "the ordinary case still answers")
    end)
end

-- =============================================================================
-- Published the way the other suites publish: the runner clears `TestResult`
-- before loading a suite and reads it afterwards.

TestResult = {
    passed = passed,
    failed = #failures,
    failures = failures,
}
