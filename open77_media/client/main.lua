-- =============================================================================
-- open77_media -- client/main.lua
-- =============================================================================
-- Turns "there is a television at this position, showing this URL" into pixels
-- on that television's screen, for the televisions close enough to matter.
--
-- -----------------------------------------------------------------------------
-- WHY ONLY THE NEAR ONES
-- -----------------------------------------------------------------------------
-- A screen is one CEF surface, and a resource may hold eight
-- (`kMaximumWebSurfaces`). A server may hold sixty-four televisions. So the
-- client must pick, and it is the only party that can: it is the only one that
-- knows where the player is standing.
--
-- The selection runs on a one-second thread. That is deliberate and it is the
-- opposite of the rule this project applies everywhere else -- the rule is that
-- anything whose *smoothness* the player sees must not be on a Lua tick, and
-- materialising a page is not smooth: it is a decision about whether a browser
-- exists, and a second is far below the time it takes to walk into range of a
-- screen. Nothing positional goes through Lua. The screen's rectangle and its
-- screen position are projected natively every game frame by `Api::MediaScreens`;
-- this file only decides which screens exist.
--
-- -----------------------------------------------------------------------------
-- NO TICK FOR ANYTHING ELSE
-- -----------------------------------------------------------------------------
-- A snapshot arrives whole, so binding and unbinding happen when it arrives.
-- There is no polling of server state, no per-frame work, and no accumulation:
-- `screens` is exactly the set the server last sent.

-- A client can receive a newer resource generation before its native plugin has
-- been restarted. Stay inert on that transition rather than failing the VM on
-- every screen.
if type(Open77.media) ~= "table"
    or type(Open77.media.bind) ~= "function"
    or type(Open77.media.unbind) ~= "function" then
    print("[open77_media] native media API unavailable; restart Cyberpunk to activate it")
    return
end

-- How many screens may be materialised at once, and how far away a screen may be
-- to be considered. Six, under the eight-surface ceiling, so the resource never
-- spends its last two surfaces on televisions and has none left for anything
-- else it might want. The radius is generous because a screen is legible long
-- before it is close; the count is what actually binds.
local MAX_MATERIALISED = 6
local MATERIALISE_RADIUS = 90.0

-- Above this, the surface is created at a lower frame rate. A distant screen
-- showing a menu is not worth 30 fps of a compositor this process shares with
-- the game, and the page has no animation that matters at that size.
local NEAR_DISTANCE = 25.0

-- mediaId -> { spec = <server payload>, page = <WebUI page or nil>,
--              native = <screen id string or nil>, materialised = bool }
local screens = {}
local state = { version = 0 }

local function distanceTo(position)
    if type(position) ~= "table" then return nil end
    local character = Open77.character.position()
    if character == nil then return nil end
    local dx = (tonumber(position.x) or 0.0) - (tonumber(character.x) or 0.0)
    local dy = (tonumber(position.y) or 0.0) - (tonumber(character.y) or 0.0)
    local dz = (tonumber(position.z) or 0.0) - (tonumber(character.z) or 0.0)
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

---What the page is told. The page owns no state of its own: everything it shows
---came from the server through here, so two clients watching the same television
---cannot disagree about what is playing.
local function pageState(entry)
    return {
        id = entry.spec.id,
        label = entry.spec.label,
        url = entry.spec.url,
        volume = entry.spec.volume,
        muted = entry.spec.muted,
        paused = entry.spec.paused,
    }
end

local function destroyPage(entry)
    if entry.native ~= nil then
        Open77.media.unbind(entry.native)
        entry.native = nil
    end
    if entry.page ~= nil then
        entry.page:destroy()
        entry.page = nil
    end
    entry.materialised = false
end

---Creates the page and binds it to the television's prop.
---
---Order matters: the page must be created before it is bound, because the
---binding names it, and the binding is by *page handle* rather than by surface
---id so the native side can refuse a screen pointing at another resource's page.
local function materialisePage(entry)
    if entry.materialised then return true end

    local near = entry.distance ~= nil and entry.distance <= NEAR_DISTANCE
    local page, reason = WebUI.create({
        entry = "web/tv.html",
        layer = "hud",
        width = 1280,
        height = 720,
        -- A television does not need the compositor's full rate. The screen is
        -- mapped 1:1 onto the quad and most of what it shows is static.
        fps = near and 30 or 15,
        transparent = false,
        visible = true,
        -- The whole reason a television does not also paint itself across the
        -- player's viewport: `Draw` skips a hud-suppressed surface and
        -- `DrawSurfaceQuad` composites it into the world quad instead.
        hudSuppressed = true,
    })
    if page == nil then
        print(string.format(
            "[open77_media] television %s: WebUI.create failed: %s",
            tostring(entry.spec.id), tostring(reason)))
        return false
    end

    local id, bindError = Open77.media.bind({
        prop = entry.spec.prop,
        page = page,
        label = entry.spec.label,
        quad = entry.spec.quad,
    })
    if id == nil then
        print(string.format(
            "[open77_media] television %s: bind failed: %s",
            tostring(entry.spec.id), tostring(bindError)))
        page:destroy()
        return false
    end

    entry.page = page
    entry.native = id
    entry.materialised = true

    -- The page announces itself once its document is live, and the state is sent
    -- then rather than now: `page:send` before the page has run its script is
    -- delivered to nothing. A second send on every later change is the same
    -- path, so there is one route for state and not two.
    page:on("media:ready", function()
        page:send("media:state", pageState(entry))
    end)

    page:on("media:report", function(payload)
        if type(payload) ~= "table" then return end
        -- Only ever logged. The page's own opinion of what it is doing must
        -- never become state: the server is the only writer of a television's
        -- content, so a page that claims a different URL is reporting a bug.
        print(string.format("[open77_media] television %s: %s%s",
            tostring(entry.spec.id), tostring(payload.status),
            payload.detail ~= nil and (" (" .. tostring(payload.detail) .. ")") or ""))
    end)

    -- The page's URL bar, when someone is driving the television on foot. It goes
    -- to the server like any other change, so the URL policy is applied once, in
    -- one place, and every other client sees the same result.
    page:on("media:url", function(payload)
        if type(payload) ~= "table" or type(payload.url) ~= "string" then return end
        TriggerServerEvent("open77:media:control", "url",
            { id = entry.spec.id, url = payload.url })
    end)

    page:on("media:volume", function(payload)
        if type(payload) ~= "table" then return end
        if type(payload.volume) == "number" then
            TriggerServerEvent("open77:media:control", "volume",
                { id = entry.spec.id, volume = math.floor(payload.volume) })
        elseif type(payload.muted) == "boolean" then
            TriggerServerEvent("open77:media:control", "muted",
                { id = entry.spec.id, value = payload.muted })
        elseif type(payload.paused) == "boolean" then
            TriggerServerEvent("open77:media:control", "paused",
                { id = entry.spec.id, value = payload.paused })
        end
    end)
    return true
end

---Applies a screen's new state to a page that already exists. Cheaper than
---rebinding, and it is the common case: someone changed the URL.
local function refreshPage(entry)
    if not entry.materialised or entry.page == nil then return end
    entry.page:send("media:state", pageState(entry))
end

-- =============================================================================
-- SNAPSHOT
-- =============================================================================

---One whole set, applied as a whole.
---
---Diffs rather than rebuilding: a television that merely changed its URL must not
---lose its page, because destroying and recreating a CEF surface is a visible
---flash and a fresh page load. Only screens that appeared or disappeared change
---their materialisation.
local function applySnapshot(snapshot)
    if type(snapshot) ~= "table" then snapshot = {} end

    local incoming = {}
    for _, spec in ipairs(snapshot) do
        local id = tonumber(spec.id)
        if id ~= nil then incoming[id] = spec end
    end

    for id, entry in pairs(screens) do
        if incoming[id] == nil then
            destroyPage(entry)
            screens[id] = nil
        end
    end

    for id, spec in pairs(incoming) do
        local entry = screens[id]
        if entry == nil then
            screens[id] = { spec = spec, distance = nil, materialised = false }
        else
            local changed = entry.spec.url ~= spec.url or entry.spec.volume ~= spec.volume
                or entry.spec.muted ~= spec.muted or entry.spec.paused ~= spec.paused
                or entry.spec.label ~= spec.label
            entry.spec = spec
            if changed then refreshPage(entry) end
        end
    end

    state.version = state.version + 1
    TriggerEvent("open77:media:changed", state.version)
end

RegisterNetEvent("open77:media:snapshot", applySnapshot)

-- Logged here and nowhere re-emitted. A resource that wants the answer registers
-- the same net event and receives it from the server directly, exactly as this
-- file does -- re-emitting it locally would deliver it twice to anyone listening
-- on both, and "received the result twice" is how a menu ends up showing the
-- same confirmation twice.
RegisterNetEvent("open77:media:result", function(ok, detail)
    print(string.format("[open77_media] %s %s", ok and "OK" or "ERR", tostring(detail)))
end)

-- =============================================================================
-- MATERIALISATION
-- =============================================================================

---Pushes what this client currently has on screen, locally, to any resource that
---asked by registering the event.
---
---A local event rather than an export, because there is no cross-resource export
---call anywhere in this project and inventing one for a status read would be a
---new mechanism for a convenience. It is one-directional on purpose: nothing
---comes back, so there is no request/response pairing to get wrong.
---
---Sent only when the set actually changes, so a menu open for ten minutes is not
---handed a thousand identical tables.
local lastLocalSignature = ""

local function publishLocal()
    local out = {}
    local signature = {}
    for _, entry in pairs(screens) do
        out[#out + 1] = {
            id = entry.spec.id,
            label = entry.spec.label,
            url = entry.spec.url,
            volume = entry.spec.volume,
            muted = entry.spec.muted,
            paused = entry.spec.paused,
            distance = entry.distance,
            materialised = entry.materialised,
        }
        signature[#signature + 1] = string.format("%s:%s:%s:%s:%s:%s",
            tostring(entry.spec.id), tostring(entry.materialised),
            tostring(entry.spec.url), tostring(entry.spec.volume),
            tostring(entry.spec.muted), tostring(entry.spec.paused))
    end
    table.sort(out, function(a, b) return (tonumber(a.id) or 0) < (tonumber(b.id) or 0) end)
    table.sort(signature)

    local joined = table.concat(signature, "|")
    if joined == lastLocalSignature then return end
    lastLocalSignature = joined
    TriggerEvent("open77:media:local", out)
end

---Ranks every screen by distance and materialises at most `MAX_MATERIALISED`.
---
---A television already materialised is not dematerialised for being sixth rather
---than fifth: the hysteresis is the whole reason this is a ranked *selection*
---with a stable tie-break (by id, ascending) rather than a raw distance cut. Two
---screens at 40.1 and 40.2 m would otherwise swap every second, and each swap is
---a page destroyed and a page created.
local function reselect()
    local reachable = {}
    local count = 0
    for _, entry in pairs(screens) do
        entry.distance = distanceTo(entry.spec.position)
        if entry.distance ~= nil and entry.distance <= MATERIALISE_RADIUS then
            count = count + 1
            reachable[#reachable + 1] = entry
        end
    end
    table.sort(reachable, function(a, b)
        if a.materialised ~= b.materialised then return a.materialised end
        if math.abs(a.distance - b.distance) > 0.5 then return a.distance < b.distance end
        return (tonumber(a.spec.id) or 0) < (tonumber(b.spec.id) or 0)
    end)

    -- Already-materialised screens sort first, so they are counted before the
    -- budget is spent and a screen never loses its page to one that merely came
    -- into range this second.
    local kept = 0
    for _, entry in ipairs(reachable) do
        if entry.materialised then
            kept = kept + 1
        elseif kept < MAX_MATERIALISED and materialisePage(entry) then
            kept = kept + 1
        end
    end

    -- Out of range entirely.
    for _, entry in pairs(screens) do
        if entry.materialised and (entry.distance == nil or entry.distance > MATERIALISE_RADIUS) then
            destroyPage(entry)
        end
    end

    publishLocal()
end

-- =============================================================================
-- LIFECYCLE
-- =============================================================================

local function clearAll()
    for id, entry in pairs(screens) do
        destroyPage(entry)
        screens[id] = nil
    end
    -- Belt and braces, as the props resource does it: an entry orphaned by a
    -- mismatch between this file's bookkeeping and the native registry would
    -- otherwise survive a resource restart as a picture on a wall.
    Open77.media.clear()
end

AddEventHandler("onClientResourceStart", function(name)
    if name ~= GetCurrentResourceName() then return end
    clearAll()
    lastLocalSignature = ""
    TriggerServerEvent("open77:media:ready")

    -- One thread, one second, one job: which screens exist. See the file header
    -- for why this is not a per-frame concern.
    CreateThread(function()
        while true do
            local ok, err = pcall(reselect)
            if not ok then
                print("[open77_media] materialisation failed: " .. tostring(err))
            end
            Wait(1000)
        end
    end)
end)

AddEventHandler("onClientResourceStop", function(name)
    if name ~= GetCurrentResourceName() then return end
    clearAll()
end)

-- =============================================================================
-- SURFACE FOR GAMEPLAY RESOURCES
-- =============================================================================
-- The menu asks this resource rather than talking to the server itself, so the
-- catalogue and the spawn path are reachable through one audited doorway.

-- No exports, and that is a decision rather than an omission. There is no
-- cross-resource export CALL anywhere in this project -- `exports` registers, and
-- nothing invokes another resource's registered function -- so a menu that wanted
-- to drive this feature through an export would be inventing the mechanism first.
-- It does not have to: every mutation is a server event (`open77:media:spawn`,
-- `open77:media:control`) that any resource may send and this one validates, and
-- the current set is pushed locally as `open77:media:local` below. Gameplay
-- resources then talk to the same audited surface a console operator does.

-- =============================================================================
-- WHAT THIS CLIENT HAS ON SCREEN, PUSHED LOCALLY
-- =============================================================================
-- `open77:media:local` carries the sets this client is rendering, with their
-- distance and their materialisation state. Local and deliberately so: it
-- describes THIS client, not the server's set, and a menu that showed the
-- server's set here would claim six screens on a server holding sixty.

print("open77_media client ready")
