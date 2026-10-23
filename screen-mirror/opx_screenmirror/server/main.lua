-- opx_screenmirror -- server/main.lua
--
-- The one job: hold the screen-mirror's viewer link. The room service on this
-- host reads server/config.lua itself and puts the link behind the Mirror app on
-- the room's home screen, so that is where a player meets the mirror now (the
-- capture cinema records are gone -- the room is the screen). The `link` export
-- below remains for a deployment that still names this resource on a record; it
-- answers open77_media alone. The link ends in the mirror mark so a television
-- page shows it as a live view-only screen rather than probing it like a website
-- (`MIRROR_MARK` = "#open77-mirror" in open77_media's web/tv.js).

local config = type(OpxScreenMirrorConfig) == "table" and OpxScreenMirrorConfig or {}
local MARK = "#open77-mirror"

---The configured link, as one a television page will show as the remote screen:
---https, no whitespace, and ending in the mirror mark (added if the operator left
---it off). nil and a reason otherwise.
local function accept(url)
    if type(url) ~= "string" or url == "" then return nil, "not_configured" end
    if #url > 1000 then return nil, "too_long" end
    if not url:match("^https://") then return nil, "not_https" end
    if url:find("%s") or url:find("%c") then return nil, "whitespace" end
    if url:sub(-#MARK) ~= MARK then url = url .. MARK end
    return url
end

local LINK, linkReason = accept(config.url)

-- A host without cross-resource exports still runs; the room's Mirror app then
-- just says the mirror is unavailable.
local function publish(name, fn)
    if exports == nil then return false end
    return (pcall(exports, name, fn))
end

-- Only open77_media is answered: the link carries the channel + token, and the
-- television page is the only thing that should put it on a screen.
publish("link", function()
    if GetInvokingResource() ~= "open77_media" then return nil end
    return LINK
end)

AddEventHandler("onResourceStart", function(name)
    if name ~= GetCurrentResourceName() then return end
    if LINK then
        print("[opx_screenmirror] ready: mirror link set for this room's Mirror app")
    else
        print("[opx_screenmirror] ready WITHOUT a mirror link (" .. tostring(linkReason) ..
            "): this room's Mirror app will say so")
    end
end)
