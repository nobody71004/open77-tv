-- opx_tvbrowser -- server/main.lua
--
-- The commands. The server holds the link and hands it only to the player who
-- asked; that player's client picks the television in front of them from
-- open77_media's set list and asks open77_media for it, through the same control
-- message its remote sends when a link is pasted.

local VERSION = "1.0.2"
local config = type(OpxTvBrowserConfig) == "table" and OpxTvBrowserConfig or {}
local LINK, linkReason = OpxTvBrowserLink.accept(config.url)
local CINEMA = type(config.cinemaRecord) == "string" and config.cinemaRecord or "cinema.150ft.browser"
local CINEMA_100 = type(config.cinema100Record) == "string" and config.cinema100Record or "cinema.100ft.browser"

local HELP = "/browser -- the shared browser on the TV in front of you. /browser cinema -- a 150 ft cinema " ..
    "showing it (/browser cinema 100 -- a 100 ft one). /browser off -- take it off this TV. " ..
    "Press F8 at the screen to use it; Ctrl+V pastes a link into it."

---Answers the player who typed a command (both channels the other resources use:
---the host's own result line, and the gamemode's answer line on this server).
local function tell(source, raw, success, text)
    print("[opx_tvbrowser] " .. tostring(source) .. ": " .. text)
    if source == nil or source <= 0 then return end
    TriggerClientEvent("open77:command:result", source, raw or "", success == true, text)
    if success == true then
        TriggerClientEvent("opx:net:runtime:commandAnswer", source, raw or "", "success", text, false)
    end
end

RegisterCommand("browser", function(source, args, raw)
    source = tonumber(source) or 0
    if source <= 0 then
        print("[opx_tvbrowser] " .. (LINK and "configured" or ("not configured: " .. tostring(linkReason))))
        return
    end
    local verb, unknown = OpxTvBrowserLink.verb(args)
    if verb == nil then return tell(source, raw, false, "unknown: " .. tostring(unknown) .. ". " .. HELP) end
    if verb == "help" then return tell(source, raw, true, HELP) end
    if LINK == nil then
        return tell(source, raw, false, "the shared browser is not set up on this server")
    end
    if verb == "cinema" then
        local size, typed = OpxTvBrowserLink.cinemaSize(args)
        if size == nil then
            return tell(source, raw, false, "no " .. tostring(typed) .. " cinema: /browser cinema (150 ft) or /browser cinema 100")
        end
        TriggerClientEvent("opx:tvbrowser:cinema", source, { url = LINK, record = size == "100" and CINEMA_100 or CINEMA })
    elseif verb == "off" then
        TriggerClientEvent("opx:tvbrowser:off", source, { url = LINK })
    else
        TriggerClientEvent("opx:tvbrowser:put", source, { url = LINK })
    end
end, false)

-- The browser cinemas in the TV menu (open77_media's `cinema.100ft.browser` and
-- `cinema.150ft.browser`, `linkFrom = "opx_tvbrowser"`) ask for the link when the
-- set is put up. Only open77_media is answered: the link carries the viewer password.
local function publish(name, fn)
    -- A host without cross-resource exports still runs the commands.
    if exports == nil then return false end
    return (pcall(exports, name, fn))
end
publish("link", function()
    if GetInvokingResource() ~= "open77_media" then return nil end
    return LINK
end)

-- What the client did with a request, said back to the player in chat.
RegisterNetEvent("opx:tvbrowser:report", function(payload)
    local source = source
    if source == nil or type(payload) ~= "table" then return end
    local text = tostring(payload.text or "")
    if #text > 300 then text = text:sub(1, 300) end
    tell(source, "browser", payload.ok == true, text)
end)

AddEventHandler("onResourceStart", function(name)
    if name ~= GetCurrentResourceName() then return end
    if LINK then
        print(string.format("[opx_tvbrowser] ready %s: /browser puts the shared browser on a TV", VERSION))
    else
        print(string.format("[opx_tvbrowser] ready %s WITHOUT a browser link (%s): /browser says so",
            VERSION, tostring(linkReason)))
    end
end)
