-- opx_watchparty -- server/main.lua
--
-- Wiring: the in-game side (chat commands, the panel's requests, the state every
-- client draws), the browser side (Open77.http.listen) and the clock that keeps
-- parties tidy. The rules are in party.lua and http.lua.

local VERSION = "1.0.1"

local function convar(name, fallback)
    if type(GetConvar) == "function" then
        local ok, value = pcall(GetConvar, name, fallback)
        if ok and type(value) == "string" and value ~= "" then return value end
    end
    return fallback
end

-- Where browsers reach this resource: the web server in front of the server's
-- loopback HTTP listener publishes `/<this path>/` as `/opx_watchparty/`.
local PUBLIC_BASE = convar("opx_watchparty_public_base", "https://xbuniverse.duckdns.org/opx-watch-xb-staging")
    :gsub("/+$", "")

local function nowMs()
    if type(Open77) == "table" and type(Open77.time) == "table" and type(Open77.time.unix) == "function" then
        local ok, t = pcall(Open77.time.unix)
        if ok and type(t) == "number" then return math.floor(t * 1000) end
    end
    return GetGameTimer()
end

local registry = WatchParty.new({ clock = nowMs, random = function(n) return math.random(n) end })
local httpRoute, httpReason = nil, nil

local function nameOf(player)
    if type(GetPlayerName) == "function" then
        local ok, name = pcall(GetPlayerName, player)
        if ok and type(name) == "string" and name ~= "" then return name end
    end
    return "player " .. tostring(player)
end

local function tvUrl(code)
    return PUBLIC_BASE .. "/v1/tv/" .. code
end

local function connectedPlayers()
    if type(Open77.players) == "table" and type(Open77.players.all) == "function" then
        local ok, ids = pcall(Open77.players.all)
        if ok and type(ids) == "table" then return ids end
    end
    if type(GetPlayers) == "function" then
        local ok, ids = pcall(GetPlayers)
        if ok and type(ids) == "table" then return ids end
    end
    return {}
end

---Every client gets every party: the list is small, and the panel and the
---television's neighbours both need it.
local function broadcast(target)
    local list = registry:list()
    local payload = { parties = list, base = PUBLIC_BASE, version = VERSION }
    if target ~= nil then
        TriggerClientEvent("opx:watch:state", target, payload)
        return
    end
    for _, player in ipairs(connectedPlayers()) do
        TriggerClientEvent("opx:watch:state", tonumber(player) or player, payload)
    end
end

local function sendYou(player)
    local party = registry:partyOf(player)
    if party == nil then
        TriggerClientEvent("opx:watch:you", player, { code = false })
        return
    end
    TriggerClientEvent("opx:watch:you", player, { code = party.code, key = registry:keyOf(party, player) })
end

---Answers the player who typed a command (both channels open77_media uses: the
---host's own result line, and the gamemode's answer line on this server).
local function tell(source, raw, success, text)
    print("[opx_watchparty] " .. tostring(source) .. ": " .. text)
    if source == nil or source <= 0 then return end
    TriggerClientEvent("open77:command:result", source, raw or "", success == true, text)
    if success == true then
        TriggerClientEvent("opx:net:runtime:commandAnswer", source, raw or "", "success", text, false)
    end
end

local REASONS = {
    netflix_link_expected = "give a Netflix link or title number: /watch netflix <link> [title]",
    not_a_netflix_link = "that is not a netflix.com link",
    no_title_in_link = "that Netflix link has no title in it (open the film, then copy the address)",
    netflix_link_too_long = "that link is too long",
    netflix_title_expected = "no Netflix title given",
    not_in_a_party = "you are not in a watch party: /watch netflix <link> at a TV, or /watch join <code>",
    no_such_party = "no watch party has that code",
    party_full = "that watch party is full",
    too_many_parties = "too many watch parties on this server right now",
    only_the_host = "only the party's host can end it for everyone (/watch leave to leave it)",
    position_out_of_range = "that time is outside the film",
    delta_out_of_range = "that jump is too far",
    time_unreadable = "write times like 1:02:03, 62:03, 95, +30 or -10",
    time_expected = "give a time, like 1:02:03 or +30",
    time_too_large = "that time is past 12 hours",
}
local function why(reason)
    return REASONS[reason] or tostring(reason)
end

local function describe(state)
    local where = WatchPlayhead.format(state.positionMs)
    if state.durationMs then where = where .. " / " .. WatchPlayhead.format(state.durationMs) end
    return string.format("%s -- %s, %s (party %s, %d in it, %d of %d browsers in step)",
        state.title ~= "" and state.title or ("Netflix title " .. state.netflixId),
        state.playing and "playing" or "paused", where, state.code, state.members, state.inSync, state.browsers)
end

-- ---------------------------------------------------------------------------
-- The in-game side
-- ---------------------------------------------------------------------------

local function startFor(source, payload)
    if type(payload) ~= "table" then return nil, "netflix_link_expected" end
    local id, reason = WatchNetflix.parse(tostring(payload.netflixId or payload.link or ""))
    if id == nil then return nil, reason end
    local tvId = tonumber(payload.tvId)
    if tvId ~= nil and (tvId ~= tvId or tvId < 1 or tvId > 1000000 or math.floor(tvId) ~= tvId) then tvId = nil end
    local title = payload.title
    if type(title) ~= "string" then title = nil elseif #title > 200 then title = title:sub(1, 200) end
    local party, key, how = registry:start(source, { netflixId = id, title = title, tvId = tvId, name = nameOf(source) })
    if party == nil then return nil, key end
    sendYou(source)
    if party.tvId ~= nil then
        TriggerClientEvent("opx:watch:bindtv", source, { tvId = party.tvId, url = tvUrl(party.code) })
    end
    broadcast(nil)
    return party, how
end

local function controlFor(source, payload)
    local party = registry:partyOf(source)
    if party == nil then return nil, "not_in_a_party" end
    if type(payload) ~= "table" then return nil, "unknown_action" end
    local action = payload.action
    local args = { positionMs = tonumber(payload.positionMs), deltaMs = tonumber(payload.deltaMs) }
    if type(payload.time) == "string" then
        local current = WatchPlayhead.position(party.playhead, registry:now())
        local target, reason = WatchPlayhead.parseTime(payload.time, current)
        if target == nil then return nil, reason end
        action = "seek"
        args.positionMs = target
    end
    local ok, reason = registry:control(party, action, args)
    if not ok then return nil, reason end
    party.lastBy = nameOf(source)
    broadcast(nil)
    return party
end

local function stopFor(source)
    local party = registry:partyOf(source)
    if party == nil then return nil, "not_in_a_party" end
    if party.host ~= source then return nil, "only_the_host" end
    local members = {}
    for player in pairs(party.members) do members[#members + 1] = player end
    local tvId = party.tvId
    registry:stop(party)
    for _, player in ipairs(members) do sendYou(player) end
    if tvId ~= nil then
        TriggerClientEvent("opx:watch:releasetv", source, { tvId = tvId, url = tvUrl(party.code) })
    end
    broadcast(nil)
    return party
end

RegisterNetEvent("opx:watch:hello", function()
    local source = source
    if source == nil then return end
    sendYou(source)
    broadcast(source)
end)

RegisterNetEvent("opx:watch:start", function(payload)
    local source = source
    if source == nil then return end
    local party, how = startFor(source, payload)
    if party == nil then
        tell(source, "watch", false, "watch party: " .. why(how))
        return
    end
    local state = registry:state(party)
    tell(source, "watch", true, string.format("watch party %s %s: %s. Open /watch and press Copy browser link, " ..
        "then open it in Edge or Chrome with the Open77 Watch Party extension.",
        party.code, how == "switched" and "switched to a new title" or "started", describe(state)))
end)

RegisterNetEvent("opx:watch:control", function(payload)
    local source = source
    if source == nil then return end
    local party, reason = controlFor(source, payload)
    if party == nil then
        TriggerClientEvent("opx:watch:result", source, { ok = false, text = why(reason) })
        return
    end
    TriggerClientEvent("opx:watch:result", source, { ok = true, text = describe(registry:state(party)) })
end)

RegisterNetEvent("opx:watch:join", function(payload)
    local source = source
    if source == nil or type(payload) ~= "table" then return end
    local party, key = registry:join(source, tostring(payload.code or ""), nameOf(source))
    if party == nil then
        TriggerClientEvent("opx:watch:result", source, { ok = false, text = why(key) })
        return
    end
    sendYou(source)
    broadcast(nil)
    TriggerClientEvent("opx:watch:result", source, { ok = true, text = "joined " .. describe(registry:state(party)) })
end)

RegisterNetEvent("opx:watch:leave", function()
    local source = source
    if source == nil then return end
    local party = registry:leave(source)
    sendYou(source)
    broadcast(nil)
    TriggerClientEvent("opx:watch:result", source, { ok = party ~= nil,
        text = party and ("left watch party " .. party.code) or why("not_in_a_party") })
end)

RegisterNetEvent("opx:watch:stop", function()
    local source = source
    if source == nil then return end
    local party, reason = stopFor(source)
    TriggerClientEvent("opx:watch:result", source, { ok = party ~= nil,
        text = party and ("ended watch party " .. party.code) or why(reason) })
end)

local function forget(player)
    player = tonumber(player)
    if player == nil then return end
    if registry:leave(player) then broadcast(nil) end
end
AddEventHandler("onPlayerDisconnected", function(playerId) forget(playerId) end)
AddEventHandler("playerDropped", function() forget(source) end)

local HELP = "/watch -- the panel. /watch netflix <link> [title] -- start a party at the TV in front of you. " ..
    "/watch play | pause | seek 1:02:03 | +30 | -10 | join <code> | leave | stop | link | status"

RegisterCommand("watch", function(source, args, raw)
    source = tonumber(source) or 0
    if source <= 0 then
        print("[opx_watchparty] " .. #registry:list() .. " watch part(ies)")
        for _, state in ipairs(registry:list()) do print("  " .. describe(state)) end
        return
    end
    local verb = (args[1] or ""):lower()
    if verb == "" or verb == "panel" then
        TriggerClientEvent("opx:watch:panel", source, true)
        return
    end
    if verb == "help" then return tell(source, raw, true, HELP) end
    if verb == "netflix" or verb == "start" then
        local link = args[2]
        local id, reason = WatchNetflix.parse(link or "")
        if id == nil then return tell(source, raw, false, "watch party: " .. why(reason)) end
        local title = table.concat(args, " ", 3)
        -- The television is the client's to pick: it knows which set is in front
        -- of the player (open77_media's snapshot), the server does not.
        TriggerClientEvent("opx:watch:pick", source, { netflixId = id, title = title })
        return
    end
    if verb == "join" then
        local code = (args[2] or ""):upper()
        if code == "" then
            TriggerClientEvent("opx:watch:joinnear", source, true)
            return
        end
        local party, key = registry:join(source, code, nameOf(source))
        if party == nil then return tell(source, raw, false, "watch party: " .. why(key)) end
        sendYou(source)
        broadcast(nil)
        return tell(source, raw, true, "joined " .. describe(registry:state(party)) ..
            ". /watch link copies your browser link.")
    end
    if verb == "leave" then
        local party = registry:leave(source)
        sendYou(source)
        broadcast(nil)
        if party == nil then return tell(source, raw, false, why("not_in_a_party")) end
        return tell(source, raw, true, "left watch party " .. party.code)
    end
    if verb == "stop" then
        local party, reason = stopFor(source)
        if party == nil then return tell(source, raw, false, why(reason)) end
        return tell(source, raw, true, "ended watch party " .. party.code)
    end
    if verb == "link" then
        if registry:partyOf(source) == nil then return tell(source, raw, false, why("not_in_a_party")) end
        TriggerClientEvent("opx:watch:copylink", source, true)
        return
    end
    if verb == "status" then
        local party = registry:partyOf(source)
        if party == nil then return tell(source, raw, false, why("not_in_a_party")) end
        return tell(source, raw, true, describe(registry:state(party)))
    end
    local payload
    if verb == "play" or verb == "pause" or verb == "toggle" then
        payload = { action = verb }
    elseif verb == "seek" then
        payload = { time = args[2] or "" }
    elseif verb:match("^[+-]") then
        payload = { time = verb }
    else
        return tell(source, raw, false, "unknown: " .. verb .. ". " .. HELP)
    end
    local party, reason = controlFor(source, payload)
    if party == nil then return tell(source, raw, false, "watch party: " .. why(reason)) end
    return tell(source, raw, true, describe(registry:state(party)))
end, false)

-- ---------------------------------------------------------------------------
-- The browser side
-- ---------------------------------------------------------------------------

local httpEnv = {
    version = VERSION,
    encode = function(value) return json.encode(value) end,
    decode = function(text) return json.decode(text) end,
    tvPage = function(code) return WatchTvPage.html(code) end,
    selftest = function()
        local party, key = registry:start(-1, { netflixId = "80057281", title = "Self test", test = true,
            name = "self-test" })
        return party, key
    end,
}

local function listen()
    if type(Open77.http) ~= "table" or type(Open77.http.listen) ~= "function" then
        httpReason = "no_http_listen_on_this_server"
        return
    end
    local ok, route, reason = pcall(Open77.http.listen, "/", function(req, res)
        local handled, status, body, headers, changed = pcall(WatchHttp.handle, registry, req, httpEnv)
        if not handled then
            print("[opx_watchparty] http handler error: " .. tostring(status))
            res.send(500, json.encode({ ok = false, error = "handler_error" }),
                { ["Content-Type"] = "application/json; charset=utf-8", ["Access-Control-Allow-Origin"] = "*" })
            return
        end
        res.send(status, body, headers)
        if changed then broadcast(nil) end
    end)
    if ok and route then
        httpRoute = route
    else
        httpReason = tostring(ok and reason or route)
    end
end

AddEventHandler("onResourceStart", function(name)
    if name ~= GetCurrentResourceName() then return end
    listen()
    if httpRoute then
        print(string.format("[opx_watchparty] ready %s: browsers reach %s (served at %s); televisions show %s/v1/tv/<code>",
            VERSION, PUBLIC_BASE, tostring(httpRoute), PUBLIC_BASE))
    else
        print(string.format("[opx_watchparty] ready %s WITHOUT browser sync: %s (the server's httpHandlers listener " ..
            "must be on); parties still run in game", VERSION, tostring(httpReason)))
    end
    broadcast(nil)
end)

CreateThread(function()
    local beat = 0
    while true do
        Wait(2000)
        local ended = registry:sweep()
        beat = beat + 1
        if #ended > 0 or (registry.count > 0 and beat % 3 == 0) then broadcast(nil) end
    end
end)
