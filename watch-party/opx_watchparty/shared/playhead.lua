-- opx_watchparty -- shared/playhead.lua
--
-- The shared playhead: where a watch party's film is, as arithmetic and nothing
-- else. No clock is read here and nothing is sent: every function takes the time
-- it is asked about (milliseconds, any monotonic or wall clock, as long as one
-- party uses one), so the server, the in-game panel, the TV page and the browser
-- extension all agree on the same numbers, and the tests can pin them.
--
-- A playhead is { playing, positionMs, atMs, durationMs }: the film was at
-- `positionMs` at the moment `atMs`, and moves at one second per second while
-- `playing`. It never moves backwards by itself, never goes below 0, and never
-- past `durationMs` once a duration is known (it stops there: the film ended).

WatchPlayhead = {}

local function clamp(ms, durationMs)
    if ms < 0 then ms = 0 end
    if durationMs ~= nil and durationMs > 0 and ms > durationMs then ms = durationMs end
    return ms
end

---A new playhead, paused at the start.
---@param nowMs number
function WatchPlayhead.new(nowMs)
    return { playing = false, positionMs = 0, atMs = nowMs, durationMs = nil }
end

---Where the film is at `nowMs`.
---@return number positionMs, boolean playing (false once it ran into the end)
function WatchPlayhead.position(p, nowMs)
    local ms = p.positionMs
    local playing = p.playing
    if playing then
        local elapsed = nowMs - p.atMs
        if elapsed > 0 then ms = ms + elapsed end
    end
    ms = clamp(ms, p.durationMs)
    if playing and p.durationMs ~= nil and p.durationMs > 0 and ms >= p.durationMs then
        playing = false
    end
    return math.floor(ms + 0.5), playing
end

---Brings the stored point up to `nowMs` (position and state), so the next change
---starts from where the film really is.
local function settle(p, nowMs)
    local ms, playing = WatchPlayhead.position(p, nowMs)
    p.positionMs = ms
    p.playing = playing
    p.atMs = nowMs
end

function WatchPlayhead.play(p, nowMs)
    settle(p, nowMs)
    if p.durationMs ~= nil and p.durationMs > 0 and p.positionMs >= p.durationMs then
        -- Play at the end starts it over, the way every player does.
        p.positionMs = 0
    end
    p.playing = true
    return p
end

function WatchPlayhead.pause(p, nowMs)
    settle(p, nowMs)
    p.playing = false
    return p
end

function WatchPlayhead.toggle(p, nowMs)
    local _, playing = WatchPlayhead.position(p, nowMs)
    if playing then return WatchPlayhead.pause(p, nowMs) end
    return WatchPlayhead.play(p, nowMs)
end

---Jumps to `targetMs`, keeping play or pause as it was.
function WatchPlayhead.seek(p, nowMs, targetMs)
    settle(p, nowMs)
    p.positionMs = math.floor(clamp(targetMs, p.durationMs) + 0.5)
    return p
end

---Moves by `deltaMs` (negative goes back).
function WatchPlayhead.nudge(p, nowMs, deltaMs)
    local ms = WatchPlayhead.position(p, nowMs)
    return WatchPlayhead.seek(p, nowMs, ms + deltaMs)
end

---Learns the film's length. A first duration, or one that differs by more than a
---second (a different cut, a corrected report), replaces the old one.
---@return boolean changed
function WatchPlayhead.setDuration(p, nowMs, durationMs)
    if type(durationMs) ~= "number" or durationMs ~= durationMs or durationMs < 1000
        or durationMs > 12 * 3600 * 1000 then
        return false
    end
    durationMs = math.floor(durationMs + 0.5)
    if p.durationMs ~= nil and math.abs(p.durationMs - durationMs) <= 1000 then return false end
    settle(p, nowMs)
    p.durationMs = durationMs
    p.positionMs = clamp(p.positionMs, durationMs)
    return true
end

---What a follower should do to be where the party is.
---
---`localMs`/`localPlaying`: where this viewer's own player is; `targetMs`/
---`targetPlaying`: where the party is, at the same instant. A seek is asked only
---when the two are more than `toleranceMs` apart (default 2 s): a smaller drift
---is left alone, because a seek is a visible stall and two viewers a frame apart
---are watching together already.
---@return table { seekMs = number|nil, play = true|nil, pause = true|nil, driftMs = number }
function WatchPlayhead.decide(localMs, localPlaying, targetMs, targetPlaying, toleranceMs)
    toleranceMs = toleranceMs or 2000
    local out = { driftMs = math.floor(localMs - targetMs + 0.5) }
    if math.abs(localMs - targetMs) > toleranceMs then
        out.seekMs = math.floor(targetMs + 0.5)
    end
    if targetPlaying and not localPlaying then out.play = true end
    if (not targetPlaying) and localPlaying then out.pause = true end
    return out
end

---"1:02:03", "2:03" or "0:05".
function WatchPlayhead.format(ms)
    if type(ms) ~= "number" or ms ~= ms or ms < 0 then ms = 0 end
    local total = math.floor(ms / 1000)
    local h = math.floor(total / 3600)
    local m = math.floor((total % 3600) / 60)
    local s = total % 60
    if h > 0 then return string.format("%d:%02d:%02d", h, m, s) end
    return string.format("%d:%02d", m, s)
end

---Reads a time a player typed.
---
---Absolute: "1:02:03", "62:03", "95" (seconds), "95s", "12m", "1h", "1h2m3s".
---Relative to `currentMs`: the same with a leading "+" or "-" ("+30", "-10",
---"+1:30"). Returns the absolute target in ms, or nil and a reason.
---@return number|nil targetMs, string|nil reason
function WatchPlayhead.parseTime(text, currentMs)
    if type(text) ~= "string" then return nil, "time_expected" end
    text = text:gsub("^%s+", ""):gsub("%s+$", ""):lower()
    if text == "" then return nil, "time_expected" end
    local sign = text:sub(1, 1)
    local relative = sign == "+" or sign == "-"
    if relative then text = text:sub(2) end
    if text == "" or #text > 16 then return nil, "time_unreadable" end
    local seconds
    if text:match("^%d+$") then
        seconds = tonumber(text)
    elseif text:match("^%d+:%d%d$") then
        local m, s = text:match("^(%d+):(%d%d)$")
        if tonumber(s) > 59 then return nil, "time_unreadable" end
        seconds = tonumber(m) * 60 + tonumber(s)
    elseif text:match("^%d+:%d%d:%d%d$") then
        local h, m, s = text:match("^(%d+):(%d%d):(%d%d)$")
        if tonumber(m) > 59 or tonumber(s) > 59 then return nil, "time_unreadable" end
        seconds = tonumber(h) * 3600 + tonumber(m) * 60 + tonumber(s)
    elseif text:match("^[%dhms]+$") and text:match("%d[hms]") then
        local rest = text
        local h = rest:match("^(%d+)h")
        if h then rest = rest:sub(#h + 2) end
        local m = rest:match("^(%d+)m")
        if m then rest = rest:sub(#m + 2) end
        local s = rest:match("^(%d+)s$")
        if s then rest = "" end
        if rest ~= "" then return nil, "time_unreadable" end
        seconds = (tonumber(h) or 0) * 3600 + (tonumber(m) or 0) * 60 + (tonumber(s) or 0)
    else
        return nil, "time_unreadable"
    end
    if seconds > 12 * 3600 then return nil, "time_too_large" end
    local ms = seconds * 1000
    if relative then
        currentMs = tonumber(currentMs) or 0
        if sign == "-" then ms = currentMs - ms else ms = currentMs + ms end
        if ms < 0 then ms = 0 end
    end
    return math.floor(ms)
end

