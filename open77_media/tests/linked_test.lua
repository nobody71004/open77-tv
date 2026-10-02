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
-- (preloads shared/records.lua, shared/placement.lua, shared/clock.lua,
-- server/config.lua and server/adblock.lua -- everything server/main.lua reads as
-- it loads -- and names the root in OPEN77_REPO_ROOT)
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

-- `exports.<resource>:link()`, as the host's synchronous export proxy answers it.
local providers = {}
_G.exports = setmetatable({}, {
    __index = function(_, resource)
        local provider = providers[resource]
        if provider == nil then error("export_target_missing: " .. tostring(resource)) end
        return { link = function(_) return provider() end }
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

-- 1. The browser cinema takes opx_tvbrowser's link, whatever the panel's field held.
providers.opx_tvbrowser = function() return LINK end
local result, snapshot = spawnAs(7, { record = "cinema.150ft.browser", url = "https://typed.example/", yaw = 90 })
check(result ~= nil and result[1] == true, "the browser cinema is put up: " .. tostring(result and result[2]))
local set = findRecord(snapshot, "cinema.150ft.browser")
check(set ~= nil and set.url == LINK, "it shows the shared browser's link, not the typed one: " .. tostring(set and set.url))
check(set ~= nil and set.label == "Browser cinema, 150 ft", "it carries its own label")
check(#created == 1 and created[1].scale == 39.4137931 and created[1].collision == true,
    "on the same 150 ft panel as the cinema screen")

-- 1b. The 100 ft browser cinema the same way, on its own panel.
created = {}
result, snapshot = spawnAs(7, { record = "cinema.100ft.browser", yaw = 90 })
check(result ~= nil and result[1] == true, "the 100 ft browser cinema is put up: " .. tostring(result and result[2]))
set = findRecord(snapshot, "cinema.100ft.browser")
check(set ~= nil and set.url == LINK and set.label == "Browser cinema, 100 ft", "showing the shared browser's link, with its own label")
check(#created == 1 and created[1].scale == 26.2758621 and created[1].collision == true, "on the 100 ft panel")

-- 2. Without the resource, it says so and puts up nothing.
providers.opx_tvbrowser = nil
local createdBefore = #created
result = spawnAs(7, { record = "cinema.150ft.browser" })
check(result ~= nil and result[1] == false and result[2] == "linked_resource_unavailable:opx_tvbrowser",
    "no opx_tvbrowser: refused by name (" .. tostring(result and result[2]) .. ")")
check(#created == createdBefore, "and no prop is created for it")

-- 2b. `/browser cinema` brings the link itself (from the server that holds it):
-- when the export cannot be asked, a shared-browser link in the request is used.
result, snapshot = spawnAs(7, { record = "cinema.150ft.browser", url = LINK })
check(result ~= nil and result[1] == true, "no export, a shared-browser link in the request: put up (" ..
    tostring(result and result[2]) .. ")")
set = nil
for _, spec in ipairs(snapshot or {}) do
    if spec.record == "cinema.150ft.browser" and spec.url == LINK then set = spec end
end
check(set ~= nil, "showing the link the request brought")

-- 2c. Any other link in the request is not the browser: still refused.
createdBefore = #created
result = spawnAs(7, { record = "cinema.150ft.browser", url = "https://typed.example/" })
check(result ~= nil and result[1] == false and result[2] == "linked_resource_unavailable:opx_tvbrowser",
    "no export, an ordinary link in the request: refused (" .. tostring(result and result[2]) .. ")")
check(#created == createdBefore, "and nothing is put up for it")
result = spawnAs(7, { record = "cinema.150ft.browser", url = "#open77-shared-browser" })
check(result ~= nil and result[1] == false, "the bare mark is not a link")

-- 3. A resource with no link configured is the same refusal.
providers.opx_tvbrowser = function() return nil end
result = spawnAs(7, { record = "cinema.150ft.browser" })
check(result ~= nil and result[1] == false, "a provider with no link is refused too")

-- 4. A record without linkFrom keeps the link it was given.
providers.opx_tvbrowser = function() return LINK end
result, snapshot = spawnAs(7, { record = "cinema.150ft", url = "https://www.youtube.com/watch?v=abc" })
check(result ~= nil and result[1] == true, "the plain cinema is put up")
local plain = nil
for _, spec in ipairs(snapshot or {}) do
    if spec.record == "cinema.150ft" then plain = spec end
end
check(plain ~= nil and plain.url == "https://www.youtube.com/watch?v=abc", "the plain cinema keeps the link it was given")

for _, name in ipairs(STUBBED) do rawset(_G, name, saved[name]) end
_G.print = realPrint

_G.TestResult = { passed = passed, failed = #failures, failures = failures }
