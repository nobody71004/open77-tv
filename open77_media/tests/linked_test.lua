-- =============================================================================
-- open77_media -- tests/linked_test.lua
-- =============================================================================
-- The browser cinema: a record whose picture is another resource's
-- (`linkFrom = "opx_tvbrowser"`) is put up showing that resource's link, asked of
-- its `link` export at spawn -- whatever the panel's link field held -- and a
-- server without that resource answers so instead of putting up a blank screen.
-- A record without `linkFrom` keeps the link it was given.
--
-- The server half is loaded against recorded stubs, as keys_test.lua does, and
-- the spawn request is sent the way the panel sends it.
--
-- Standalone:  lua tools/lua-test/run.lua <repo-root>
-- (preloads shared/records.lua, shared/placement.lua, shared/keys.lua,
-- shared/clock.lua, server/config.lua and server/adblock.lua -- everything
-- server/main.lua reads as it loads -- and names the root in OPEN77_REPO_ROOT)
-- =============================================================================

local passed = 0
local failures = {}
local function check(condition, message)
    if condition then passed = passed + 1 else failures[#failures + 1] = message or "assertion failed" end
end

local LINK = "https://browser.example/tv/?usr=open77&pwd=x&embed=1#open77-shared-browser"

-- Every global this suite stubs is put back at the end, so a suite that runs
-- after it in the same state (tools/run-suite.py shares one) sees what it would
-- have seen without this one.
local STUBBED = { "RegisterCommand", "RegisterNetEvent", "AddEventHandler", "TriggerClientEvent",
    "GetCurrentResourceName", "GetGameTimer", "Open77", "exports", "source" }
local saved = {}
for _, name in ipairs(STUBBED) do saved[name] = rawget(_G, name) end

local handlers, clientEvents, created, live = {}, {}, {}, {}
local nextProp = 4000
_G.GetGameTimer = function() return 0 end
_G.RegisterCommand = function() end
_G.RegisterNetEvent = function(name, fn) handlers[name] = fn end
_G.AddEventHandler = function() end
_G.TriggerClientEvent = function(name, target, ...)
    clientEvents[#clientEvents + 1] = { name = name, target = target, args = table.pack(...) }
end
_G.GetCurrentResourceName = function() return "open77_media" end
_G.Open77 = {
    players = {
        position = function() return { x = 10.0, y = 20.0, z = 30.0, bucket = 0 } end,
        all = function() return { 7 } end,
    },
    props = {
        create = function(spec)
            nextProp = nextProp + 1
            created[#created + 1] = spec
            live[#live + 1] = { id = nextProp }
            return nextProp
        end,
        all = function() return live end,
        setTransform = function() return true end,
        remove = function() return true end,
    },
    time = { monotonic = function() return 0 end },
}

-- `exports.<resource>:link()`, as the host's synchronous export proxy answers it --
-- with `claim` and `linkWhy` beside it, nil when the provider has neither (older
-- providers only have `link`).
local providers = {}
local claimers, whys = {}, {}
_G.exports = setmetatable({}, {
    __index = function(_, resource)
        local provider = providers[resource]
        if provider == nil then error("export_target_missing: " .. tostring(resource)) end
        local claimer, why = claimers[resource], whys[resource]
        return {
            link = function(_) return provider() end,
            claim = function(_, url) return claimer ~= nil and claimer(url) or nil end,
            linkWhy = function(_) return why ~= nil and why() or nil end,
        }
    end,
})

local repoRoot = type(OPEN77_REPO_ROOT) == "string" and OPEN77_REPO_ROOT or "."
local serverPath = repoRoot .. "/resources/system/open77_media/server/main.lua"
local server = assert(loadfile(serverPath), serverPath .. " not found")
local realPrint = print
_G.print = function() end
local loaded, loadError = pcall(server)
_G.print = realPrint
check(loaded, "the server half loads: " .. tostring(loadError))
local spawnHandler = handlers["open77:media:spawn"]
check(type(spawnHandler) == "function", "the spawn request has a handler")

local function spawnAs(player, payload)
    -- A server half that did not load has no handler: every check below then
    -- fails by name instead of the suite stopping at the first call.
    if type(spawnHandler) ~= "function" then return nil, nil end
    clientEvents = {}
    _G.source = player
    _G.print = function() end
    spawnHandler(payload)
    _G.print = realPrint
    _G.source = nil
    local result, snapshot
    for _, event in ipairs(clientEvents) do
        if event.name == "open77:media:result" then result = event.args end
        if event.name == "open77:media:snapshot" then snapshot = event.args[1] end
    end
    return result, snapshot
end

local function findRecord(snapshot, record)
    for _, spec in ipairs(snapshot or {}) do
        if spec.record == record then return spec end
    end
end

-- 1. The room's 150 ft panel takes opx_tvbrowser's link, whatever the panel's
-- field held: the room's home screen is what the record is for.
providers.opx_tvbrowser = function() return LINK end
local result, snapshot = spawnAs(7, { record = "cinema.150ft", url = "https://typed.example/", yaw = 90 })
check(result ~= nil and result[1] == true, "the room's 150 ft panel is put up: " .. tostring(result and result[2]))
local set = findRecord(snapshot, "cinema.150ft")
check(set ~= nil and set.url == LINK, "it shows the room's link, not the typed one: " .. tostring(set and set.url))
check(set ~= nil and set.label == "open77-TV-ROOM (150 ft)", "it carries its own label")
check(#created == 1 and created[1].scale == 39.4137931 and created[1].collision == true,
    "on the 150 ft panel, the same one the cinema screen was")

-- 1b. The 100 ft panel the same way.
created = {}
result, snapshot = spawnAs(7, { record = "cinema.100ft", yaw = 90 })
check(result ~= nil and result[1] == true, "the 100 ft panel is put up: " .. tostring(result and result[2]))
set = findRecord(snapshot, "cinema.100ft")
check(set ~= nil and set.url == LINK and set.label == "open77-TV-ROOM (100 ft)",
    "showing the room's link, with its own label")
check(#created == 1 and created[1].scale == 26.2758621 and created[1].collision == true, "on the 100 ft panel")

-- 2. Without the resource, it says so and puts up nothing.
providers.opx_tvbrowser = nil
local createdBefore = #created
result = spawnAs(7, { record = "cinema.150ft" })
check(result ~= nil and result[1] == false and result[2] == "linked_resource_unavailable:opx_tvbrowser",
    "no opx_tvbrowser: refused by name (" .. tostring(result and result[2]) .. ")")
check(#created == createdBefore, "and no prop is created for it")

-- 2b. `/browser cinema` brings the link itself (from the server that holds it):
-- when the export cannot be asked, a shared-browser link in the request is used.
result, snapshot = spawnAs(7, { record = "cinema.150ft", url = LINK })
check(result ~= nil and result[1] == true, "no export, a shared-browser link in the request: put up (" ..
    tostring(result and result[2]) .. ")")
set = nil
for _, spec in ipairs(snapshot or {}) do
    if spec.record == "cinema.150ft" and spec.url == LINK then set = spec end
end
check(set ~= nil, "showing the link the request brought")

-- 2c. Any other link in the request is not the browser: still refused.
createdBefore = #created
result = spawnAs(7, { record = "cinema.150ft", url = "https://typed.example/" })
check(result ~= nil and result[1] == false and result[2] == "linked_resource_unavailable:opx_tvbrowser",
    "no export, an ordinary link in the request: refused (" .. tostring(result and result[2]) .. ")")
check(#created == createdBefore, "and nothing is put up for it")
result = spawnAs(7, { record = "cinema.150ft", url = "#open77-shared-browser" })
check(result ~= nil and result[1] == false, "the bare mark is not a link")

-- 3. A resource with no link configured is the same refusal.
providers.opx_tvbrowser = function() return nil end
result = spawnAs(7, { record = "cinema.150ft" })
check(result ~= nil and result[1] == false, "a provider with no link is refused too")

-- 5. A provider vouches for a link the spawner brought: /watch cinema puts the
-- player's own party page on the same room panel, over the room's own link.
-- `linkClaimFrom` on the record is what makes opx_watchparty's `claim` asked.
local WPARTY = "https://xbuniverse.duckdns.org/opx-watch-xb-staging/v1/tv/ABC234"
providers.opx_tvbrowser = function() return LINK end
providers.opx_watchparty = function() return nil end
claimers.opx_watchparty = function(url) if url == WPARTY then return url end return nil end
whys.opx_watchparty = function() return "too_many_watch_parties" end
-- `live` is reset with `created`: the snapshot lists every set in the world, and
-- this test's panel has the same record id as the ones put up above.
created = {}
live = {}
result, snapshot = spawnAs(7, { record = "cinema.150ft", url = WPARTY, yaw = 90 })
check(result ~= nil and result[1] == true, "a vouched party page is put up: " .. tostring(result and result[2]))
set = findRecord(snapshot, "cinema.150ft")
check(set ~= nil and set.url == WPARTY, "showing the vouched page, not the room's link")
check(#created == 1 and created[1].scale == 39.4137931, "on the 150 ft panel")

-- 5b. A page nobody vouches for does not displace the room: the spawn lands on
-- the room's own link, which is what the record is for.
created = {}
live = {}
result, snapshot = spawnAs(7, { record = "cinema.150ft", url = "https://evil.test/" })
check(result ~= nil and result[1] == true, "an unvouched page: still put up (" .. tostring(result and result[2]) .. ")")
set = findRecord(snapshot, "cinema.150ft")
check(set ~= nil and set.url == LINK, "showing the room's link, never the unvouched one")

-- 5c. With the room's link gone too, the unvouched page is refused rather than
-- shown: a record's picture is still its provider's to choose.
providers.opx_tvbrowser = function() return nil end
createdBefore = #created
result = spawnAs(7, { record = "cinema.150ft", url = "https://evil.test/" })
check(result ~= nil and result[1] == false, "no room link, an unvouched page: refused")
check(#created == createdBefore, "and nothing is put up for it")

-- 4. A record without linkFrom keeps the link it was given.
result, snapshot = spawnAs(7, { record = "tv.16x9", url = "https://www.youtube.com/watch?v=abc" })
check(result ~= nil and result[1] == true, "a plain television is put up")
local plain = nil
for _, spec in ipairs(snapshot or {}) do
    if spec.record == "tv.16x9" then plain = spec end
end
check(plain ~= nil and plain.url == "https://www.youtube.com/watch?v=abc", "the plain television keeps the link it was given")

for _, name in ipairs(STUBBED) do rawset(_G, name, saved[name]) end
_G.print = realPrint

_G.TestResult = { passed = passed, failed = #failures, failures = failures }
