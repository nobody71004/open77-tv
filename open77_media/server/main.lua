-- =============================================================================
-- open77_media -- server/main.lua
-- =============================================================================
-- The authoritative half of the television feature.
--
-- The server owns one thing: which screen exists, which prop carries it, and
-- what it is showing. The entity itself is an ordinary world prop created
-- through `Open77.props`, so streaming, buckets, replication and cleanup are the
-- props channel's business -- this resource never touches an entity and has no
-- opinion about one.
--
-- Three consequences of that split are worth stating, because each one is a
-- question someone will ask:
--
--   * A television is in exactly one routing bucket: its prop's. A player in
--     another bucket does not receive the screen, because they do not receive
--     the prop.
--   * A client that joins late receives the current state in one snapshot, and a
--     client that reconnects re-binds from the same snapshot. There is no
--     per-client accumulation anywhere in this file.
--   * A television whose prop is removed by another resource is detected at the
--     next broadcast, because the registry is rebuilt from `Open77.props.all()`
--     rather than trusted. That is deliberately timer-free: nothing here runs on
--     a tick, so a server with no televisions has no background cost at all.
--
-- =============================================================================

local MAX_URL_LENGTH = 512
local MAX_TITLE_LENGTH = 96

local media = {}      -- mediaId (number) -> entry
local nextMediaId = 1
local byProp = {}     -- prop id (string) -> mediaId

local function output(source, raw, success, text)
    print(text)
    if source ~= nil and source > 0 then
        TriggerClientEvent("open77:command:result", source, raw or "", success == true, text)
    end
end

local function number(value)
    local parsed = tonumber(value)
    if parsed == nil then error("number expected: " .. tostring(value), 0) end
    return parsed
end

local function command(name, usage, restricted, handler)
    RegisterCommand(name, function(source, args, raw)
        local ok, err = pcall(handler, source, args, raw)
        if not ok then
            output(source, raw, false, string.format("%s -- usage: %s", tostring(err), usage))
        end
    end, restricted == true)
end

local function callerPosition(source)
    if source == nil or source <= 0 then
        error("this command requires an in-game caller", 0)
    end
    local position = Open77.players.position(source)
    if position == nil then
        error(string.format("player %d has no fresh position snapshot", source), 0)
    end
    return position
end

-- =============================================================================
-- URL POLICY
-- =============================================================================
-- A television loads whatever URL it is handed, inside a Chromium surface that
-- belongs to the game process. That makes this the one place in the feature
-- where a string from a player reaches a browser, so the allow-list is written
-- as "these schemes, nothing else" rather than "these schemes are blocked".
--
-- `javascript:` is the obvious one. `file:` and `data:` matter more than they
-- look: the surface is confined to this resource's own web root by CEF's
-- allowed-files list, but a `file:` navigation is a different code path from a
-- resource read, and there is no reason for a television to need one. Anything
-- without a scheme at all is refused rather than assumed to be https.
--
-- This is enforced on the server, not in the page. A page-side check is a
-- convenience for the person typing; it is not a boundary, because the same
-- message can be sent with any client.
local function acceptUrl(url)
    if type(url) ~= "string" then return nil, "url_must_be_a_string" end
    if url == "" then return "" end
    if #url > MAX_URL_LENGTH then return nil, "url_too_long" end

    local scheme = string.match(url, "^([%a][%w+.-]*):")
    if scheme == nil then return nil, "url_needs_a_scheme" end
    scheme = string.lower(scheme)
    if scheme ~= "http" and scheme ~= "https" then
        return nil, "url_scheme_not_allowed"
    end
    -- Control characters and whitespace never appear in a URL a person means to
    -- type, and they are how a string smuggles a second line into a log or a
    -- header. Refused rather than stripped: silently repairing caller input is
    -- how a caller never learns it is wrong.
    if string.match(url, "%s") or string.match(url, "%c") then
        return nil, "url_contains_whitespace"
    end
    if #url < 12 then return nil, "url_too_short" end
    return url
end

local function acceptText(text, maximum, what)
    if text == nil then return nil end
    if type(text) ~= "string" then return nil, what .. "_must_be_a_string" end
    if #text > maximum then return nil, what .. "_too_long" end
    if string.match(text, "%c") then return nil, what .. "_contains_control_characters" end
    return text
end

-- =============================================================================
-- REPLICATION
-- =============================================================================

---A wire record. Deliberately not the internal entry: the client needs the prop
---id, the record id (for the quad) and the playback state, and nothing else.
---No owner, no proposer, no bucket -- a client that wants a bucket wants the
---prop registry.
local function payload(entry)
    -- `quad` is on the wire, not looked up from `records.lua` on the client, and
    -- that is what makes the quad tunable in game: the server holds the effective
    -- rectangle, so `media.quad` can move a screen by a centimetre without a
    -- rebuild, a resource reload or a second person. The record's own numbers are
    -- only the starting point.
    return {
        id = entry.id,
        prop = entry.prop,
        record = entry.record,
        label = entry.label,
        url = entry.url,
        volume = entry.volume,
        muted = entry.muted,
        paused = entry.paused,
        quad = entry.quad,
        -- The television's world position, so a client can decide which screens
        -- are worth materialising without a second lookup. A client can only
        -- hold a few CEF surfaces (`kMaximumWebSurfaces`, eight per resource),
        -- and a server may hold sixty-four televisions, so the decision has to
        -- be made somewhere and the client is the only party that knows where
        -- the player is standing.
        position = entry.position,
    }
end

---A private copy of a record's quad. Deep, not a reference: the catalogue is
---shared state and one screen tuning itself must not move every other screen
---that happens to use the same record.
local function copyQuad(quad)
    local function axis(value)
        return { value[1], value[2], value[3] }
    end
    return {
        offset = axis(quad.offset),
        right = axis(quad.right),
        up = axis(quad.up),
        width = quad.width,
        height = quad.height,
    }
end

---Every live screen, rebuilt from the props registry rather than from memory.
---
---This is where a television whose prop was removed by someone else disappears:
---the entry is dropped the next time anyone asks for state, with no timer, no
---sweep and nothing running in the background on a server with no televisions.
local function liveEntries()
    local present = {}
    for _, record in ipairs(Open77.props.all()) do
        present[tostring(record.id)] = true
    end

    local result = {}
    local dropped = false
    for id, entry in pairs(media) do
        if present[tostring(entry.prop)] then
            result[#result + 1] = entry
        else
            media[id] = nil
            byProp[tostring(entry.prop)] = nil
            dropped = true
        end
    end
    table.sort(result, function(a, b) return a.id < b.id end)
    return result, dropped
end

local function snapshotFor(entries)
    local out = {}
    for _, entry in ipairs(entries) do out[#out + 1] = payload(entry) end
    return out
end

---Pushes the whole live set to one player, or to everyone.
---
---Whole-set rather than deltas, and that is not laziness. The set is small by
---construction (`MEDIA_MAX` below), the client diffs it in one line, and a
---delta stream is one more place for a late joiner to end up with a screen that
---exists on the server and not on their machine.
---Everyone connected, through the two doors the rest of the server uses.
---
---`Open77.players.all()` is the server-side enumerator (`players.ids` does not
---exist -- it is the name this resource used first, and the cost of being wrong
---about it was every broadcast: a television spawned, the prop appeared, the
---command reported success, and no client was ever told about it). The
---`GetPlayers` fallback and the `pcall` pair are copied from `open77_sandevistan`
---and `freeroam`, which ask the same question, so a host that answers one of them
---answers all three.
local function connectedPlayers()
    if type(Open77.players) == "table" and type(Open77.players.all) == "function" then
        local ok, ids = pcall(Open77.players.all)
        if ok and type(ids) == "table" then return ids end
    end
    if type(GetPlayers) == "function" then
        local ok, ids = pcall(GetPlayers)
        if ok and type(ids) == "table" then return ids end
    end
    return {}
end

local function broadcast(target)
    local entries = liveEntries()
    local snapshot = snapshotFor(entries)
    if target ~= nil then
        TriggerClientEvent("open77:media:snapshot", target, snapshot)
        return
    end
    for _, player in ipairs(connectedPlayers()) do
        TriggerClientEvent("open77:media:snapshot", player, snapshot)
    end
end

local MAX_MEDIA = 64

---Creates a television: one prop, one screen, bound together by prop id.
---
---The prop is created first. If it is refused there is nothing to bind a screen
---to, and reporting that is more useful than a screen that is permanently blank
---because the prop it names does not exist.
---Clamps a caller-supplied facing. The *position* stays server-authoritative --
---this resource reads it from the player snapshot and never accepts one -- but
---the facing is cosmetic, has no authority attached to it, and the server has no
---way to read it (`Open77.players.position` publishes x, y, z and bucket, and
---there is no heading in the snapshot; `prop.pickup` in the props resource hit
---the same wall and records it). So it is accepted from the client and clamped
---to finite degrees, which is the whole of what a yaw can do.
local function acceptYaw(value)
    if value == nil then return 0.0 end
    local yaw = tonumber(value)
    if yaw == nil or yaw ~= yaw or yaw == math.huge or yaw == -math.huge then return 0.0 end
    return math.max(-360.0, math.min(360.0, yaw))
end

local function spawn(recordId, position, yaw, url, source)
    local record = Open77MediaRecord(recordId)
    if record == nil then return nil, "unknown_record" end
    if nextMediaId > MAX_MEDIA then return nil, "too_many_televisions" end

    local cleanUrl, urlError = acceptUrl(url)
    if cleanUrl == nil then return nil, urlError end

    local prop, propError = Open77.props.create({
        model = record.model,
        position = { x = position.x, y = position.y, z = position.z },
        yaw = acceptYaw(yaw),
        bucket = position.bucket,
        -- No collision, and this is a fix for a specific problem rather than a
        -- preference. A television is spawned where its owner is standing (the
        -- same place `prop.here` puts a prop), so with the props default of
        -- static collision the player is *inside* the thing they just spawned
        -- and can be pushed out of the world by it or pinned against it. A
        -- screen is not something to stand on, so the honest answer is that it
        -- has no collision at all -- and the prop can then be moved freely with
        -- `media.place` if it is in the way visually.
        physics = "none",
        collision = false,
    })
    if prop == nil then return nil, "prop_create_failed:" .. tostring(propError) end

    local id = nextMediaId
    nextMediaId = nextMediaId + 1

    local entry = {
        id = id,
        prop = tostring(prop),
        record = record.id,
        label = record.label,
        url = cleanUrl or "",
        volume = 100,
        muted = false,
        paused = false,
        quad = copyQuad(record.quad),
        position = { x = position.x, y = position.y, z = position.z },
        source = source,
    }
    media[id] = entry
    byProp[tostring(prop)] = id
    return entry
end

local function remove(id)
    local entry = media[id]
    if entry == nil then return false end
    media[id] = nil
    byProp[tostring(entry.prop)] = nil
    -- The prop goes with the screen. A screen without its prop would be dropped
    -- by the next `liveEntries` anyway, but removing the prop is what actually
    -- takes the television out of the world, and doing it here means "remove"
    -- from the menu does what it says.
    Open77.props.remove(tonumber(entry.prop) or entry.prop, "media_removed")
    return true
end

local function describeEntry(entry)
    return string.format(
        "media=%d prop=%s record=%s url=%s volume=%d muted=%s paused=%s",
        entry.id, tostring(entry.prop), tostring(entry.record),
        entry.url == "" and "(idle screen)" or entry.url,
        entry.volume, tostring(entry.muted), tostring(entry.paused))
end

-- =============================================================================
-- COMMANDS
-- =============================================================================

command("media.records", "media.records", true, function(source, args, raw)
    local counter = {}
    for _, counterEntry in ipairs(Open77MediaCatalogue()) do
        counter[#counter + 1] = counterEntry
    end
    local lines = { string.format("%d television records:", #counter) }
    for _, record in ipairs(counter) do
        lines[#lines + 1] = string.format(
            "  %-12s %-26s %s  (%.2fx%.2f m)",
            record.id, record.label, record.model, record.quad.width, record.quad.height)
    end
    output(source, raw, true, table.concat(lines, "\n"))
end)

command("media.spawn", "media.spawn <record> [yaw] [url]", true, function(source, args, raw)
    if args.n < 1 or args.n > 3 then error("wrong argument count", 0) end
    local position = callerPosition(source)
    local entry, reason = spawn(args[1], position, args[2] and number(args[2]) or 0.0,
        args[3], source)
    if entry == nil then
        return output(source, raw, false, "media spawn failed: " .. tostring(reason))
    end
    broadcast(nil)
    output(source, raw, true, string.format(
        "television %d created here (%s, prop=%s)", entry.id, entry.record, entry.prop))
end)

command("media.place", "media.place <record> <x> <y> <z> [yaw] [url]", true,
    function(source, args, raw)
        if args.n < 4 or args.n > 6 then error("wrong argument count", 0) end
        local position = {
            x = number(args[2]), y = number(args[3]), z = number(args[4]),
            bucket = source ~= nil and source > 0 and callerPosition(source).bucket or 0,
        }
        local entry, reason = spawn(args[1], position, args[5] and number(args[5]) or 0.0,
            args[6], source)
        if entry == nil then
            return output(source, raw, false, "media spawn failed: " .. tostring(reason))
        end
        broadcast(nil)
        output(source, raw, true, string.format(
            "television %d created at %.2f,%.2f,%.2f (%s)", entry.id,
            position.x, position.y, position.z, entry.record))
    end)

command("media.url", "media.url <id> <url>", true, function(source, args, raw)
    if args.n ~= 2 then error("wrong argument count", 0) end
    local entry = media[math.floor(number(args[1]))]
    if entry == nil then return output(source, raw, false, "no such television") end
    local url, urlError = acceptUrl(args[2])
    if url == nil then
        return output(source, raw, false, "url refused: " .. tostring(urlError))
    end
    entry.url = url
    broadcast(nil)
    output(source, raw, true, string.format(
        "television %d now showing %s", entry.id,
        url == "" and "its idle screen" or url))
end)

command("media.title", "media.title <id> <text>", true, function(source, args, raw)
    if args.n ~= 2 then error("wrong argument count", 0) end
    local entry = media[math.floor(number(args[1]))]
    if entry == nil then return output(source, raw, false, "no such television") end
    local label, labelError = acceptText(args[2], MAX_TITLE_LENGTH, "title")
    if label == nil then
        return output(source, raw, false, "title refused: " .. tostring(labelError))
    end
    entry.label = label
    broadcast(nil)
    output(source, raw, true, string.format("television %d is now '%s'", entry.id, label))
end)

command("media.volume", "media.volume <id> <0-100>", true, function(source, args, raw)
    if args.n ~= 2 then error("wrong argument count", 0) end
    local entry = media[math.floor(number(args[1]))]
    if entry == nil then return output(source, raw, false, "no such television") end
    local volume = math.floor(number(args[2]))
    if volume < 0 or volume > 100 then error("volume must be 0..100", 0) end
    entry.volume = volume
    broadcast(nil)
    output(source, raw, true, string.format("television %d volume %d", entry.id, volume))
end)

command("media.mute", "media.mute <id> <on|off>", true, function(source, args, raw)
    if args.n ~= 2 then error("wrong argument count", 0) end
    local entry = media[math.floor(number(args[1]))]
    if entry == nil then return output(source, raw, false, "no such television") end
    local state = string.lower(tostring(args[2]))
    if state ~= "on" and state ~= "off" then error("state must be on or off", 0) end
    entry.muted = state == "on"
    broadcast(nil)
    output(source, raw, true, string.format(
        "television %d muted=%s", entry.id, tostring(entry.muted)))
end)

command("media.pause", "media.pause <id> <on|off>", true, function(source, args, raw)
    if args.n ~= 2 then error("wrong argument count", 0) end
    local entry = media[math.floor(number(args[1]))]
    if entry == nil then return output(source, raw, false, "no such television") end
    local state = string.lower(tostring(args[2]))
    if state ~= "on" and state ~= "off" then error("state must be on or off", 0) end
    entry.paused = state == "on"
    broadcast(nil)
    output(source, raw, true, string.format(
        "television %d paused=%s", entry.id, tostring(entry.paused)))
end)

command("media.remove", "media.remove <id>", true, function(source, args, raw)
    if args.n ~= 1 then error("wrong argument count", 0) end
    local id = math.floor(number(args[1]))
    if not remove(id) then return output(source, raw, false, "no such television") end
    broadcast(nil)
    output(source, raw, true, string.format("television %d removed", id))
end)

-- Live quad tuning, and the reason the quad is on the wire at all.
--
-- The catalogue's rectangles are authored estimates: the props hosts publish no
-- screen dimension and the only way to measure one is to look at it in game.
-- This command is what turns that from "edit a file, reload the resource, walk
-- back, look again" into "nudge it and watch". The geometry is echoed back in a
-- form that can be pasted straight into `shared/records.lua` when it is right.
--
-- `media.quad <id> width=<m> height=<m> offx= offy= offz= rightx= righty= rightz=
--             upx= upy= upz=` -- any subset, in any order.
command("media.quad", "media.quad <id> [width=] [height=] [offx= offy= offz=] [upx= upy= upz=]",
    true, function(source, args, raw)
        if args.n < 2 then error("wrong argument count", 0) end
        local entry = media[math.floor(number(args[1]))]
        if entry == nil then return output(source, raw, false, "no such television") end

        local quad = entry.quad
        local applied = 0
        for index = 2, args.n do
            local key, value = string.match(tostring(args[index]), "^([%a_]+)=(.+)$")
            if key == nil then
                return output(source, raw, false,
                    "expected key=value, got " .. tostring(args[index]))
            end
            local parsed = tonumber(value)
            if parsed == nil then
                return output(source, raw, false, "not a number: " .. tostring(value))
            end
            local target, component = nil, nil
            local head, tail = string.match(key, "^(%a+)([xyz])$")
            if head == "off" then target = quad.offset elseif head == "right" then
                target = quad.right elseif head == "up" then target = quad.up end
            if tail == "x" then component = 1 elseif tail == "y" then component = 2
            elseif tail == "z" then component = 3 end
            if target ~= nil and component ~= nil then
                target[component] = parsed
            elseif key == "width" or key == "height" then
                if not (parsed > 0.01 and parsed <= 100.0) then
                    return output(source, raw, false, key .. " must be 0.01..100 metres")
                end
                quad[key] = parsed
            else
                return output(source, raw, false, "unknown knob: " .. tostring(key))
            end
            applied = applied + 1
        end

        if applied == 0 then return output(source, raw, false, "nothing to change") end
        broadcast(nil)
        output(source, raw, true, string.format(
            "television %d quad: width=%.3f height=%.3f\n  offset = { %.3f, %.3f, %.3f }\n" ..
            "  right  = { %.3f, %.3f, %.3f }\n  up     = { %.3f, %.3f, %.3f }",
            entry.id, quad.width, quad.height,
            quad.offset[1], quad.offset[2], quad.offset[3],
            quad.right[1], quad.right[2], quad.right[3],
            quad.up[1], quad.up[2], quad.up[3]))
    end)

-- Puts a screen back on its record's authored rectangle.
command("media.quad.reset", "media.quad.reset <id>", true, function(source, args, raw)
    if args.n ~= 1 then error("wrong argument count", 0) end
    local entry = media[math.floor(number(args[1]))]
    if entry == nil then return output(source, raw, false, "no such television") end
    local record = Open77MediaRecord(entry.record)
    if record == nil then return output(source, raw, false, "record gone") end
    entry.quad = copyQuad(record.quad)
    broadcast(nil)
    output(source, raw, true, string.format("television %d quad reset", entry.id))
end)

command("media.list", "media.list", true, function(source, args, raw)
    local entries = liveEntries()
    if #entries == 0 then return output(source, raw, true, "no televisions") end
    local lines = { string.format("%d television(s):", #entries) }
    for _, entry in ipairs(entries) do lines[#lines + 1] = "  " .. describeEntry(entry) end
    output(source, raw, true, table.concat(lines, "\n"))
end)

-- =============================================================================
-- CLIENT PROTOCOL
-- =============================================================================

-- A client asks for the current set on join. It is not credited with knowing
-- anything: the answer is the same whole-set snapshot everyone else gets.
RegisterNetEvent("open77:media:ready", function()
    local source = source
    if source == nil then return end
    local entries = liveEntries()
    TriggerClientEvent("open77:media:snapshot", source, snapshotFor(entries))
end)

-- The menu's path to every mutation, so the same validation serves the console
-- and the UI without one being able to do something the other cannot.
--
-- Every branch re-reads the entry from `media` rather than trusting anything in
-- the payload except an id and a value: a client can send any table it likes,
-- and the only fields it is allowed to influence are the four below.
RegisterNetEvent("open77:media:control", function(action, payload)
    local source = source
    if source == nil or type(action) ~= "string" or type(payload) ~= "table" then return end
    local entry = media[math.floor(tonumber(payload.id) or -1)]
    if entry == nil then return end

    if action == "spawn" then
        -- Spawning needs a world position and is handled on its own event, so
        -- that a `control` message can never be the thing that creates an
        -- entity. Reaching here with "spawn" means a caller used the wrong
        -- event, and saying so is cheaper than guessing.
        TriggerClientEvent("open77:media:result", source, false, "use_open77_media_spawn")
        return
    end

    if action == "quad" then
        -- Not reachable from the menu and not meant to be: there is no UI for a
        -- screen rectangle and a player nudging one would move a screen other
        -- players are watching. `media.quad` is the operator's path.
        TriggerClientEvent("open77:media:result", source, false, "quad_is_operator_only")
        return
    elseif action == "url" then
        local url, urlError = acceptUrl(payload.url)
        if url == nil then
            TriggerClientEvent("open77:media:result", source, false, tostring(urlError))
            return
        end
        entry.url = url
    elseif action == "title" then
        local label, labelError = acceptText(payload.title, MAX_TITLE_LENGTH, "title")
        if label == nil then
            TriggerClientEvent("open77:media:result", source, false, tostring(labelError))
            return
        end
        entry.label = label
    elseif action == "volume" then
        local volume = math.floor(tonumber(payload.volume) or -1)
        if volume < 0 or volume > 100 then
            TriggerClientEvent("open77:media:result", source, false, "volume_out_of_range")
            return
        end
        entry.volume = volume
    elseif action == "muted" or action == "paused" then
        if type(payload.value) ~= "boolean" then
            TriggerClientEvent("open77:media:result", source, false, "value_must_be_boolean")
            return
        end
        entry[action] = payload.value
    elseif action == "remove" then
        remove(entry.id)
    else
        TriggerClientEvent("open77:media:result", source, false, "unknown_action")
        return
    end

    broadcast(nil)
    TriggerClientEvent("open77:media:result", source, true, describeEntry(media[entry.id] or entry))
end)

-- Spawn from the menu. Kept separate from `control` because it is the only
-- media message that creates a world entity, and it carries its own position
-- instead of naming an existing screen.
RegisterNetEvent("open77:media:spawn", function(payload)
    local source = source
    if source == nil or type(payload) ~= "table" then return end
    local record = tostring(payload.record or "")
    if Open77MediaRecord(record) == nil then
        TriggerClientEvent("open77:media:result", source, false, "unknown_record")
        return
    end

    -- The position is the caller's own, read server-side. A client-supplied
    -- position would let any client put a screen anywhere in the world,
    -- including inside someone else's building, and the menu has no need for
    -- that: it spawns where the player is standing.
    local position = Open77.players.position(source)
    if position == nil then
        TriggerClientEvent("open77:media:result", source, false, "no_position_snapshot")
        return
    end

    local entry, reason = spawn(record, position, payload.yaw, payload.url, source)
    if entry == nil then
        TriggerClientEvent("open77:media:result", source, false, tostring(reason))
        return
    end
    broadcast(nil)
    TriggerClientEvent("open77:media:result", source, true,
        string.format("television %d created", entry.id))
end)

-- The catalogue, for the menu. Served rather than duplicated in the page so a
-- record added here appears in the menu without a second edit.
RegisterNetEvent("open77:media:catalogue", function()
    local source = source
    if source == nil then return end
    local out = {}
    for _, record in ipairs(Open77MediaCatalogue()) do
        out[#out + 1] = {
            id = record.id,
            label = record.label,
            model = record.model,
            blurb = record.blurb,
            width = record.quad.width,
            height = record.quad.height,
        }
    end
    TriggerClientEvent("open77:media:catalogue", source, out)
end)

print("open77_media ready -- " .. tostring(#Open77MediaCatalogue()) .. " television records")
