-- opx_watchparty -- client/main.lua
--
-- The player's side: the panel (/watch), which television a party goes on, the
-- browser link, and the screen's page. Every party state comes from the server
-- (`opx:watch:state`); this file only draws it and asks for changes.
--
-- WHICH TELEVISION. open77_media owns the sets. Its server pushes the whole set
-- list to every client (`open77:media:snapshot`, the host-wide bus delivers it to
-- every resource that registers the name), with each set's position and reach.
-- A party started here goes on the nearest set the player is within reach of,
-- and the screen is pointed at the party's page through open77_media's own
-- control message -- the same request its remote sends when a player pastes a
-- link into it.

local VERSION = "1.0.1"
local DEFAULT_REACH = 3.0

local parties = {}         -- code -> state (+ _receivedAt, GetGameTimer ms)
local base = nil           -- where browsers reach the server
local mine = { code = nil, key = nil }
local screens = {}         -- open77_media's sets: id -> spec
local panel, panelOpen = nil, false
local lastResult = nil

local function now() return GetGameTimer() end

---This player's session id (the server's numbering), or nil.
local function myId()
    if type(Open77) == "table" and type(Open77.session) == "table" and type(Open77.session.playerId) == "function" then
        local ok, id = pcall(Open77.session.playerId)
        if ok then return tonumber(id) end
    end
    return nil
end

local function myPosition()
    if type(Open77) ~= "table" or type(Open77.character) ~= "table" then return nil end
    local ok, x, y, z = pcall(Open77.character.position)
    if not ok or type(x) ~= "number" then return nil end
    return x, y, z
end

local function distanceTo(position)
    if type(position) ~= "table" then return nil end
    local x, y, z = myPosition()
    if x == nil then return nil end
    local dx = (tonumber(position.x) or 0) - x
    local dy = (tonumber(position.y) or 0) - y
    local dz = (tonumber(position.z) or 0) - z
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

---The nearest set, its distance, and whether the player is within its reach.
local function nearestScreen()
    local best, bestDistance
    for _, spec in pairs(screens) do
        local d = distanceTo(spec.position)
        if d ~= nil and (bestDistance == nil or d < bestDistance) then best, bestDistance = spec, d end
    end
    if best == nil then return nil end
    local reach = tonumber(best.reach) or DEFAULT_REACH
    return best, bestDistance, bestDistance <= reach + 0.5
end

local function partyAtScreen(id)
    if id == nil then return nil end
    for _, party in pairs(parties) do
        if tonumber(party.tvId) == tonumber(id) then return party end
    end
    return nil
end

local function myParty()
    return mine.code and parties[mine.code] or nil
end

---Where a party's film is now, from its last state and the time since it came.
local function livePosition(party)
    local ms = party.positionMs or 0
    if party.playing then ms = ms + math.max(0, now() - (party._receivedAt or now())) end
    if party.durationMs and ms > party.durationMs then ms = party.durationMs end
    return math.floor(ms)
end

local function browserLink(party)
    return WatchNetflix.watchUrl(party.netflixId, livePosition(party), party.code, mine.key)
end

local function view(party)
    if party == nil then return nil end
    return {
        code = party.code, title = party.title, netflixId = party.netflixId,
        playing = party.playing, positionMs = livePosition(party), durationMs = party.durationMs,
        hostName = party.hostName, lastBy = party.lastBy, members = party.members,
        browsers = party.browsers, inSync = party.inSync, tvId = party.tvId,
    }
end

local function pushPanel()
    if panel == nil then return end
    local screen, distance, inReach = nearestScreen()
    local here = screen and partyAtScreen(screen.id) or nil
    local current = myParty()
    panel:send("watch:state", {
        open = panelOpen,
        version = VERSION,
        party = view(current),
        isHost = current ~= nil and current.host ~= nil and tonumber(current.host) == myId(),
        hasKey = mine.key ~= nil,
        nearby = (here ~= nil and (current == nil or here.code ~= current.code)) and view(here) or nil,
        screen = screen and {
            id = screen.id, label = screen.label or screen.record or ("set " .. tostring(screen.id)),
            distance = distance and math.floor(distance * 10 + 0.5) / 10 or nil,
            inReach = inReach == true,
        } or nil,
        result = lastResult,
    })
end

local function result(ok, text)
    lastResult = { ok = ok == true, text = tostring(text or ""), at = now() }
    print("[opx_watchparty] " .. (ok and "" or "refused: ") .. lastResult.text)
    pushPanel()
end

-- ---------------------------------------------------------------------------
-- What the server says
-- ---------------------------------------------------------------------------

RegisterNetEvent("open77:media:snapshot", function(snapshot)
    local incoming = {}
    if type(snapshot) == "table" then
        for _, spec in ipairs(snapshot) do
            local id = tonumber(spec.id)
            if id ~= nil then incoming[id] = spec end
        end
    end
    screens = incoming
end)

RegisterNetEvent("opx:watch:state", function(payload)
    if type(payload) ~= "table" then return end
    local received = now()
    local incoming = {}
    for _, party in ipairs(payload.parties or {}) do
        if type(party) == "table" and type(party.code) == "string" then
            party._receivedAt = received
            incoming[party.code] = party
        end
    end
    parties = incoming
    base = payload.base or base
    pushPanel()
end)

RegisterNetEvent("opx:watch:you", function(payload)
    if type(payload) ~= "table" then return end
    if payload.code == false or payload.code == nil then
        mine = { code = nil, key = nil }
    else
        mine = { code = tostring(payload.code), key = type(payload.key) == "string" and payload.key or nil }
    end
    pushPanel()
end)

RegisterNetEvent("opx:watch:result", function(payload)
    if type(payload) ~= "table" then return end
    result(payload.ok, payload.text)
end)

---Starts (or switches) a party at the nearest set in reach.
local function startHere(netflixId, title)
    local screen, _, inReach = nearestScreen()
    local tvId = (screen ~= nil and inReach) and screen.id or nil
    TriggerServerEvent("opx:watch:start", { netflixId = netflixId, title = title, tvId = tvId })
    if tvId == nil then
        result(true, "no television in reach: the party runs without a screen (stand at a TV and start it again to put it on one)")
    end
end

RegisterNetEvent("opx:watch:pick", function(payload)
    if type(payload) ~= "table" then return end
    startHere(payload.netflixId, payload.title)
end)

-- The server asks this client to point a set at the party's page: the request
-- open77_media's own remote makes when a player pastes a link into it.
RegisterNetEvent("opx:watch:bindtv", function(payload)
    if type(payload) ~= "table" or tonumber(payload.tvId) == nil or type(payload.url) ~= "string" then return end
    local spec = screens[tonumber(payload.tvId)]
    if spec ~= nil and spec.url == payload.url then return end
    TriggerServerEvent("open77:media:control", "url", { id = tonumber(payload.tvId), url = payload.url })
end)

-- A party that ended takes its page off the set -- only if the set still shows it.
RegisterNetEvent("opx:watch:releasetv", function(payload)
    if type(payload) ~= "table" or tonumber(payload.tvId) == nil then return end
    local spec = screens[tonumber(payload.tvId)]
    if spec == nil or spec.url ~= payload.url then return end
    TriggerServerEvent("open77:media:control", "url", { id = tonumber(payload.tvId), url = "" })
end)

local function copyLink()
    local party = myParty()
    if party == nil or mine.key == nil then
        result(false, "join a watch party first (/watch join <code>, or Start at a TV)")
        return
    end
    local link = browserLink(party)
    local copied, reason = false, "no_clipboard"
    if type(Open77.clipboard) == "table" and type(Open77.clipboard.setText) == "function" then
        local ok, value, why = pcall(Open77.clipboard.setText, link)
        copied, reason = ok and value == true, ok and why or value
    end
    if copied then
        result(true, "browser link copied: paste it into Edge or Chrome (with the Open77 Watch Party extension). " ..
            "It carries your own key -- do not share it.")
    else
        result(false, "could not copy the link (" .. tostring(reason) .. "): it is " .. link)
    end
end

RegisterNetEvent("opx:watch:copylink", function() copyLink() end)

local function joinNear()
    local screen = nearestScreen()
    local here = screen and partyAtScreen(screen.id) or nil
    if here == nil then
        result(false, "no watch party on the TV in front of you: /watch join <code>")
        return
    end
    TriggerServerEvent("opx:watch:join", { code = here.code })
end

RegisterNetEvent("opx:watch:joinnear", function() joinNear() end)

-- ---------------------------------------------------------------------------
-- The panel
-- ---------------------------------------------------------------------------

local function setPanel(open)
    if panel == nil then
        print("[opx_watchparty] no panel on this client; the /watch commands still work")
        return
    end
    panelOpen = open == true
    if panelOpen then
        local focused, reason = panel:setFocus(true, true)
        if focused ~= true then print("[opx_watchparty] panel focus refused: " .. tostring(reason)) end
    else
        panel:setFocus(false, false)
    end
    pushPanel()
end

RegisterNetEvent("opx:watch:panel", function(open)
    if open == true and panelOpen then setPanel(false) else setPanel(open ~= false) end
end)

AddEventHandler("open77:pauseKey", function()
    if panelOpen then setPanel(false) end
end)

local function onAction(payload)
    if type(payload) ~= "table" or type(payload.action) ~= "string" then return end
    local action = payload.action
    if action == "close" then return setPanel(false) end
    if action == "copylink" then return copyLink() end
    if action == "joinnear" then return joinNear() end
    if action == "join" then
        local code = tostring(payload.code or ""):upper():gsub("[^A-Z2-9]", "")
        if #code ~= 6 then return result(false, "a party code is six letters and digits") end
        return TriggerServerEvent("opx:watch:join", { code = code })
    end
    if action == "leave" then return TriggerServerEvent("opx:watch:leave") end
    if action == "stop" then return TriggerServerEvent("opx:watch:stop") end
    if action == "start" then
        local id, reason = WatchNetflix.parse(tostring(payload.link or ""))
        if id == nil then return result(false, "that is not a Netflix title link (" .. tostring(reason) .. ")") end
        local title = type(payload.title) == "string" and payload.title:sub(1, 200) or nil
        return startHere(id, title)
    end
    if action == "play" or action == "pause" or action == "toggle" then
        return TriggerServerEvent("opx:watch:control", { action = action })
    end
    if action == "nudge" and tonumber(payload.deltaMs) then
        return TriggerServerEvent("opx:watch:control", { action = "nudge", deltaMs = tonumber(payload.deltaMs) })
    end
    if action == "seek" and tonumber(payload.positionMs) then
        return TriggerServerEvent("opx:watch:control", { action = "seek", positionMs = tonumber(payload.positionMs) })
    end
end

local function createPanel()
    if type(WebUI) ~= "table" or type(WebUI.create) ~= "function" then
        print("[opx_watchparty] this client has no WebUI: the /watch commands still work")
        return
    end
    local page, reason = WebUI.create({
        entry = "web/panel.html",
        layer = "menu",
        width = 1920,
        height = 1080,
        fps = 30,
        zIndex = 690,
        transparent = true,
        -- Never false: a surface created hidden never uploads a frame on this
        -- build (open77_media measured it); the page hides itself instead.
        visible = true,
    })
    if page == nil then
        print("[opx_watchparty] panel refused: " .. tostring(reason) .. " (the /watch commands still work)")
        return
    end
    panel = page
    page:on("watch:ready", function() pushPanel() end)
    page:on("watch:action", onAction)
end

AddEventHandler("onClientResourceStart", function(name)
    if name ~= GetCurrentResourceName() then return end
    createPanel()
    TriggerServerEvent("opx:watch:hello")
    -- open77_media pushes its sets on changes only; ask once for the current
    -- list (its own join request: the answer is the same whole-set snapshot).
    TriggerServerEvent("open77:media:ready")
    print("[opx_watchparty] client " .. VERSION .. " ready (/watch)")
end)

AddEventHandler("onClientResourceStop", function(name)
    if name ~= GetCurrentResourceName() then return end
    if panel ~= nil then
        pcall(function() panel:setFocus(false, false) end)
        pcall(function() panel:destroy() end)
        panel = nil
    end
end)

CreateThread(function()
    while true do
        Wait(1000)
        if panelOpen then pushPanel() end
    end
end)
