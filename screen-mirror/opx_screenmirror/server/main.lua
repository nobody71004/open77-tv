-- opx_screenmirror -- server/main.lua
--
-- The one job: hold the screen-mirror's viewer link and hand it to open77_media
-- when a capture cinema is put up (`linkedUrl` in open77_media's server/main.lua
-- asks this resource's `link` export). The link ends in the mirror mark so the
-- television page shows it as a live view-only screen rather than probing it like
-- a website (`MIRROR_MARK` = "#open77-mirror" in open77_media's web/tv.js).

local config = type(OpxScreenMirrorConfig) == "table" and OpxScreenMirrorConfig or {}
local MARK = "#open77-mirror"

---The configured link, as one the television page will show as the remote screen:
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

-- A host without cross-resource exports still runs; the capture cinema then just
-- says the capture is unavailable.
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
        print("[opx_screenmirror] ready: capture link set for the capture cinemas")
    else
        print("[opx_screenmirror] ready WITHOUT a capture link (" .. tostring(linkReason) ..
            "): a capture cinema will say so")
    end
end)
