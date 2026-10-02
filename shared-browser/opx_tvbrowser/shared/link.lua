-- opx_tvbrowser -- shared/link.lua
--
-- The one rule both halves need: what a shared-browser link is. open77_media's
-- television page frames a link ending in this mark as the shared browser
-- (`SHARED_BROWSER_MARK` in its web/tv.js), so a link without it would go to the
-- probe like any website and be framed as one.

OpxTvBrowserLink = OpxTvBrowserLink or {}

OpxTvBrowserLink.MARK = "#open77-shared-browser"

---The configured link if it is one this resource may put on a screen, else nil
---and the reason.
function OpxTvBrowserLink.accept(url)
    if type(url) ~= "string" or url == "" then return nil, "not_configured" end
    if #url > 1000 then return nil, "too_long" end
    if not url:match("^https://[%w%.%-]+/") then return nil, "not_https" end
    if url:find("%s") or url:find("%c") then return nil, "whitespace" end
    if url:sub(-#OpxTvBrowserLink.MARK) ~= OpxTvBrowserLink.MARK then return nil, "no_mark" end
    return url
end

---Whether a television's current link is the shared browser.
function OpxTvBrowserLink.isShared(url)
    return type(url) == "string" and url:sub(-#OpxTvBrowserLink.MARK) == OpxTvBrowserLink.MARK
end

---Which browser cinema `/browser cinema [size]` asks for: "100" or "150" (the
---default), else nil and what was typed.
function OpxTvBrowserLink.cinemaSize(args)
    local size = type(args) == "table" and tostring(args[2] or ""):lower() or ""
    size = size:gsub("%s*f[ee]*t$", ""):gsub("%s*foot$", "")
    if size == "" or size == "150" then return "150" end
    if size == "100" then return "100" end
    return nil, size
end

---The words a player reads for a verb they typed: on, cinema, off or help.
function OpxTvBrowserLink.verb(args)
    local verb = type(args) == "table" and tostring(args[1] or ""):lower() or ""
    if verb == "" or verb == "on" or verb == "here" then return "on" end
    if verb == "cinema" or verb == "off" or verb == "help" then return verb end
    return nil, verb
end
