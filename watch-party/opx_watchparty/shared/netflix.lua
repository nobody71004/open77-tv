-- opx_watchparty -- shared/netflix.lua
--
-- Which Netflix title a player means, from whatever they pasted: a watch link, a
-- title page, a browse link with `jbv=`, a localised link (`/gb/title/...`), with
-- or without the scheme -- or the bare number. Nothing here talks to Netflix: the
-- film plays in each viewer's own browser, on their own account, and the only
-- thing a party shares is this number and where the film is.

WatchNetflix = {}

local function validId(id)
    return type(id) == "string" and #id >= 5 and #id <= 12 and id:match("^%d+$") ~= nil
end

---The title id in `text`, or nil and a reason.
---@return string|nil id, string|nil reason
function WatchNetflix.parse(text)
    if type(text) ~= "string" then return nil, "netflix_link_expected" end
    text = text:gsub("^%s+", ""):gsub("%s+$", "")
    if text == "" then return nil, "netflix_link_expected" end
    if #text > 512 then return nil, "netflix_link_too_long" end
    if validId(text) then return text end
    local lower = text:lower()
    local host = lower:match("^%a[%w+.-]*://([^/?#]+)") or lower:match("^([^/?#]+)")
    if host == nil then return nil, "not_a_netflix_link" end
    host = host:gsub(":%d+$", "")
    if host ~= "netflix.com" and host:sub(-12) ~= ".netflix.com" then
        return nil, "not_a_netflix_link"
    end
    local id = lower:match("/watch/(%d+)") or lower:match("/title/(%d+)")
        or lower:match("[?&]jbv=(%d+)")
    if not validId(id) then return nil, "no_title_in_link" end
    return id
end

---The page that plays `id`, optionally from `positionMs`, carrying the party's
---code and the viewer's key in the fragment for the browser extension.
---
---The fragment never reaches Netflix (browsers do not send it), and the
---extension reads it before Netflix's own page rewrites the address.
function WatchNetflix.watchUrl(id, positionMs, code, key)
    if not validId(id) then return nil end
    local url = "https://www.netflix.com/watch/" .. id
    local seconds = math.floor((tonumber(positionMs) or 0) / 1000)
    if seconds > 0 then url = url .. "?t=" .. seconds end
    if type(code) == "string" and code ~= "" then
        url = url .. "#opxwatch=" .. code
        if type(key) == "string" and key ~= "" then url = url .. "." .. key end
    end
    return url
end
