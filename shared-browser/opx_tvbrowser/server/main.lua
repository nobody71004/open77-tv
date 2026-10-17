-- opx_tvbrowser -- server/main.lua
--
-- The commands. The server holds the link and hands it only to the player who
-- asked; that player's client picks the television in front of them from
-- open77_media's set list and asks open77_media for it, through the same control
-- message its remote sends when a link is pasted.

local VERSION = "1.0.3"
local config = type(OpxTvBrowserConfig) == "table" and OpxTvBrowserConfig or {}
local LINK, linkReason = OpxTvBrowserLink.accept(config.url)
local CINEMA = type(config.cinemaRecord) == "string" and config.cinemaRecord or "cinema.150ft.browser"
local CINEMA_100 = type(config.cinema100Record) == "string" and config.cinema100Record or "cinema.100ft.browser"

local HELP = "/browser -- the shared browser on the TV in front of you. /browser cinema -- a 150 ft cinema " ..
    "showing it (/browser cinema 100 -- a 100 ft one). /browser off -- take it off this TV. " ..
    "Press F8 at the screen to use it; Ctrl+V pastes a link into it."

-- The link is a credential, not a URL.
--
-- It carries the browser's viewer password, and that password is a whole session on
-- the room's desktop: whoever has the link can open that desktop from their own PC
-- and use whatever is signed into it there. So /browser -- the one place a link is
-- handed to a player -- is admin-only, and a player who is not an admin is told that
-- rather than being given it. Nothing else changes: the cinemas are put up through
-- this resource's `link` export, which answers open77_media alone.
local ADMIN_ACE = type(config.adminAce) == "string" and config.adminAce ~= ""
    and config.adminAce or "command.media.lock"

--- Whether this player holds the admin ACE, asked of Open77's own ACL.
-- `IsPlayerAceAllowed` is FiveM's name for the question and does not exist in
-- Open77, so `type(...) ~= "function"` was true on every server and this answered
-- false for everyone -- admins included, who were then told the browser was
-- "admin-only on this server (ACE command.media.lock)" while holding exactly that
-- ACE. The host's gate is `Open77.acl.isAllowed(source, 'command.<word>')`, the same
-- one every restricted command passes through; it is tried first and FiveM's name
-- is kept as the fallback for a FiveM-shaped host.
--
-- The call is inside the pcall rather than handed to it: `pcall(Open77.acl.isAllowed,
-- ...)` resolves the field before pcall runs and raises where there is no `acl`
-- table. A host with neither API refuses instead of crashing.
local function isAdmin(source)
    if source == nil or source <= 0 then return true end
    local ok, allowed = pcall(function() return Open77.acl.isAllowed(source, ADMIN_ACE) end)
    if ok then return allowed == true end
    if type(IsPlayerAceAllowed) == "function" then
        return IsPlayerAceAllowed(source, ADMIN_ACE) == true
    end
    return false
end

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
    -- Before the link is read, let alone sent: this command is the one path that
    -- hands the browser's own credentials to a player.
    if not isAdmin(source) then
        return tell(source, raw, false, "the shared browser is admin-only on this server (ACE " ..
            ADMIN_ACE .. ")")
    end
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

-- Whether the host installed `Open77.acl` for THIS resource, which it does only
-- for a manifest that declares `acl.read`. Without it the wrapped read in
-- `isAdmin` raises, the FiveM fallback is absent, and `/browser` tells every
-- player it is admin-only -- the owner holding the ACE included -- with nothing
-- in the log to say why. Printed at start so the two are told apart.
local function aclInstalled()
    return type(Open77) == "table" and type(Open77.acl) == "table"
        and type(Open77.acl.isAllowed) == "function"
end

AddEventHandler("onResourceStart", function(name)
    if name ~= GetCurrentResourceName() then return end
    print(string.format("[opx_tvbrowser] acl: %s", aclInstalled() and
        "installed (acl.read declared)" or
        "NOT INSTALLED -- open77.lua does not declare \"acl.read\", so /browser refuses everyone"))
    if LINK then
        print(string.format("[opx_tvbrowser] ready %s: /browser (admin-only, ACE %s) puts the shared browser on a TV",
            VERSION, ADMIN_ACE))
    else
        print(string.format("[opx_tvbrowser] ready %s WITHOUT a browser link (%s): /browser says so",
            VERSION, tostring(linkReason)))
    end
end)
