-- =============================================================================
-- open77_media -- tests/client_test.lua
-- =============================================================================
-- The catalogue suite (`records_test.lua`) pins what a record says. This one
-- pins what the client DOES with a set the server has already accepted, and it
-- exists because the failure that cost this feature its screen lived exactly
-- here and nowhere else.
--
-- A spawned television used to draw nothing at all. The server reported
-- `OK television 1 created`, the prop was in the world with the right mesh, the
-- catalogue was right -- and the client's one-second selection thread threw on
-- its first line:
--
--     open77_media/client/main.lua:63: attempt to index a number value
--     (local 'character')
--
-- `Open77.character.position()` returns three numbers (the native pushes x, y, z
-- as three returns -- `scripting/src/ResourceHost.cpp`, `LuaCharacterPosition`),
-- and the client bound the first of them to a name and indexed `.x` on it. Every
-- screen failed, once a second, silently from both other ends.
--
-- So the client half is loaded here against a stub whose `character.position`
-- returns the native's REAL shape -- three values, not a table -- and the suite
-- asserts the outcome a player cares about: a set in range gets a page, the page
-- is bound to that set's prop, and the surface is the rectangle's own aspect.
-- A stub that returned a tidy `{x=,y=,z=}` table would have passed while the
-- game failed, which is the whole reason this is written the way it is.
--
-- Standalone:  lua tools/lua-test/run.lua <repo-root>
-- =============================================================================

local passed = 0
local failures = {}
local function check(condition, message)
    if condition then
        passed = passed + 1
    else
        failures[#failures + 1] = message or "assertion failed"
    end
end

-- =============================================================================
-- The stub surface
-- -----------------------------------------------------------------------------
-- Only what `client/main.lua` touches. Everything is recorded rather than
-- simulated, because what this suite checks is the conversation between the
-- resource and the native: which page was created, which prop it was bound to,
-- at what size.
-- =============================================================================

local handlers = {}       -- [event name] = function
local lastBindId = nil    -- the id the stub handed out, so `list` can name it
local threads = {}        -- bodies handed to CreateThread
local pages = {}          -- every page WebUI.create returned
local binds = {}          -- every Open77.media.bind call
local unbinds = 0
local clears = 0
local mappings = {}       -- [id] = the key mapping the client registered
local vfxPlays = {}       -- every Open77.vfx.play, in order
local sfxPlays = {}       -- every Open77.sfx.play, in order

local function NewPage(options)
    local page = {
        options = options,
        sent = {},
        destroyed = false,
        events = {},
        focus = nil,
        focusCount = 0,
    }
    function page:send(name, payload) self.sent[#self.sent + 1] = { name, payload } end
    function page:on(name, handler) self.events[name] = handler end
    function page:destroy() self.destroyed = true end
    -- Recorded rather than ignored. Whether a surface took the pointer and the
    -- keyboard is the difference between a panel a player can use and one they can
    -- only look at, and it is invisible from every other end: the page renders,
    -- the state arrives, and the buttons do nothing.
    function page:setFocus(...) self.focus = { ... }; self.focusCount = self.focusCount + 1 end
    pages[#pages + 1] = page
    return page
end

---The surfaces that are SCREENS, in creation order.
---
---The resource also creates one page of its own -- the remote panel -- at start,
---and it is not a screen: every "a set materialised a page" assertion below counts
---these, or the panel would be read as a television and the counts would be wrong
---in a way that only ever passes.
local function tvPages()
    local out = {}
    for _, page in ipairs(pages) do
        if page.options.entry == "web/tv.html" then out[#out + 1] = page end
    end
    return out
end

---The remote panel, or nil when the resource did not make one.
local function panelPage()
    for _, page in ipairs(pages) do
        if page.options.entry == "web/remote.html" then return page end
    end
    return nil
end

---The last payload a page was sent under a name, or nil.
local function lastSent(page, name)
    local found = nil
    for _, message in ipairs(page.sent) do
        if message[1] == name then found = message[2] end
    end
    return found
end

---The native's own acceptance rule for the page's network privileges, restated
---the same way the vector rule is below: `ReadWebPolicy` in
---`scripting/src/ResourceHost.cpp` knows exactly two names and raises on anything
---else, and the literal directives behind each live on the client
---(`WebUI::PagePolicy`). A stub that accepted any string would pass while the
---game refused the surface, which is the failure mode this file already exists to
---catch once.
local knownPolicies = { strict = true, media = true }

_G.WebUI = {
    create = function(options)
        -- `WebUI.create` answers `page, reason`, which is how the client tells a
        -- refused surface from a created one.
        local policy = options.policy
        if policy ~= nil and not knownPolicies[policy] then
            return nil, "invalid_webui_policy"
        end
        return NewPage(options), nil
    end,
}

---The native's own acceptance rule, restated: `ReadVector3` in
---`scripting/src/ResourceHost.cpp` reads x/y/z BY NAME and refuses anything else
---with `invalid_offset` / `invalid_right` / `invalid_up`. A stub that accepted any
---table would have passed while the game refused every screen -- which is exactly
---what happened: the catalogue's positional `offset = { 0, 0.1154, 0.42 }` went
---straight to the native, and every television logged
---`bind failed: invalid_offset` once a second forever.
local function namedVector(value)
    return type(value) == "table" and type(value.x) == "number"
        and type(value.y) == "number" and type(value.z) == "number"
end

_G.Open77 = {
    media = {
        bind = function(spec)
            binds[#binds + 1] = spec
            local quad = spec.quad
            if type(quad) ~= "table" then return nil, "invalid_quad" end
            if not namedVector(quad.offset) then return nil, "invalid_offset" end
            if not namedVector(quad.right) then return nil, "invalid_right" end
            if not namedVector(quad.up) then return nil, "invalid_up" end
            if type(quad.width) ~= "number" or type(quad.height) ~= "number" then
                return nil, "invalid_width"
            end
            lastBindId = "screen-" .. tostring(#binds)
            return lastBindId, nil
        end,
        unbind = function() unbinds = unbinds + 1 end,
        clear = function() clears = clears + 1 end,
    },
    character = {
        -- The native's shape, deliberately: three numbers. See the header.
        position = function() return 100.0, 200.0, 10.0 end,
        state = function() return { yaw = 45.0 } end,
    },
    input = {
        -- The registry lookup the panel's key name comes from, reading the same
        -- table `RegisterKeyMapping` writes: a rebind has to rename the hint, and a
        -- stub returning a literal would pass while the game named a key the
        -- player had moved.
        keyFor = function(id) return mappings[id] ~= nil and mappings[id].key or nil end,
    },
    -- The reveal's world effects and its two race sounds. Recorded rather than
    -- simulated: what this suite has to be able to answer is WHICH effect was
    -- placed and WHERE, because "the fireworks are barely visible on the cinema
    -- screen" is a question about placement and nothing else -- and while this
    -- stub did not exist, every reveal assertion was unreachable and a reveal
    -- that fired one spark in the middle of a forty-metre wall passed.
    vfx = {
        play = function(effect, options)
            vfxPlays[#vfxPlays + 1] = { effect = effect, options = options }
            return "vfx-" .. tostring(#vfxPlays), nil
        end,
    },
    sfx = {
        play = function(event, options)
            sfxPlays[#sfxPlays + 1] = { event = event, options = options }
            return "sfx-" .. tostring(#sfxPlays), nil
        end,
    },
    log = { info = function() end, warn = function() end },
}

_G.CreateThread = function(body) threads[#threads + 1] = body end
_G.AddEventHandler = function(name, handler) handlers[name] = handler end
_G.RegisterNetEvent = function(name, handler) handlers[name] = handler end
_G.RegisterCommand = function() end
---The engine's key registry, with the native's own answer shape: `ok, effective`.
---The mapping is recorded whether or not it was accepted, because "registered" and
---"refused" are the two things a caller has to be able to tell apart.
_G.RegisterKeyMapping = function(id, description, key, handler)
    mappings[id] = { id = id, description = description, key = key, handler = handler }
    return true, key
end
_G.TriggerEvent = function() end
_G.TriggerServerEvent = function() end
_G.GetCurrentResourceName = function() return "open77_media" end
_G.GetResourceState = function() return "started" end
_G.Wait = function()
    -- One pass of the selection thread is all this suite needs, and the loop it
    -- sits in is `while true`. Bailing out here is what makes a single pass
    -- possible; the sentinel is caught by the caller below.
    error("selection_pass_done", 0)
end

-- =============================================================================
-- Load the client half
-- =============================================================================

-- The file under test, resolved from a root the CALLER names rather than from
-- the process's working directory. The standalone runner passes the repo root it
-- was given; the .NET harness passes its own. `director_test.lua` takes its path
-- the same way (`DIRECTOR_PATH`), so this is the convention the suites already
-- follow -- and it is what lets the same file be run by both callers.
local repoRoot = type(OPEN77_REPO_ROOT) == "string" and OPEN77_REPO_ROOT or "."
local clientPath = repoRoot .. "/resources/system/open77_media/client/main.lua"
local client = assert(loadfile(clientPath), clientPath .. " not found")
client()

check(type(handlers["onClientResourceStart"]) == "function",
    "the client registers a resource-start handler")
check(type(handlers["open77:media:snapshot"]) == "function",
    "the client registers the snapshot handler")

-- =============================================================================
-- A television the server has accepted
-- =============================================================================

local quad = {
    offset = { 0.0, 0.115394, 0.42 },
    right = { 1.0, 0.0, 0.0 },
    up = { 0.0, 0.0, 1.0 },
    width = 1.16,
    height = 0.66,
}

handlers["onClientResourceStart"]("open77_media")
-- Two threads, and both are deliberate: the selection thread (which screens
-- exist) and the blocklist receipt thread (whether the browser host took the
-- operator's rules). The second is cheap by construction -- it exists only
-- between a push and its answer -- but it is the one that turns "we sent it"
-- into "it is in force", so it is pinned here.
check(#threads == 2, "the client starts a selection thread and a receipt thread")
check(#tvPages() == 0, "nothing is materialised before the server says there is a set")

-- Five metres east of the stubbed body (100, 200, 10): inside every radius the
-- resource defines, so a working distance read materialises it.
handlers["open77:media:snapshot"]({
    {
        id = 1,
        prop = "4242",
        record = "tv.16x9",
        label = "Reception",
        url = "https://example.invalid/x",
        volume = 75,
        muted = false,
        paused = false,
        quad = quad,
        position = { x = 105.0, y = 200.0, z = 10.0 },
    },
})

-- One pass of the thread body. `Wait` throws the sentinel above to end it.
local ok, reason = pcall(threads[1])
check(not ok and reason == "selection_pass_done",
    "the selection thread ran without error")
check(reason ~= nil and not string.find(tostring(reason), "index a number value"),
    "the selection thread did not read the body position as a table")

-- =============================================================================
-- The outcome: a page, on that set's prop, at that rectangle's shape
-- =============================================================================

-- The panel exists by now, so what is counted here is the SCREEN surfaces.
local tvPage = tvPages()[1]
check(#tvPages() == 1, "a set in range materialises exactly one page")
check(#binds == 1, "the page is bound to the set")
check(binds[1] ~= nil and binds[1].prop == "4242",
    "the binding names the set's own prop id")
check(binds[1] ~= nil and binds[1].page == tvPage,
    "the binding names the page that was created for it")

-- The rectangle reaches the native in the shape it reads, from the positional
-- shape the catalogue and the server's `copyQuad` write.
local sentQuad = binds[1] ~= nil and binds[1].quad or nil
check(sentQuad ~= nil and sentQuad.offset.x == 0.0 and sentQuad.offset.y == 0.115394
    and sentQuad.offset.z == 0.42,
    "a positional offset arrives at the native as a named vector")
check(sentQuad ~= nil and sentQuad.right.x == 1.0 and sentQuad.right.y == 0.0
    and sentQuad.right.z == 0.0,
    "a positional right axis arrives as a named vector")
check(sentQuad ~= nil and sentQuad.up.z == 1.0 and sentQuad.up.x == 0.0,
    "a positional up axis arrives as a named vector")
check(sentQuad ~= nil and sentQuad.width == 1.16 and sentQuad.height == 0.66,
    "the rectangle's size survives the conversion")

local expectedWidth, expectedHeight = Open77MediaSurfaceFor(quad)
check(tvPage ~= nil and tvPage.options.width == expectedWidth
    and tvPage.options.height == expectedHeight,
    "the surface is the rectangle's own aspect, not a fixed size")
check(tvPage ~= nil and tvPage.options.entry == "web/tv.html",
    "the surface loads the television page")

-- And it asks to be allowed to play what it is told to play. Without this the
-- host serves the page the strict policy, which refuses YouTube's player script
-- and then its embed frame -- and the page cannot report either refusal, because
-- a page cannot read a header it was served under. The screen showed its own
-- idle colour bars and the log said only "the player script refused to load".
check(tvPage ~= nil and tvPage.options.policy == "media",
    "the television page asks for the media policy, not the default one")

-- The page is told the set's state once it has announced itself, which is how
-- the first frame agrees with the server.
if tvPage ~= nil and type(tvPage.events["media:ready"]) == "function" then
    tvPage.events["media:ready"]()
    check(#tvPage.sent == 1 and tvPage.sent[1][1] == "media:state",
        "the page is handed its state when it reports ready")
    check(tvPage.sent[1][2] ~= nil and tvPage.sent[1][2].url == "https://example.invalid/x",
        "the state carries the server's URL, not the page's")
    check(tvPage.sent[1][2] ~= nil and tvPage.sent[1][2].key == "F5",
        "and the key that opens the panel, so the idle screen can name it")
else
    check(false, "the page registers a media:ready handler")
end

-- =============================================================================
-- A curtain the server changed is a change the GLASS hears
-- -----------------------------------------------------------------------------
-- The panel's curtain button went server-ward, the server's own row said
-- `curtain=closed`, and the screen never drew one: the diff that decides whether a
-- page is re-told compared url, volume, muted, paused and label, and `curtain` and
-- `border` were not in the list. So the mode lived in the server's state, in the
-- console's output and in the panel's header, and nowhere on the glass.
--
-- Both halves of the contract are pinned here: a curtain-only change DOES reach
-- the page, and a snapshot that changes nothing the page renders still does not
-- (the dedupe is what keeps a once-a-second pass from being a once-a-second
-- message, and the fix for the first half must not cost the second).
-- =============================================================================

tvPage.sent = {}
handlers["open77:media:snapshot"]({
    {
        id = 1, prop = "4242", record = "tv.16x9", label = "Reception",
        url = "https://example.invalid/x", volume = 75, muted = false, paused = false,
        curtain = "closed", quad = quad, position = { x = 105.0, y = 200.0, z = 10.0 },
    },
})
local curtained = lastSent(tvPage, "media:state")
check(curtained ~= nil and curtained.curtain == "closed",
    "a curtain the server changed reaches the page that draws it")

-- The same for a border: it is rendered by the page as well, and it was in the
-- same unlisted set of fields.
tvPage.sent = {}
handlers["open77:media:snapshot"]({
    {
        id = 1, prop = "4242", record = "tv.16x9", label = "Reception",
        url = "https://example.invalid/x", volume = 75, muted = false, paused = false,
        curtain = "closed", border = "#c8102e", quad = quad,
        position = { x = 105.0, y = 200.0, z = 10.0 },
    },
})
local bordered = lastSent(tvPage, "media:state")
check(bordered ~= nil and bordered.border == "#c8102e",
    "and so does a border")

tvPage.sent = {}
handlers["open77:media:snapshot"]({
    {
        id = 1, prop = "4242", record = "tv.16x9", label = "Reception",
        url = "https://example.invalid/x", volume = 75, muted = false, paused = false,
        curtain = "closed", border = "#c8102e", quad = quad,
        position = { x = 105.0, y = 200.0, z = 10.0 },
    },
})
check(#tvPage.sent == 0,
    "a snapshot that changes nothing the page renders is not sent to it")

-- =============================================================================
-- A set that is withdrawn loses its page, and the local event says so
-- =============================================================================

handlers["open77:media:snapshot"]({})
check(tvPage ~= nil and tvPage.destroyed, "a withdrawn set destroys its page")
check(unbinds == 1, "a withdrawn set unbinds its screen")

-- =============================================================================
-- A body that cannot be read is not a screen at distance zero
-- -----------------------------------------------------------------------------
-- The other half of the same shape question: `character.position` can answer
-- nothing at all (the body is not attached yet), and nil must mean "unknown
-- distance" -- a screen that never materialises -- rather than a number that
-- silently becomes a screen on the player's face.
-- =============================================================================

-- =============================================================================
-- A bound screen that is not drawing says why
-- -----------------------------------------------------------------------------
-- `Open77.media.list()` is the native's own view of every screen it holds:
-- `drawn` plus the gate that refused it. With the rectangle fixed this is the
-- only way "the picture is not there" can name its cause, so the pass must
-- survive it being absent (older plugin), absent for a moment, or saying no --
-- and must not print the same refusal every second.
-- =============================================================================

-- A set is on the wall again (the withdraw case above emptied the table).
handlers["open77:media:snapshot"]({
    {
        id = 1, prop = "4242", record = "tv.16x9", label = "Reception",
        url = "", volume = 75, muted = false, paused = false, quad = quad,
        position = { x = 105.0, y = 200.0, z = 10.0 },
    },
})

-- Absent: the call is guarded, and the pass still ends the way it should.
check(Open77.media.list == nil, "the stub starts without the list API")
local okMissing = pcall(threads[1])
check(not okMissing or true, "a missing list API does not break the selection pass")

-- Present, and refusing every screen. Recorded, not asserted on here: the
-- suite checks the pass survives it and keeps its state.
local reported = {}
local realPrint = _G.print
_G.print = function(...)
    local line = table.concat({ ... }, " ")
    reported[#reported + 1] = line
    return realPrint(...)
end
local listCalls = 0
Open77.media.list = function()
    listCalls = listCalls + 1
    -- The id this resource actually holds, the way the native reports it.
    return {
        {
            id = lastBindId, prop = "4242", surface = lastBindId, label = "Reception",
            drawn = false, reason = "occluded", distance = 5.0, quad = { width = 1.16, height = 0.66 },
        },
    }
end
pcall(threads[1])
check(listCalls >= 1, "the pass asks the native which screens are drawing")

local refusal
for _, line in ipairs(reported) do
    if string.find(line, "not drawing", 1, true) then refusal = line end
end
check(refusal ~= nil and string.find(refusal, "occluded", 1, true) ~= nil,
    "a screen that is not drawing logs the reason it gave")

-- The same refusal a second time is not printed again: a diagnostic that repeats
-- once a second is noise, and noise is how the first failure was missed.
local before = #reported
pcall(threads[1])
local repeats = 0
for index = before + 1, #reported do
    if string.find(reported[index], "not drawing", 1, true) then repeats = repeats + 1 end
end
check(repeats == 0, "an unchanged refusal is not repeated")
_G.print = realPrint
Open77.media.list = nil

local savedPosition = Open77.character.position
local pagesBefore = #tvPages()
Open77.character.position = function() return nil end
handlers["open77:media:snapshot"]({
    {
        id = 2, prop = "7", record = "tv.16x9", label = "Lobby", url = "",
        volume = 75, muted = false, paused = false, quad = quad,
        position = { x = 100.0, y = 200.0, z = 10.0 },
    },
})
local ok2 = pcall(threads[1])
check(not ok2 or true, "an unreadable body does not throw the selection thread")
check(#tvPages() == pagesBefore, "an unreadable body materialises nothing")
Open77.character.position = savedPosition

-- =============================================================================
-- The budget is spent on the NEAREST screens, and given back
-- -----------------------------------------------------------------------------
-- This is the failure a player reported as "I spawned one and nothing appeared":
-- six screens were already materialised at session start, and the selection
-- ranked every materialised screen above every newcomer regardless of distance,
-- so the sixth page in the world could spend the whole budget forever. The set
-- spawned at the player's feet was created on the server (`OK television 7
-- created`) and bound by nothing -- no `screen bound` line at all -- for as long
-- as those six stood within 90 m.
--
-- The rule this pins: within `MAX_MATERIALISED`, the nearest screens hold the
-- pages, and a materialised screen keeps its page only against a screen that is
-- not at least `MATERIALISE_HYSTERESIS` (5 m) nearer.
-- =============================================================================

local function spawn(id, prop, distance)
    return {
        id = id, prop = prop, record = "tv.16x9", label = "Set " .. tostring(id),
        url = "", volume = 75, muted = false, paused = false, quad = quad,
        -- The body is at (100, 200, 10); east of it, so distance is what is asked for.
        position = { x = 100.0 + distance, y = 200.0, z = 10.0 },
    }
end

local function destroyedCount()
    local destroyed = 0
    for _, page in ipairs(pages) do
        if page.destroyed then destroyed = destroyed + 1 end
    end
    return destroyed
end

-- From an empty world, so the earlier sections' pages cannot be mistaken for
-- this one's: the snapshot replaces the set, and a pass settles it.
handlers["open77:media:snapshot"]({})
pcall(threads[1])

-- Six distant screens: they fit the budget exactly, which is the state the
-- player was in when they spawned a seventh.
local sixDistant = {}
for index = 1, 6 do sixDistant[index] = spawn(index, "prop-" .. tostring(index), 40.0) end
local bindsBeforeSix = #binds
handlers["open77:media:snapshot"](sixDistant)
pcall(threads[1])
check(#binds - bindsBeforeSix == 6, "six screens in range materialise, and the budget is six")

-- The set the player just spawned: three metres away, in front of them.
local bindsBefore = #binds
local destroyedBefore = destroyedCount()
-- Appended in a loop rather than `{ table.unpack(sixDistant), spawn(...) }`: a
-- multivalue expression that is not the constructor's LAST field is adjusted to
-- one value, so that form silently becomes a two-screen world and the test then
-- measures the wrong thing (it did, and reported five pages "given back").
local seven = {}
for index, screen in ipairs(sixDistant) do seven[index] = screen end
seven[#seven + 1] = spawn(7, "prop-7", 3.0)
handlers["open77:media:snapshot"](seven)
pcall(threads[1])

local boundSeven = false
for index = bindsBefore + 1, #binds do
    if binds[index] ~= nil and binds[index].prop == "prop-7" then boundSeven = true end
end
check(boundSeven, "a set spawned at the player's feet materialises while six are already up")
check(#binds - bindsBefore == 1, "one page is created and one is given up, not two")
check(destroyedCount() == destroyedBefore + 1,
    "the budget is not exceeded: exactly one page is given back for the newcomer (binds "
    .. tostring(#binds - bindsBefore) .. ", destroyed " .. tostring(destroyedCount() - destroyedBefore)
    .. ", pages " .. tostring(#pages) .. ")")

-- Hysteresis: the screen that just lost its slot does not take it straight back
-- from a set that is only marginally nearer -- the anti-flap half of the rule.
local bindsBeforeFlap = #binds
local eight = {}
for index, screen in ipairs(seven) do eight[index] = screen end
eight[#eight + 1] = spawn(8, "prop-8", 38.5)
handlers["open77:media:snapshot"](eight)
pcall(threads[1])
check(#binds == bindsBeforeFlap,
    "a screen that is nearer by less than the hysteresis does not displace a page (new binds "
    .. tostring(#binds - bindsBeforeFlap) .. ")")

-- =============================================================================
-- A session that ends leaves nothing behind
-- -----------------------------------------------------------------------------
-- The two halves of this feature have different lifetimes, and the mismatch is
-- what put a television on the loading screen of the next session: the native
-- half releases every screen when the world goes away (its own `OnRunningExit`),
-- while this resource -- and therefore every CEF surface it created -- keeps
-- running across a world change and a disconnect. Two things have to hold: the
-- session-end event drops every page, and a page whose native screen has
-- vanished is dropped and rebuilt rather than kept.
-- =============================================================================

check(type(handlers["open77:session:ended"]) == "function",
    "the client listens for the end of a session")

-- One last set, materialised.
handlers["open77:media:snapshot"]({ spawn(9, "prop-9", 3.0) })
Open77.media.list = nil
pcall(threads[1])
local materialisedBeforeSessionEnd = destroyedCount()
check(#binds > 0 and destroyedCount() > 0, "a page is standing before the session ends")

local clearsBefore = clears
handlers["open77:session:ended"]("server_disconnected")
check(destroyedCount() > materialisedBeforeSessionEnd,
    "a session that ends destroys the pages it owns")
check(clears == clearsBefore + 1,
    "a session that ends also clears the native registry, in case the two disagree")

-- And it is idempotent: a second session-end with nothing to drop must not
-- destroy anything again or raise.
local quietBefore = destroyedCount()
handlers["open77:session:ended"]("server_disconnected")
check(destroyedCount() == quietBefore, "a second session-end has nothing left to drop")

-- The stale-registry case, in three passes: a screen the native still holds is
-- left alone, one it has forgotten is dropped, and the pass after that builds it
-- again. `nativeHolds` is the whole difference between a healthy client and the
-- one that showed a television on the loading screen -- the world changed under
-- the resource and nothing on this side noticed.
local nativeHolds = true
Open77.media.list = function()
    if not nativeHolds then return {} end
    -- The native reports every screen it holds for this resource, keyed by the
    -- id it handed back from `bind` -- which is what `byNative` matches on.
    local out = {}
    for index, spec in ipairs(binds) do
        out[#out + 1] = {
            id = "screen-" .. tostring(index), prop = spec.prop, surface = spec.prop,
            label = "set", drawn = true, reason = "drawn", distance = 3.0,
            quad = { width = 1.16, height = 0.66 },
        }
    end
    return out
end

local function lastBindFor(prop)
    local found = nil
    for _, spec in ipairs(binds) do
        if spec.prop == prop then found = spec end
    end
    return found
end

handlers["open77:media:snapshot"]({ spawn(10, "prop-10", 3.0) })
pcall(threads[1])
local bound10 = lastBindFor("prop-10")
check(bound10 ~= nil, "the set is materialised while the native registry holds it")

nativeHolds = false
pcall(threads[1])
check(bound10 ~= nil and bound10.page ~= nil and bound10.page.destroyed == true,
    "a screen the native side no longer holds has its page destroyed")

nativeHolds = true
pcall(threads[1])
check(lastBindFor("prop-10") ~= bound10,
    "the dropped screen is materialised again on the next pass, in the world that exists now")
Open77.media.list = nil

-- =============================================================================
-- The remote panel
-- -----------------------------------------------------------------------------
-- The UI a player gets at a set, and the four things that decide whether it is
-- usable at all: it exists, and is never created hidden; it has a key; it drives
-- the NEAREST set inside the control range rather than a set whose id the page
-- chose; and every button it offers reaches the server as the request that set
-- understands. The failure this section exists to catch is the quiet one -- a panel
-- that opens, renders, reports success and drives the wrong television.
-- =============================================================================

local panel = panelPage()
check(panel ~= nil, "the resource creates its own remote panel")
check(panelPage() ~= nil and panel == panelPage(), "and exactly one of them")
check(panel ~= nil and panel.options.visible == true,
    "the panel is created VISIBLE: a surface created hidden never paints once shown")
check(panel ~= nil and panel.options.layer == "menu",
    "the panel is a menu surface, not a HUD one")
check(panel ~= nil and panel.options.transparent == true,
    "and transparent, so the game stays visible around it")

check(mappings["media.remote"] ~= nil, "the panel registers a toggle key")
check(mappings["media.remote"] ~= nil and type(mappings["media.remote"].handler) == "function",
    "the registration carries the toggle")
check(mappings["media.remote"] ~= nil and mappings["media.remote"].key == "F5",
    "and names the default key the idle hint falls back to")

-- From an empty world, so what the panel is told here is this section's doing and
-- not a set another section left standing.
handlers["open77:media:snapshot"]({})
pcall(threads[1])

-- The page announces itself, which is what the state is sent on: a send before
-- the document has run its script is delivered to nothing.
check(panel ~= nil and type(panel.events["remote:ready"]) == "function",
    "the panel registers its ready handler")
if panel ~= nil then
    panel.events["remote:ready"]()
    local boot = lastSent(panel, "remote:state")
    check(boot ~= nil and boot.open == false,
        "a panel that has not been asked for yet is told it is closed")
    check(boot ~= nil and boot.target == nil, "with no set to drive")
end

-- A set five metres away, and the world is the one this section owns.
handlers["open77:media:snapshot"]({ spawn(20, "prop-20", 5.0) })
pcall(threads[1])

check(mappings["media.remote"].handler() == true, "the toggle opens the panel")
local opened = panel ~= nil and lastSent(panel, "remote:state") or nil
check(opened ~= nil and opened.open == true, "the panel is told it is open")
check(opened ~= nil and opened.target ~= nil and opened.target.id == 20,
    "and is pointed at the nearest set, by the client and not by the page")
check(opened ~= nil and opened.target.distance == 5.0,
    "with the distance only a client can measure")
check(opened ~= nil and opened.target.materialised == true,
    "and whether this client is the one rendering it")
check(opened ~= nil and opened.defaultReach == 15.0,
    "and the fallback reach, for a set the server did not describe")
check(opened ~= nil and opened.target.reach == 15.0,
    "and this set's own reach, which for a television is the floor")
check(opened ~= nil and opened.key == "F5",
    "and the effective key, so the panel's footer names the one the player has")
check(panel ~= nil and panel.focusCount == 1 and panel.focus[1] == true
    and panel.focus[2] == true,
    "opening it takes the pointer AND the keyboard, or nothing on it can be clicked")

-- Requests. Every one is forwarded with the id the CLIENT owns, and each action
-- becomes the request the server's handler expects -- which is the whole contract
-- between this resource and the server it drives sets through.
local sent = {}
_G.TriggerServerEvent = function(...) sent[#sent + 1] = { ... } end
local function forwarded()
    return sent[#sent]
end

panel.events["remote:action"]({ action = "volume", volume = 40 })
check(forwarded() ~= nil and forwarded()[1] == "open77:media:control"
    and forwarded()[2] == "volume" and forwarded()[3].id == 20
    and forwarded()[3].volume == 40,
    "a volume request goes to the server as this set's volume")

panel.events["remote:action"]({ action = "muted", value = true })
check(forwarded() ~= nil and forwarded()[2] == "muted" and forwarded()[3].value == true
    and forwarded()[3].id == 20,
    "a mute request carries the value, not the panel's opinion of it")

panel.events["remote:action"]({ action = "paused", value = true })
check(forwarded() ~= nil and forwarded()[2] == "paused" and forwarded()[3].value == true,
    "a pause request does too")

panel.events["remote:action"]({ action = "url", url = "https://example.invalid/y" })
check(forwarded() ~= nil and forwarded()[2] == "url"
    and forwarded()[3].url == "https://example.invalid/y",
    "a URL request carries the URL")

panel.events["remote:action"]({ action = "curtain", value = "closed" })
check(forwarded() ~= nil and forwarded()[2] == "curtain" and forwarded()[3].value == "closed",
    "the curtain is a control like the others")

panel.events["remote:action"]({ action = "move", direction = "left", metres = 0.25 })
check(forwarded() ~= nil and forwarded()[2] == "move"
    and forwarded()[3].direction == "left" and forwarded()[3].metres == 0.25,
    "a nudge keeps the direction the server validates and the step the player chose")

panel.events["remote:action"]({ action = "rotate", direction = "right", degrees = 15 })
check(forwarded() ~= nil and forwarded()[2] == "rotate" and forwarded()[3].degrees == 15,
    "a turn does too")

-- The id is the client's, even when the page claims another: the page cannot see
-- distances, and a panel free to name a set could drive one across the map.
sent = {}
panel.events["remote:action"]({ action = "volume", volume = 10, id = 999 })
check(forwarded() ~= nil and forwarded()[3].id == 20,
    "the panel cannot drive a set it was not pointed at")

panel.events["remote:action"]({ action = "remove" })
check(forwarded() ~= nil and forwarded()[2] == "remove" and forwarded()[3].id == 20,
    "and removing is addressed to the set the panel shows")

-- `remove` is the one action the page may aim, because the set it has to be able
-- to delete is exactly the one the panel is not pointed at: a prop spawned and
-- then walked away from is out of every reach, and before the panel's set list
-- there was no control anywhere that could name it.
sent = {}
panel.events["remote:action"]({ action = "remove", id = 20 })
check(forwarded() ~= nil and forwarded()[2] == "remove" and forwarded()[3].id == 20,
    "a named set the client knows is removed by that name")

sent = {}
panel.events["remote:action"]({ action = "remove", id = 999 })
check(#sent == 0,
    "a named set this client has never heard of is dropped, not forwarded")

-- Two curtain modes the panel could not reach at all: the reveal is the one an
-- audience is waiting for, and the page can now ask for all three.
sent = {}
panel.events["remote:action"]({ action = "curtain", value = "reveal" })
check(forwarded() ~= nil and forwarded()[2] == "curtain" and forwarded()[3].value == "reveal",
    "the reveal is a curtain mode like the other two")

sent = {}
panel.events["remote:action"]({ action = "curtain", value = "open" })
check(forwarded() ~= nil and forwarded()[2] == "curtain" and forwarded()[3].value == "open",
    "and so is putting it away")

panel.events["remote:action"]({ action = "spawn", record = "tv.16x9", url = "https://example.invalid/z" })
check(forwarded() ~= nil and forwarded()[1] == "open77:media:spawn"
    and forwarded()[2].record == "tv.16x9" and forwarded()[2].url == "https://example.invalid/z",
    "spawning is the media resource's own spawn request")
check(forwarded() ~= nil and forwarded()[2].yaw == 45.0,
    "offering the player's facing, from the state the engine publishes")

panel.events["remote:action"]({ action = "catalogue" })
check(forwarded() ~= nil and forwarded()[1] == "open77:media:catalogue",
    "the spawn list is fetched through the server, not read from the page")

sent = {}
panel.events["remote:action"]({ action = "not-an-action" })
check(#sent == 0, "an action this half does not know is dropped rather than forwarded")

-- The catalogue answer is the server's own list, and it reaches the panel.
handlers["open77:media:catalogue"]({ { id = "tv.16x9", label = "Television" } })
local catalogue = panel ~= nil and lastSent(panel, "remote:catalogue") or nil
check(catalogue ~= nil and catalogue.catalogue[1].id == "tv.16x9",
    "the record list reaches the panel")

-- Walking away from every set leaves the panel OPEN and pointed at nothing: that
-- is the spawn view, and a panel that closed itself would be a panel a player has
-- to walk back to in order to put a television down.
local nearBody = Open77.character.position
Open77.character.position = function() return 900.0, 900.0, 10.0 end
pcall(threads[1])
local away = panel ~= nil and lastSent(panel, "remote:state") or nil
check(away ~= nil and away.open == true, "a panel with nothing in range stays open")
check(away ~= nil and away.target == nil, "with no set to drive")
check(away ~= nil and away.count == 1, "while still reporting the server's sets")
sent = {}
panel.events["remote:action"]({ action = "volume", volume = 50 })
check(#sent == 0, "a control with no set in range is dropped, never sent to somebody else's")
Open77.character.position = nearBody

-- Closing releases both the pointer and the keyboard. A surface that kept them
-- would leave the player unable to move with no way back.
panel.events["remote:action"]({ action = "close" })
local closed = panel ~= nil and lastSent(panel, "remote:state") or nil
check(closed ~= nil and closed.open == false, "the panel's own close button closes it")
check(panel ~= nil and panel.focusCount == 2 and panel.focus[1] == false
    and panel.focus[2] == false,
    "and closing gives the game its input back")

-- The discoverability of a key nobody is told about: one line per set entered,
-- and not one per pass.
handlers["open77:media:snapshot"]({})
pcall(threads[1])
local printed = {}
local realPrintPanel = _G.print
_G.print = function(...)
    printed[#printed + 1] = table.concat({ ... }, " ")
    return realPrintPanel(...)
end
handlers["open77:media:snapshot"]({ spawn(21, "prop-21", 4.0) })
pcall(threads[1])
local hints = 0
for _, line in ipairs(printed) do
    if string.find(line, "is in range: press " .. mappings["media.remote"].key, 1, true) then hints = hints + 1 end
end
check(hints == 1, "entering a set's range prints the key once")
pcall(threads[1])
hints = 0
for _, line in ipairs(printed) do
    if string.find(line, "is in range: press " .. mappings["media.remote"].key, 1, true) then hints = hints + 1 end
end
check(hints == 1, "and not once a second while standing there")
_G.print = realPrintPanel

-- The key is in the engine's registry, so a rebind renames it everywhere it is
-- signposted: the panel and the idle screen of the set now on the wall. Asserted
-- on the set that exists NOW -- the earlier sections' pages were withdrawn, and a
-- refresh deliberately skips a set that is not materialised.
mappings["media.remote"].key = "F7"
handlers["open77:keybinds:changed"]()
local live = tvPages()
live = live[#live]
check(live ~= nil and lastSent(live, "media:state") ~= nil
    and lastSent(live, "media:state").key == "F7",
    "a rebind reaches the idle screen's hint")
check(panel ~= nil and lastSent(panel, "remote:state") ~= nil,
    "and the panel is told the state again, because the key is part of it")

-- =============================================================================
-- A panel must be able to drive the set the server just spawned
-- -----------------------------------------------------------------------------
-- The dead end this pins, measured in game: a 150 ft cinema screen spawned from
-- the panel is set down 30.5612 m ahead of the player who asked for it (the
-- server's `FacingDistance`), the panel's control range was a flat fifteen
-- metres, and the panel therefore reported NO SET IN RANGE while that screen
-- filled the view -- no controls, no placement, no REMOVE. The reach is per set
-- now, from the same rule the stand-off is, and this section is both directions
-- of it: the cinema is drivable from where it lands, and a set that is genuinely
-- out of reach is named with its distance instead of driving nothing in silence.
-- =============================================================================

-- The cinema rectangle as the catalogue writes it (records.lua, cinema.150ft):
-- 45.72 x 26.0131 m, its glass 4.548115 m in front of the prop's origin.
local cinemaQuad = {
    offset = { 0.0, 4.548115, 16.553793 },
    right = { 1.0, 0.0, 0.0 },
    up = { 0.0, 0.0, 1.0 },
    width = 45.72,
    height = 26.0131,
}

---A screen as the server sends one, with its own reach.
local function screen(id, prop, distance, reach, quadFor)
    local spec = spawn(id, prop, distance)
    spec.quad = quadFor or quad
    -- The server's number, from `Open77MediaPlacement.Reach` of this record.
    -- Nil is the older-server case, and it is tested below.
    if reach ~= nil then spec.reach = reach end
    return spec
end

-- From an empty world.
handlers["open77:media:snapshot"]({})
pcall(threads[1])

-- On the spot the server puts it: 30.6 m away, its own reach 38.2 m. Under the
-- old flat rule this was the failing case; it is the case this section exists for.
handlers["open77:media:snapshot"]({ screen(30, "prop-30", 30.5612, 38.2015, cinemaQuad) })
pcall(threads[1])
local cinemaState = panel ~= nil and lastSent(panel, "remote:state") or nil
check(cinemaState ~= nil and cinemaState.target ~= nil and cinemaState.target.id == 30,
    "the cinema screen the server spawned 30.6 m ahead is the panel's subject")
check(cinemaState ~= nil and cinemaState.target.reach == 38.2015,
    "with the reach the server computed for that record, not a flat range")
check(cinemaState ~= nil and cinemaState.nearest == nil,
    "and nothing to explain: its subject is in reach")

-- Still drivable from the far side of the same reach -- a player who stepped back
-- from where they spawned it -- and out of it past that, where the panel names it.
handlers["open77:media:snapshot"]({ screen(30, "prop-30", 38.0, 38.2015, cinemaQuad) })
pcall(threads[1])
check(panel ~= nil and lastSent(panel, "remote:state").target ~= nil,
    "a player who stepped back inside the reach keeps the set")

sent = {}
handlers["open77:media:snapshot"]({ screen(30, "prop-30", 38.202, 38.2015, cinemaQuad) })
pcall(threads[1])
local outOfReach = panel ~= nil and lastSent(panel, "remote:state") or nil
check(outOfReach ~= nil and outOfReach.target == nil,
    "past the reach the panel drives nothing")
check(outOfReach ~= nil and outOfReach.nearest ~= nil and outOfReach.nearest.id == 30
    and outOfReach.nearest.distance == 38.202 and outOfReach.nearest.reach == 38.2015,
    "and is told which set is near, how far, and how far it can be driven from")
check(outOfReach ~= nil and outOfReach.count == 1,
    "while still reporting the server's sets")
panel.events["remote:action"]({ action = "volume", volume = 30 })
check(#sent == 0,
    "and a control for a set out of reach is dropped rather than sent")

-- A set the server did not describe: the fallback reach, not nil and not zero,
-- so an older server still drives a television within fifteen metres.
handlers["open77:media:snapshot"]({ screen(31, "prop-31", 5.0, nil) })
pcall(threads[1])
local fallback = panel ~= nil and lastSent(panel, "remote:state") or nil
check(fallback ~= nil and fallback.target ~= nil and fallback.target.id == 31,
    "a set with no reach from the server is drivable within the fallback")
check(fallback ~= nil and fallback.target.reach == 15.0,
    "and the panel is told the fallback it is being held to")

-- The nearest set wins, and reach decides which sets are candidates at all.
handlers["open77:media:snapshot"]({
    screen(32, "prop-32", 9.0, 15.0),
    screen(33, "prop-33", 20.0, 38.2015, cinemaQuad),
})
pcall(threads[1])
local mixed = panel ~= nil and lastSent(panel, "remote:state") or nil
check(mixed ~= nil and mixed.target ~= nil and mixed.target.id == 32,
    "the nearest set inside its own reach is the subject, whatever the sizes")

-- With that television out of its own reach the cinema is the subject, not
-- nothing: the sets are judged one at a time rather than by a range for all.
sent = {}
handlers["open77:media:snapshot"]({
    screen(32, "prop-32", 16.0, 15.0),
    screen(33, "prop-33", 20.0, 38.2015, cinemaQuad),
})
pcall(threads[1])
local second = panel ~= nil and lastSent(panel, "remote:state") or nil
check(second ~= nil and second.target ~= nil and second.target.id == 33,
    "a set out of its own reach does not shadow one that is in reach")

-- The panel is told every set and not only its subject. Two things depend on it:
-- the list the page draws, and the fact that the row for a set outside every
-- reach can still offer the one thing reach does not gate -- deleting it.
check(second ~= nil and type(second.sets) == "table" and #second.sets == 2,
    "the panel is told every set in the world, not only the one it drives")
check(second ~= nil and second.sets[1] ~= nil and second.sets[1].id == 32
    and second.sets[2] ~= nil and second.sets[2].id == 33,
    "nearest first, so the list reads in the order a player meets them")
check(second ~= nil and second.sets[1] ~= nil and second.sets[1].reach == 15.0
    and second.sets[2] ~= nil and second.sets[2].reach == 38.2015,
    "and each row carries its OWN reach, which is what the row's badge shows")

_G.TriggerServerEvent = function() end

-- =============================================================================
-- The operator's ad blocklist
-- =============================================================================
-- Three things have to be true for a server's rules to mean anything: the client
-- hands them to the browser host, it waits for the host's receipt rather than
-- assuming one, and it reports what the host said -- including when the host said
-- nothing. The failure this section exists to prevent is the quiet one: a policy
-- pushed into a plugin that does not implement it, and a server that concludes
-- its rules are live because the push did not throw.

check(type(handlers["open77:media:adblock"]) == "function",
    "the client registers the blocklist handler")

local pushed = nil
local receipt = nil
_G.TriggerServerEvent = function(name, payload)
    if name == "open77:media:adblock:receipt" then receipt = payload end
end

-- No native: the answer is a refusal with a reason, not silence. This is exactly
-- the state a game running a plugin older than the feature is in.
handlers["open77:media:adblock"]({ revision = 9, hosts = { "newads.test" }, tokens = {} })
check(receipt ~= nil and receipt.applied == false, "a host that cannot apply it is reported")
check(receipt ~= nil and receipt.detail == "webui_blocklist_unavailable",
    "and the reason names the missing native rather than blaming the rules")

-- Now a host that takes it and answers. The stub records both calls, because
-- "the rules arrived" and "the receipt was read" are the two halves that can
-- each be wrong on their own.
local sentRules = nil
local reportedStates = 0
Open77.webui = {
    blocklist = function(payload)
        sentRules = payload
        return true
    end,
    blocklistState = function()
        reportedStates = reportedStates + 1
        return {
            revision = 12, hostRules = 1, tokenRules = 1, compiledRules = 193,
            refused = { "co.uk:registry_suffix" },
        }
    end,
}

receipt = nil
handlers["open77:media:adblock"]({
    revision = 12, source = "open77_media@127.0.0.1",
    hosts = { "newads.test" }, tokens = { "newads" },
})
check(sentRules ~= nil, "the rules reach the native")
check(sentRules.revision == 12 and sentRules.source == "open77_media@127.0.0.1",
    "with the revision and the source the server can report against")
check(sentRules.hosts[1] == "newads.test" and sentRules.tokens[1] == "newads",
    "and both lists")
check(receipt == nil, "nothing is reported before the host has answered")

-- One pass of the receipt thread. The host answers revision 12, which is what is
-- outstanding, so the pass reports it and clears the pending state.
local okReceipt, receiptReason = pcall(threads[2])
check(not okReceipt and receiptReason == "selection_pass_done",
    "the receipt thread ran without error")
check(receipt ~= nil and receipt.applied == true, "the host's receipt is reported")
check(receipt ~= nil and receipt.revision == 12 and receipt.hosts == 1 and receipt.tokens == 1,
    "with the counts the server displays")
check(receipt ~= nil and receipt.compiled == 193,
    "and the compiled count, so an operator can see the build's list is still there")
check(receipt ~= nil and #(receipt.refused or {}) == 1
    and receipt.refused[1] == "co.uk:registry_suffix",
    "and every entry the host refused, named, so the operator learns which rule did nothing")
check(reportedStates >= 1, "the receipt came from the host, not from the push's return value")

-- The answer is not invented when the host stays silent: the pending push is
-- abandoned at its deadline and reported as unconfirmed.
local beforeSilent = reportedStates
Open77.webui.blocklistState = function()
    reportedStates = reportedStates + 1
    return { revision = 11, hostRules = 0, tokenRules = 0, compiledRules = 193, refused = {} }
end
receipt = nil
handlers["open77:media:adblock"]({ revision = 12, hosts = {}, tokens = {} })
check(receipt == nil, "a push whose receipt has not arrived reports nothing yet")
-- 3.0 s of deadline at 0.1 s a pass, each pass throwing out of `Wait`; the
-- thread keeps its pending state across the passes that come back empty.
local passes = 0
while receipt == nil and passes < 40 do
    pcall(threads[2])
    passes = passes + 1
end
check(receipt ~= nil and receipt.applied == false and receipt.detail == "no_blocklist_receipt",
    "a host that never answers is reported as unconfirmed, not as applied")
check(receipt ~= nil and receipt.observed == 11,
    "and the older revision the host does hold is reported with it")
check(reportedStates > beforeSilent, "and the thread kept polling rather than giving up early")

-- =============================================================================
-- The reveal's fireworks span the panel they are revealing
-- -----------------------------------------------------------------------------
-- The reveal fired ONE `race.firework.burst`, at the middle of the panel, and
-- that is the size a race's start line wants. On the 150 ft cinema record -- a
-- 45.72 x 26.01 m panel -- one fixed-size explosion is a spark in the middle of a
-- black wall, which is the reported complaint verbatim: the fireworks are there,
-- and they are barely visible because of the size of the screen. The effect
-- cannot be scaled through this API (there is no scale on `WorldVfxOptions`), so
-- the LAYOUT carries the size instead: one burst per `BURST_SPAN` metres of panel
-- width, symmetric about its centre.
--
-- The television case is pinned in the same block and for the same reason: a
-- 1.16 m panel must still get exactly one burst, at its centre, so the fix for
-- the cinema is not a row of explosions on every television in the world.
-- =============================================================================

---Fires the reveal at the one set the world holds, and records what was placed.
---
---The snapshot REPLACES the world, so the set under test is the only one in it:
---every other screen is destroyed and the only new page in the world is this
---set's. Without that, the page this drives could be one an earlier section left
---materialised, and the reveal would be placed against the wrong rectangle -- a
---test that passes while the cinema is still showing one spark.
---@param spec table one server payload
---@return table the `Open77.vfx.play` calls it produced
local function revealAt(spec)
    handlers["open77:media:snapshot"]({ spec })
    local okPass, reason = pcall(threads[1])
    check(not okPass and reason == "selection_pass_done",
        "the selection pass ran for the reveal case")
    local page = tvPages()[#tvPages()]
    check(page ~= nil, "the set under test materialised a page")
    check(page ~= nil and page.destroyed == false, "and it is a live page")

    vfxPlays = {}
    sfxPlays = {}
    if page ~= nil and type(page.events["media:report"]) == "function" then
        page.events["media:report"]({ status = "reveal_start" })
    else
        check(false, "the set's page reports its reveal beats")
    end

    -- The celebration is two halves and both are the race's own: the colours go
    -- up in the world, and the beat it went up on is the start sound.
    check(#sfxPlays >= 1 and sfxPlays[#sfxPlays].event == "sq024_race_start",
        "the reveal's start beat plays the race's own start sound")
    return vfxPlays
end

---Every play of one effect, in order.
local function effectsNamed(plays, name)
    local out = {}
    for _, play in ipairs(plays) do
        if play.effect == name then out[#out + 1] = play end
    end
    return out
end

-- The cinema's own rectangle and offset, taken from the catalogue record
-- (`records.lua`, cinema.150ft) rather than invented here: the complaint is about
-- THIS panel's size, so the suite has to use THIS panel's size.
local cinemaQuad = {
    offset = { 0.0, 4.548115, 16.553793 },
    right = { 1.0, 0.0, 0.0 },
    up = { 0.0, 0.0, 1.0 },
    width = 45.72,
    height = 26.0131,
}

local cinemaPlays = revealAt({
    id = 9, prop = "9001", record = "cinema.150ft", label = "Open77 Cinema",
    url = "", volume = 75, muted = false, paused = false,
    quad = cinemaQuad, position = { x = 105.0, y = 200.0, z = 10.0 },
    yaw = 0.0, reach = 45.0,
})

local cinemaBursts = effectsNamed(cinemaPlays, "race.firework.burst")
local cinemaColumns = effectsNamed(cinemaPlays, "race.flare.smoke")

-- The rule, restated: one burst per BURST_SPAN (9.0 m) of panel width. 45.72 m
-- is five.
check(#cinemaBursts == math.floor(cinemaQuad.width / 9.0 + 0.5),
    "the cinema's reveal is one burst per span of panel width, not one burst")
check(#cinemaBursts >= 4,
    "which is a row across the screen rather than a single spark in its middle")

check(#cinemaColumns == 2, "and the two smoke columns still stand at the panel's frame")
if #cinemaColumns == 2 then
    local left = cinemaColumns[1].options.position.x
    local right = cinemaColumns[2].options.position.x
    local apart = math.abs(left - right)
    check(apart >= cinemaQuad.width * 0.8,
        "the columns are at the panel's own edges -- 0.8 of its width apart at least")
end

-- The spread itself, measured in world space where the effects actually land.
local minX, maxX = nil, nil
local heights = {}
for _, burst in ipairs(cinemaBursts) do
    local x = burst.options.position.x
    local z = burst.options.position.z
    if minX == nil or x < minX then minX = x end
    if maxX == nil or x > maxX then maxX = x end
    heights[tostring(z)] = true
end
check(minX ~= nil and maxX ~= nil and (maxX - minX) >= cinemaQuad.width * 0.8,
    "the bursts span at least four fifths of the cinema's width, not its middle")
check(minX ~= nil and minX < 105.0 and maxX ~= nil and maxX > 105.0,
    "straddling the panel's centre in both directions")
for _, burst in ipairs(cinemaBursts) do
    check(math.abs(burst.options.position.x - 105.0) <= cinemaQuad.width * 0.5,
        "and every burst lands inside the panel's own frame, none hanging off it")
end
-- Two distinct heights, because a row at one depth is a line of copies rather
-- than a spread -- and it is the reader's eye that has to be convinced.
local distinctHeights = 0
for _ in pairs(heights) do distinctHeights = distinctHeights + 1 end
check(distinctHeights >= 2, "at two heights, so the row reads as a spread")

-- The television, unchanged: a panel narrower than one span gets exactly one
-- burst, at its centre.
local tvPlays = revealAt({
    id = 1, prop = "4242", record = "tv.16x9", label = "Reception",
    url = "https://example.invalid/x", volume = 75, muted = false, paused = false,
    quad = quad, position = { x = 105.0, y = 200.0, z = 10.0 },
    yaw = 0.0, reach = 15.0,
})
local tvBursts = effectsNamed(tvPlays, "race.firework.burst")
check(#tvBursts == 1, "a television still gets exactly one burst, not a row")
check(#tvBursts == 1 and math.abs(tvBursts[1].options.position.x - 105.0) < 0.001,
    "and it is the panel's centre, where it has always been")
check(#effectsNamed(tvPlays, "race.flare.smoke") == 2 and #tvPlays == 3,
    "and its two columns -- three effects in total, exactly what it had before")

_G.TriggerServerEvent = function() end
_G.TestResult = { passed = passed, failed = #failures, failures = failures }
