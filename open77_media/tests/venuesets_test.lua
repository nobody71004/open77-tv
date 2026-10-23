-- =============================================================================
-- open77_media -- tests/venuesets_test.lua
-- =============================================================================
-- The venue-placement store, and the panel action that writes it.
--
-- A venue screen (`MediaVenueSets` in server/config.lua) is part of the world:
-- it is spawned at start, at the coordinates in that file. Those coordinates are
-- a survey, and the world is the only honest judge of them -- the drive-in
-- screen shipped a few metres off. Two panel actions are the answer:
--   * SAVE THIS SPOT (`pinHere`) writes the spot the screen is standing on now,
--     after the nudge pad has lined it up, so the work outlives the restart;
--   * PUT IT IN FRONT OF ME (`pinFront`) takes the coarse step: the server reads
--     where the operator stands and faces, derives the hang point the way a panel
--     spawn does, moves the set there and saves that.
-- Either way the spot is `data/venue-sets.json`, which the next start uses
-- instead of the survey.
--
-- Two halves are tested here:
--   * the pure store (`server/venuesets.lua`): what it accepts, what it skips and
--     says so about, and the file shape it writes;
--   * the wiring (`server/main.lua`), loaded whole against recorded stubs the way
--     linked_test.lua does, with the venue-spawn thread run at load: a press of
--     pinHere moves the set, writes the store, and the NEXT load comes up at the
--     saved spot rather than the surveyed one.
--
-- Standalone:  lua tools/lua-test/run.lua <repo-root>
-- (preloads shared/records.lua, shared/placement.lua, shared/keys.lua,
-- shared/clock.lua, server/config.lua, server/adblock.lua and
-- server/venuesets.lua -- everything server/main.lua reads as it loads -- and
-- names the root in OPEN77_REPO_ROOT)
-- =============================================================================

local passed = 0
local failures = {}
local function check(condition, message)
    if condition then passed = passed + 1 else failures[#failures + 1] = message or "assertion failed" end
end

local Store = assert(MediaVenueSetsStore, "server/venuesets.lua was not preloaded")

-- ── the pure store ──────────────────────────────────────────────────────────
local VENUE = "cinema.150ft"

local cleaned, why = Store.Placement({ x = 1.5, y = -2.25, z = 10, yaw = 400 })
check(cleaned ~= nil and cleaned.x == 1.5 and cleaned.z == 10 and cleaned.bucket == 0,
    "a placement keeps its coordinates and defaults its bucket")
check(cleaned ~= nil and math.abs(cleaned.yaw - 40.0) < 1.0e-9,
    "a yaw outside the circle is wrapped, because a heading is an angle")

check(Store.Placement({ x = 0 / 0, y = 0, z = 0, yaw = 0 }) == nil,
    "a NaN coordinate is refused")
check(select(2, Store.Placement({ x = 0, y = 0, z = 0 })) == "placement_needs_a_finite_yaw",
    "a placement with no facing is refused, and named")
check(select(2, Store.Placement("here")) == "placement_must_be_a_table",
    "a placement that is not a table is refused")

local loaded, problems = Store.Load({
    version = 1,
    placements = {
        [VENUE] = { x = 1, y = 2, z = 3, yaw = 90 },
        ["not.a.record"] = { x = 0, y = 0, z = 0, yaw = 0 },
        ["cinema.100ft"] = { x = 0, y = 0 },
    },
}, function(record) return record == VENUE or record == "cinema.100ft" end)
check(loaded[VENUE] ~= nil and loaded[VENUE].yaw == 90, "a good row is read")
check(loaded["not.a.record"] == nil and loaded["cinema.100ft"] == nil,
    "a row for a record the catalogue does not carry, and one with no z, are both skipped")
local named = table.concat(problems, " | ")
check(named:find("not.a.record", 1, true) ~= nil,
    "and the record that is not in the catalogue is named")
check(named:find("placement_needs_finite_x_y_z", 1, true) ~= nil,
    "so is the row that would not parse")

local broken, brokenProblems = Store.Load("nonsense")
check(next(broken) == nil and brokenProblems[1] == "store_is_not_a_table",
    "a store that is not a table falls back to nothing, and says so")
check(#Store.Load(nil) == 0, "an absent store is not a problem")

local snapshot = Store.Snapshot({ [VENUE] = loaded[VENUE], ["a.record"] = { x = 0, y = 0, z = 0, yaw = 5 } })
check(snapshot.version == Store.VERSION and snapshot.placements[VENUE] ~= nil,
    "the written shape carries its version and every placement")

local fromConfig, origin = Store.For({}, VENUE, { x = 5, y = 6, z = 7, yaw = 8 })
check(origin == "config" and fromConfig.x == 5 and fromConfig.yaw == 8,
    "with nothing saved, a venue row starts at its surveyed coordinates")
local fromStore, savedOrigin = Store.For(loaded, VENUE, { x = 5, y = 6, z = 7, yaw = 8 })
check(savedOrigin == "saved" and fromStore.x == 1 and fromStore.yaw == 90,
    "with a saved spot, the saved one wins over the survey")

-- ── the wiring: server/main.lua, loaded whole ───────────────────────────────
local STUBBED = { "RegisterCommand", "RegisterNetEvent", "AddEventHandler", "TriggerClientEvent",
    "GetCurrentResourceName", "GetGameTimer", "GetPlayers", "CreateThread", "Wait",
    "Open77", "exports", "source" }
local saved = {}
for _, name in ipairs(STUBBED) do saved[name] = rawget(_G, name) end

local handlers = {}
local clientEvents = {}
local props = {}
local nextProp = 900
local ioStore = {}
local writes = {}
local playerPosition = { x = 100.0, y = 200.0, z = 30.0, bucket = 0 }
local playerHeading = 90.0

_G.GetGameTimer = function() return 0 end
_G.GetPlayers = function() return {} end
_G.RegisterCommand = function() end
_G.RegisterNetEvent = function(name, fn) handlers[name] = fn end
_G.AddEventHandler = function() end
_G.TriggerClientEvent = function(name, target, ...)
    clientEvents[#clientEvents + 1] = { name = name, target = target, args = table.pack(...) }
end
_G.GetCurrentResourceName = function() return "open77_media" end
_G.CreateThread = function(fn) fn() end
_G.Wait = function() end
_G.Open77 = {
    players = {
        position = function() return playerPosition end,
        get = function()
            if playerHeading == nil then return { bucket = 0, fresh = true, ready = true } end
            return { bucket = 0, fresh = true, ready = true, heading = playerHeading, yaw = playerHeading }
        end,
        all = function() return { 7 } end,
    },
    acl = {
        -- The panel switch is admin-only by default and this suite is signed in
        -- as the operator: every control is gated on `command.media.lock`.
        isAllowed = function() return true end,
    },
    props = {
        create = function(spec)
            nextProp = nextProp + 1
            local row = { id = tostring(nextProp), spec = spec }
            props[#props + 1] = row
            return nextProp
        end,
        all = function()
            local out = {}
            for _, row in ipairs(props) do out[#out + 1] = { id = row.id } end
            return out
        end,
        setTransform = function(id, transform)
            for _, row in ipairs(props) do
                if tostring(row.id) == tostring(id) then row.transform = transform end
            end
            return true
        end,
        remove = function() return true end,
    },
    time = { monotonic = function() return 0 end },
    io = {
        exists = function(name) return ioStore[name] ~= nil, ioStore[name] ~= nil and nil or "not_found" end,
        readJson = function(name) return ioStore[name], ioStore[name] == nil and "not_found" or nil end,
        writeJson = function(name, value)
            ioStore[name] = value
            writes[#writes + 1] = { name = name, value = value }
            return true
        end,
    },
}
_G.exports = setmetatable({}, { __index = function() return { link = function() return nil end } end })

local repoRoot = type(OPEN77_REPO_ROOT) == "string" and OPEN77_REPO_ROOT or "."
local serverPath = repoRoot .. "/resources/system/open77_media/server/main.lua"

local realPrint = print
local function loadServer()
    handlers, clientEvents, props, writes = {}, {}, {}, {}
    nextProp = 900
    local chunk = assert(loadfile(serverPath), serverPath .. " not found")
    _G.print = function() end
    local ok, err = pcall(chunk)
    _G.print = realPrint
    return ok, err
end

local function lastResult()
    for index = #clientEvents, 1, -1 do
        if clientEvents[index].name == "open77:media:result" then return clientEvents[index].args end
    end
end

local function snapshot()
    for index = #clientEvents, 1, -1 do
        if clientEvents[index].name == "open77:media:snapshot" then return clientEvents[index].args[1] end
    end
end

local function venueEntry()
    local list = snapshot() or {}
    for _, spec in ipairs(list) do
        if spec.venue ~= nil then return spec end
    end
end

local function control(player, action, payload)
    local handler = handlers["open77:media:control"]
    if type(handler) ~= "function" then return end
    clientEvents = {}
    _G.source = player
    _G.print = function() end
    handler(action, payload)
    _G.print = realPrint
    _G.source = nil
end

local ok, loadError = loadServer()
check(ok, "the server half loads with the venue store: " .. tostring(loadError))

local entry = venueEntry()
check(entry ~= nil, "the venue screen is spawned at start")
check(entry ~= nil and entry.venue == VENUE,
    "and the panel is told it is a venue screen, under its record key")
check(entry ~= nil and math.abs(entry.position.x - (-74.41)) < 0.01
    and math.abs(entry.position.y - 1984.64) < 0.01,
    "at the surveyed coordinates while nothing has been saved")
check(#writes == 0, "and nothing was written just by starting")

-- SAVE THIS SPOT, with the set where the survey put it: the numbers written
-- are the entry's OWN, and nothing about the screen moves.
control(7, "pinHere", { id = entry and entry.id or 0 })
local result = lastResult()
check(result ~= nil and result[1] == true, "SAVE THIS SPOT is accepted: " .. tostring(result and result[2]))
check(result ~= nil and string.find(tostring(result[2]), "comes back here after a restart", 1, true) ~= nil,
    "and says the spot outlives the restart")
check(entry ~= nil and math.abs(entry.position.x - (-74.41)) < 0.01,
    "and saving does not move the screen")

local written = ioStore["venue-sets.json"]
check(written ~= nil and written.version == Store.VERSION,
    "the placement is written to data/venue-sets.json")
local placed = written ~= nil and written.placements[VENUE] or nil
check(placed ~= nil, "under the venue's record key")
check(placed ~= nil and math.abs(placed.x - (-74.41)) < 0.01
    and math.abs(placed.y - 1984.64) < 0.01 and math.abs(placed.yaw - 321.3) < 0.01,
    "with the spot the screen is standing on -- the surveyed one, unchanged")

-- LINE IT UP, THEN SAVE IT: a nudge from the panel, then the pin. The saved
-- numbers follow the set, which is what makes the pad worth using.
control(7, "move", { id = entry and entry.id or 0, direction = "forward", metres = 2.0 })
control(7, "pinHere", { id = entry and entry.id or 0 })
check(lastResult() ~= nil and lastResult()[1] == true, "a nudged set can be saved")
local nudged = ioStore["venue-sets.json"] and ioStore["venue-sets.json"].placements[VENUE] or nil
check(nudged ~= nil and math.abs(nudged.x - (-74.41)) > 0.5,
    "and the saved spot is the nudged one, not the survey")

-- PUT IT IN FRONT OF ME: a player standing elsewhere, facing a way. The set is
-- moved the record's own hanging distance ahead, facing back, and saved.
playerPosition = { x = 12.0, y = -4.0, z = 8.0, bucket = 0 }
playerHeading = 90.0
control(7, "pinFront", { id = entry and entry.id or 0 })
local placedResult = lastResult()
check(placedResult ~= nil and placedResult[1] == true,
    "PUT IT IN FRONT OF ME is accepted: " .. tostring(placedResult and placedResult[2]))
local inFront = ioStore["venue-sets.json"] and ioStore["venue-sets.json"].placements[VENUE] or nil
check(inFront ~= nil, "and writes the spot too")
if inFront ~= nil then
    local ahead = math.sqrt((inFront.x - playerPosition.x) ^ 2 + (inFront.y - playerPosition.y) ^ 2)
    check(math.abs(ahead - 30.5612) < 0.01,
        string.format("at the record's own hang distance in front of the player (%.3f m)", ahead))
    check(math.abs(inFront.z - playerPosition.z) < 0.01, "on the ground the player stands on")
    local turned = math.abs((inFront.yaw - 270.0 + 180.0) % 360.0 - 180.0) < 0.01
    check(turned, string.format("turned so its picture looks back at the player (yaw %.1f)", inFront.yaw))
end

-- A player the host cannot answer a facing for is refused rather than placed at
-- a zero that reads as north.
playerHeading = nil
local kept = ioStore["venue-sets.json"] and ioStore["venue-sets.json"].placements[VENUE] or nil
control(7, "pinFront", { id = entry and entry.id or 0 })
check(tostring(lastResult() and lastResult()[2]) == "no_heading_snapshot",
    "a host that cannot say which way the player faces refuses the move")
local unchanged = ioStore["venue-sets.json"] and ioStore["venue-sets.json"].placements[VENUE] or nil
check(kept ~= nil and unchanged ~= nil and unchanged.x == kept.x and unchanged.yaw == kept.yaw,
    "and leaves the saved spot alone")

-- A build whose server cannot read a player's facing: the client's own offer is
-- taken instead -- the rule `media.spawn` already follows -- and wrapped, so 370
-- is the same wall as 10.
playerHeading = nil
control(7, "pinFront", { id = entry and entry.id or 0, yaw = 370.0 })
local offered = lastResult()
check(offered ~= nil and offered[1] == true,
    "a facing the client offers is used when the server cannot read one: " .. tostring(offered and offered[2]))
local offeredSpot = ioStore["venue-sets.json"] and ioStore["venue-sets.json"].placements[VENUE] or nil
check(offeredSpot ~= nil and math.abs(offeredSpot.yaw - 190.0) < 0.01,
    string.format("and the saved screen faces back along it (yaw %.1f)", offeredSpot and offeredSpot.yaw or -1))

-- A session set has no next start, so it cannot be pinned.
clientEvents = {}
_G.source = 7
_G.print = function() end
local spawnHandler = handlers["open77:media:spawn"]
if type(spawnHandler) == "function" then
    spawnHandler({ record = "tv.16x9" })
end
_G.print = realPrint
_G.source = nil
local session = nil
for _, spec in ipairs(snapshot() or {}) do
    if spec.venue == nil and spec.record == "tv.16x9" then session = spec end
end
check(session ~= nil, "a set spawned from the panel is a session set")
playerHeading = 0.0
control(7, "pinHere", { id = session and session.id or 0 })
check(tostring(lastResult() and lastResult()[2]) == "not_a_venue_screen",
    "and SAVE THIS SPOT is refused for it")
control(7, "pinFront", { id = session and session.id or 0 })
check(tostring(lastResult() and lastResult()[2]) == "not_a_venue_screen",
    "so is PUT IT IN FRONT OF ME")

-- ── the next start reads the saved spot ─────────────────────────────────────
local before = ioStore["venue-sets.json"]
local savedSpot = before and before.placements[VENUE] or nil
local reloaded, reloadError = loadServer()
check(reloaded, "the server half loads again on top of the store: " .. tostring(reloadError))
local after = venueEntry()
check(after ~= nil, "the venue screen comes up on the next start")
check(after ~= nil and savedSpot ~= nil
    and math.abs(after.position.x - savedSpot.x) < 0.001
    and math.abs(after.position.y - savedSpot.y) < 0.001
    and math.abs(after.position.z - savedSpot.z) < 0.001,
    "at the SAVED spot, not the surveyed one")
check(after ~= nil and savedSpot ~= nil and math.abs(after.yaw - savedSpot.yaw) < 0.001,
    "and facing the way it was saved")

-- A store that cannot be read is the survey again, not a venue with no screen.
ioStore["venue-sets.json"] = "not a table"
local fallback, fallbackError = loadServer()
check(fallback, "a corrupt store still boots: " .. tostring(fallbackError))
local surveyed = venueEntry()
check(surveyed ~= nil and math.abs(surveyed.position.x - (-74.41)) < 0.01,
    "and the venue screen falls back to its surveyed spot")

for _, name in ipairs(STUBBED) do rawset(_G, name, saved[name]) end

-- The runner reads this and prints the line; the suite does not exit, so a
-- failure here is reported beside every other suite's rather than ending the run.
TestResult = { passed = passed, failed = #failures, failures = failures }
