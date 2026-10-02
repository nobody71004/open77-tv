-- opx_tvbrowser -- server/main.lua
--
-- The commands. The server holds the link and hands it only to the player who
-- asked; that player's client picks the television in front of them from
-- open77_media's set list and asks open77_media for it, through the same control
-- message its remote sends when a link is pasted.

local VERSION = "1.0.0"
local config = type(OpxTvBrowserConfig) == "table" and OpxTvBrowserConfig or {}
local LINK, linkReason = OpxTvBrowserLink.accept(config.url)
local CINEMA = type(config.cinemaRecord) == "string" and config.cinemaRecord or "cinema.150ft"

local HELP = "/browser -- the shared browser on the TV in front of you. /browser cinema -- a 150 ft cinema " ..
    "showing it. /browser off -- take it off this TV. Press F8 at the screen to use it."

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
        TriggerClientEvent("opx:tvbrowser:cinema", source, { url = LINK, record = CINEMA })
    elseif verb == "off" then
        TriggerClientEvent("opx:tvbrowser:off", source, { url = LINK })
    else
        TriggerClientEvent("opx:tvbrowser:put", source, { url = LINK })
    end
end, false)

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
