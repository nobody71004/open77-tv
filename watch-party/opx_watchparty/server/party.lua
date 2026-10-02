-- opx_watchparty -- server/party.lua
--
-- The watch parties: who is in which, which television each one is on, and the
-- one playhead each party shares. Pure: the clock and the dice are handed in, so
-- the tests run it without a server, and `main.lua` is only wiring.
--
-- A party is a Netflix title, a playhead and its members. Each member has a
-- BROWSER KEY: the secret their browser extension shows the server, so a browser
-- can move the party (pause it, seek it) only for a player who is in it. A key
-- dies with the membership. Everyone may READ a party by its code -- the code is
-- on the television for anyone standing in front of it to see.

WatchParty = {}

WatchParty.MAX_PARTIES = 32
WatchParty.MAX_MEMBERS = 64
WatchParty.MAX_VIEWERS = 64
WatchParty.VIEWER_STALE_MS = 15000      -- a browser not heard from for this long is gone
WatchParty.EMPTY_GRACE_MS = 120000      -- a party with no member left ends after this
WatchParty.TEST_LIFETIME_MS = 120000    -- a self-test party ends after this
WatchParty.CONTROL_WINDOW_MS = 10000
WatchParty.CONTROL_LIMIT = 8            -- browser commands per key per window
WatchParty.IN_SYNC_MS = 2500            -- a browser this close to the party is "in sync"
WatchParty.MAX_TITLE = 80
WatchParty.MAX_TIME_MS = 12 * 3600 * 1000

local CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
local KEY_ALPHABET = "abcdefghijkmnpqrstuvwxyz23456789"
local CODE_LENGTH = 6
local KEY_LENGTH = 12

local Registry = {}
Registry.__index = Registry

---@param options table { clock = function() -> ms, random = function(n) -> 1..n }
function WatchParty.new(options)
    options = options or {}
    return setmetatable({
        clock = options.clock or function() return 0 end,
        random = options.random or function(n) return math.random(n) end,
        parties = {},   -- code -> party
        byTv = {},      -- tvId -> code
        byKey = {},     -- key -> { code, player }
        byPlayer = {},  -- player -> code
        count = 0,
    }, Registry)
end

local function randomString(self, alphabet, length)
    local out = {}
    for i = 1, length do
        local n = self.random(#alphabet)
        out[i] = alphabet:sub(n, n)
    end
    return table.concat(out)
end

---A title a player typed, made safe to show: no control characters, no runs of
---spaces, at most MAX_TITLE characters (cut on a character, not a byte).
function WatchParty.cleanTitle(text)
    if type(text) ~= "string" then return "" end
    text = text:gsub("[%c]", " "):gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
    if #text > WatchParty.MAX_TITLE then
        local cut = WatchParty.MAX_TITLE
        -- back off a UTF-8 continuation byte so the title stays valid text
        while cut > 0 do
            local b = text:byte(cut + 1)
            if b == nil or b < 0x80 or b >= 0xC0 then break end
            cut = cut - 1
        end
        text = text:sub(1, cut)
    end
    return text
end

function WatchParty.validCode(code)
    return type(code) == "string" and #code == CODE_LENGTH and code:match("^[A-Z2-9]+$") ~= nil
end

function WatchParty.validKey(key)
    return type(key) == "string" and #key == KEY_LENGTH and key:match("^[a-z2-9]+$") ~= nil
end

local function finiteNumber(v)
    return type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge
end

function Registry:now()
    return self.clock()
end

function Registry:newCode()
    for _ = 1, 64 do
        local code = randomString(self, CODE_ALPHABET, CODE_LENGTH)
        if self.parties[code] == nil then return code end
    end
    return nil
end

function Registry:newKey()
    for _ = 1, 64 do
        local key = randomString(self, KEY_ALPHABET, KEY_LENGTH)
        if self.byKey[key] == nil then return key end
    end
    return nil
end

function Registry:get(code)
    if type(code) ~= "string" then return nil end
    return self.parties[code:upper()]
end

function Registry:partyOf(player)
    local code = self.byPlayer[player]
    return code and self.parties[code] or nil
end

function Registry:atTv(tvId)
    if tvId == nil then return nil end
    local code = self.byTv[tvId]
    return code and self.parties[code] or nil
end

function Registry:keyOf(party, player)
    local member = party and party.members[player]
    return member and member.key or nil
end

local function bump(self, party)
    party.rev = party.rev + 1
    party.updatedAt = self:now()
end

function Registry:addMember(party, player, name)
    local existing = party.members[player]
    if existing then return existing.key end
    if party.memberCount >= WatchParty.MAX_MEMBERS then return nil, "party_full" end
    local key = self:newKey()
    if key == nil then return nil, "no_key" end
    party.members[player] = { key = key, name = name or ("player " .. tostring(player)), joinedAt = self:now() }
    party.memberCount = party.memberCount + 1
    party.emptySince = nil
    self.byKey[key] = { code = party.code, player = player }
    self.byPlayer[player] = party.code
    if party.host == nil then
        party.host = player
        party.hostName = party.members[player].name
    end
    return key
end

function Registry:removeMember(party, player)
    local member = party.members[player]
    if member == nil then return false end
    self.byKey[member.key] = nil
    party.viewers[member.key] = nil
    party.members[player] = nil
    party.memberCount = party.memberCount - 1
    if self.byPlayer[player] == party.code then self.byPlayer[player] = nil end
    if party.host == player then
        party.host, party.hostName = nil, nil
        local lowest
        for other in pairs(party.members) do
            if lowest == nil or other < lowest then lowest = other end
        end
        if lowest ~= nil then
            party.host = lowest
            party.hostName = party.members[lowest].name
        end
    end
    if party.memberCount <= 0 then
        party.memberCount = 0
        party.emptySince = self:now()
    end
    bump(self, party)
    return true
end

---Sets what the party watches. A different title starts it over, paused at the
---beginning; the same title only renames it.
function Registry:setTitle(party, netflixId, title)
    local now = self:now()
    if netflixId ~= nil and netflixId ~= party.netflixId then
        party.netflixId = netflixId
        party.playhead = WatchPlayhead.new(now)
        party.title = WatchParty.cleanTitle(title)
        party.titleFrom = party.title ~= "" and "player" or nil
        party.viewers = {}
        party.viewerCount = 0
    elseif type(title) == "string" and WatchParty.cleanTitle(title) ~= "" then
        party.title = WatchParty.cleanTitle(title)
        party.titleFrom = "player"
    end
    bump(self, party)
end

---Starts a party, or switches the one on that television to this title.
---@param opts table { netflixId, title, tvId, name, test }
---@return table|nil party, string|nil key or reason, string|nil how ("started"|"switched")
function Registry:start(player, opts)
    opts = opts or {}
    local netflixId = opts.netflixId
    if type(netflixId) ~= "string" or not netflixId:match("^%d+$") then return nil, "netflix_title_expected" end
    local now = self:now()
    local onTv = self:atTv(opts.tvId)
    if onTv then
        local current = self:partyOf(player)
        if current and current ~= onTv then self:removeMember(current, player) end
        local key, reason = self:addMember(onTv, player, opts.name)
        if key == nil then return nil, reason end
        self:setTitle(onTv, netflixId, opts.title)
        onTv.host = player
        onTv.hostName = onTv.members[player].name
        return onTv, key, "switched"
    end
    local current = self:partyOf(player)
    if current then self:removeMember(current, player) end
    if self.count >= WatchParty.MAX_PARTIES then
        self:sweep()
        if self.count >= WatchParty.MAX_PARTIES then return nil, "too_many_parties" end
    end
    local code = self:newCode()
    if code == nil then return nil, "no_code" end
    local party = {
        code = code,
        tvId = opts.tvId,
        netflixId = netflixId,
        title = WatchParty.cleanTitle(opts.title),
        members = {},
        memberCount = 0,
        viewers = {},
        viewerCount = 0,
        playhead = WatchPlayhead.new(now),
        rev = 1,
        createdAt = now,
        updatedAt = now,
        test = opts.test == true,
    }
    party.titleFrom = party.title ~= "" and "player" or nil
    self.parties[code] = party
    self.count = self.count + 1
    if opts.tvId ~= nil then self.byTv[opts.tvId] = code end
    local key, reason = self:addMember(party, player, opts.name)
    if key == nil then
        self:stop(party)
        return nil, reason
    end
    return party, key, "started"
end

---@return table|nil party, string|nil key or reason
function Registry:join(player, code, name)
    local party = self:get(code)
    if party == nil then return nil, "no_such_party" end
    local current = self:partyOf(player)
    if current and current ~= party then self:removeMember(current, player) end
    local key, reason = self:addMember(party, player, name)
    if key == nil then return nil, reason end
    bump(self, party)
    return party, key
end

---@return table|nil the party left
function Registry:leave(player)
    local party = self:partyOf(player)
    if party == nil then return nil end
    self:removeMember(party, player)
    return party
end

---Ends a party for everyone. Its keys die with it.
---@return table the ended party (members as they were, for notices)
function Registry:stop(party)
    if self.parties[party.code] ~= party then return party end
    for player, member in pairs(party.members) do
        self.byKey[member.key] = nil
        if self.byPlayer[player] == party.code then self.byPlayer[player] = nil end
    end
    if party.tvId ~= nil and self.byTv[party.tvId] == party.code then self.byTv[party.tvId] = nil end
    self.parties[party.code] = nil
    self.count = self.count - 1
    party.endedAt = self:now()
    return party
end

---Moves the party. `args`: positionMs for `seek`, deltaMs for `nudge`.
---@return boolean|nil ok, string|nil reason
function Registry:control(party, action, args)
    args = args or {}
    local now = self:now()
    local p = party.playhead
    if action == "play" then
        WatchPlayhead.play(p, now)
    elseif action == "pause" then
        WatchPlayhead.pause(p, now)
    elseif action == "toggle" then
        WatchPlayhead.toggle(p, now)
    elseif action == "seek" then
        local ms = args.positionMs
        if not finiteNumber(ms) or ms < 0 or ms > WatchParty.MAX_TIME_MS then return nil, "position_out_of_range" end
        WatchPlayhead.seek(p, now, ms)
    elseif action == "nudge" then
        local delta = args.deltaMs
        if not finiteNumber(delta) or math.abs(delta) > WatchParty.MAX_TIME_MS then return nil, "delta_out_of_range" end
        WatchPlayhead.nudge(p, now, delta)
    else
        return nil, "unknown_action"
    end
    bump(self, party)
    return true
end

---A browser asking to move the party, by its key.
---@return table|nil party, string|nil reason
function Registry:controlByKey(code, key, action, args)
    local party = self:get(code)
    if party == nil then return nil, "no_such_party" end
    if not WatchParty.validKey(key) then return nil, "key_required" end
    local owner = self.byKey[key]
    if owner == nil or owner.code ~= party.code then return nil, "key_not_in_party" end
    local member = party.members[owner.player]
    if member == nil then return nil, "key_not_in_party" end
    local now = self:now()
    if member.windowAt == nil or now - member.windowAt > WatchParty.CONTROL_WINDOW_MS then
        member.windowAt, member.windowCount = now, 0
    end
    if member.windowCount >= WatchParty.CONTROL_LIMIT then return nil, "too_many_commands" end
    member.windowCount = member.windowCount + 1
    local ok, reason = self:control(party, action, args)
    if not ok then return nil, reason end
    party.lastBy = member.name
    return party, nil, owner.player
end

---What a viewer's browser says about itself; returns nothing but the change flag.
---
---`viewerId` is the key for a member's browser, or an anonymous id for one that
---only follows. A member's browser may also teach the party the film's length and
---its title (when no player named it), and only for the party's own title.
---@return boolean|nil changed, string|nil reason
function Registry:report(code, viewerId, keyed, report)
    local party = self:get(code)
    if party == nil then return nil, "no_such_party" end
    if keyed then
        local owner = self.byKey[viewerId]
        if owner == nil or owner.code ~= party.code then return nil, "key_not_in_party" end
    elseif type(viewerId) ~= "string" or not viewerId:match("^[%w_-]+$") or #viewerId < 8 or #viewerId > 40 then
        return nil, "viewer_id_expected"
    else
        viewerId = "anon:" .. viewerId
    end
    local now = self:now()
    local viewer = party.viewers[viewerId]
    if viewer == nil then
        if party.viewerCount >= WatchParty.MAX_VIEWERS then return nil, "too_many_viewers" end
        viewer = { firstSeen = now }
        party.viewers[viewerId] = viewer
        party.viewerCount = party.viewerCount + 1
    end
    viewer.lastSeen = now
    viewer.keyed = keyed == true
    viewer.netflixId = type(report.netflixId) == "string" and report.netflixId:match("^%d+$") and report.netflixId or nil
    viewer.positionMs = finiteNumber(report.positionMs) and report.positionMs or nil
    viewer.playing = report.playing == true
    local changed = false
    if viewer.netflixId == party.netflixId and viewer.positionMs ~= nil then
        local target = WatchPlayhead.position(party.playhead, now)
        viewer.driftMs = math.floor(viewer.positionMs - target + 0.5)
        if keyed then
            if finiteNumber(report.durationMs) and WatchPlayhead.setDuration(party.playhead, now, report.durationMs) then
                changed = true
            end
            local title = WatchParty.cleanTitle(report.title)
            if title ~= "" and party.titleFrom ~= "player" and title ~= party.title then
                party.title = title
                party.titleFrom = "browser"
                changed = true
            end
        end
    else
        viewer.driftMs = nil
    end
    if changed then bump(self, party) end
    return changed
end

---Drops browsers gone quiet, ends parties left empty and self-tests past their
---time. Returns the parties that ended.
function Registry:sweep()
    local now = self:now()
    local ended = {}
    for _, party in pairs(self.parties) do
        for id, viewer in pairs(party.viewers) do
            if now - (viewer.lastSeen or 0) > WatchParty.VIEWER_STALE_MS then
                party.viewers[id] = nil
                party.viewerCount = party.viewerCount - 1
            end
        end
        if (party.memberCount <= 0 and party.emptySince ~= nil and now - party.emptySince > WatchParty.EMPTY_GRACE_MS)
            or (party.test and now - party.createdAt > WatchParty.TEST_LIFETIME_MS) then
            ended[#ended + 1] = party
        end
    end
    for _, party in ipairs(ended) do self:stop(party) end
    return ended
end

---The party as everyone may see it (no key in it).
function Registry:state(party)
    local now = self:now()
    local positionMs, playing = WatchPlayhead.position(party.playhead, now)
    local total, inSync = 0, 0
    for _, viewer in pairs(party.viewers) do
        if now - (viewer.lastSeen or 0) <= WatchParty.VIEWER_STALE_MS then
            total = total + 1
            if viewer.driftMs ~= nil and math.abs(viewer.driftMs) <= WatchParty.IN_SYNC_MS
                and viewer.playing == playing then
                inSync = inSync + 1
            end
        end
    end
    return {
        code = party.code,
        tvId = party.tvId,
        netflixId = party.netflixId,
        title = party.title,
        playing = playing,
        positionMs = positionMs,
        durationMs = party.playhead.durationMs,
        serverMs = now,
        rev = party.rev,
        host = party.host,
        hostName = party.hostName,
        lastBy = party.lastBy,
        members = party.memberCount,
        browsers = total,
        inSync = inSync,
        test = party.test or nil,
    }
end

---Every party's public state, in code order.
function Registry:list()
    local out = {}
    for _, party in pairs(self.parties) do out[#out + 1] = self:state(party) end
    table.sort(out, function(a, b) return a.code < b.code end)
    return out
end
