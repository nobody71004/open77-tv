-- opx_tvbrowser -- client/main.lua
--
-- Which television. open77_media owns the sets and pushes the whole list to every
-- resource that registers `open77:media:snapshot`, each set with its position and
-- the reach its own remote uses. The set in front of the player is the nearest
-- one in reach; it is pointed at the shared browser through open77_media's own
-- control message, and a cinema is put up through its own spawn request.

local DEFAULT_REACH = 3.0
local screens = {}         -- open77_media's sets: id -> spec

local function myPosition()
    if type(Open77) ~= "table" or type(Open77.character) ~= "table" then return nil end
    local ok, x, y, z = pcall(Open77.character.position)
    if not ok or type(x) ~= "number" then return nil end
    return x, y, z
end

---The nearest set, its distance, and whether the player is within its reach.
local function nearestScreen()
    local x, y, z = myPosition()
    if x == nil then return nil end
    local best, bestDistance
    for _, spec in pairs(screens) do
        local p = spec.position
        if type(p) == "table" then
            local dx, dy, dz = (tonumber(p.x) or 0) - x, (tonumber(p.y) or 0) - y, (tonumber(p.z) or 0) - z
            local d = math.sqrt(dx * dx + dy * dy + dz * dz)
            if bestDistance == nil or d < bestDistance then best, bestDistance = spec, d end
        end
    end
    if best == nil then return nil end
    local reach = tonumber(best.reach) or DEFAULT_REACH
    return best, bestDistance, bestDistance <= reach + 0.5
end

local function report(ok, text)
    print("[opx_tvbrowser] " .. (ok and "" or "refused: ") .. text)
    TriggerServerEvent("opx:tvbrowser:report", { ok = ok == true, text = text })
end

local function nameOf(spec)
    return tostring(spec.label or spec.record or ("set " .. tostring(spec.id)))
end

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

RegisterNetEvent("opx:tvbrowser:put", function(payload)
    if type(payload) ~= "table" or OpxTvBrowserLink.accept(payload.url) == nil then return end
    local screen, distance, inReach = nearestScreen()
    if screen == nil or not inReach then
        local where = screen and string.format(" (the nearest, %s, is %.0f m away)", nameOf(screen), distance) or ""
        return report(false, "no TV in reach" .. where .. ": stand at one, or /browser cinema puts up a 150 ft screen")
    end
    if screen.url == payload.url then
        return report(true, nameOf(screen) .. " already shows the shared browser: press F8 at the screen to use it")
    end
    TriggerServerEvent("open77:media:control", "url", { id = tonumber(screen.id), url = payload.url })
    report(true, "the shared browser is on " .. nameOf(screen) .. ": press F8 at the screen to use it")
end)

RegisterNetEvent("opx:tvbrowser:off", function(payload)
    if type(payload) ~= "table" then return end
    local screen, _, inReach = nearestScreen()
    if screen == nil or not inReach then return report(false, "no TV in reach") end
    if not OpxTvBrowserLink.isShared(screen.url) then
        return report(false, nameOf(screen) .. " is not showing the shared browser")
    end
    TriggerServerEvent("open77:media:control", "url", { id = tonumber(screen.id), url = "" })
    report(true, "the shared browser is off " .. nameOf(screen))
end)

RegisterNetEvent("opx:tvbrowser:cinema", function(payload)
    if type(payload) ~= "table" or OpxTvBrowserLink.accept(payload.url) == nil then return end
    -- The facing is offered, as open77_media's own remote does: the server
    -- places the set in front of the player and clamps whatever arrives.
    local yaw = nil
    if type(Open77) == "table" and type(Open77.character) == "table" and type(Open77.character.state) == "function" then
        local ok, character = pcall(Open77.character.state)
        if ok and type(character) == "table" then yaw = character.yaw end
    end
    TriggerServerEvent("open77:media:spawn", { record = tostring(payload.record or "cinema.150ft"), url = payload.url, yaw = yaw })
    report(true, "putting up a 150 ft cinema in front of you with the shared browser on it: press F8 at the screen to use it")
end)

AddEventHandler("onClientResourceStart", function(name)
    if name ~= GetCurrentResourceName() then return end
    -- open77_media pushes its sets on changes only; ask once for the current list
    -- (its own join request: the answer is the same whole-set snapshot).
    TriggerServerEvent("open77:media:ready")
    print("[opx_tvbrowser] client ready (/browser)")
end)
