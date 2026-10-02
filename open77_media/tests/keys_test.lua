-- =============================================================================
-- open77_media -- tests/keys_test.lua
-- =============================================================================
-- The panel's key: the table that says which keys it may be opened with and what
-- each is called (`shared/keys.lua`), and the two commands every player has for
-- it (`/tv` and `/tvkey`, in `server/main.lua`).
--
-- Why a suite for a table of key names: a key the panel accepts and the host will
-- not read is a key that silently never fires. The host's list is restated below,
-- and every key the panel takes has to be on it. And why the commands are tested
-- from the server's side: a refused command is only ever drawn by the chat when
-- the SERVER refuses it, so the server has to be able to tell a player "W walks
-- forward" on its own, and send its answer to that player and to nobody else.
--
-- Standalone:  lua tools/lua-test/run.lua <repo-root>
-- (preloads shared/records.lua, shared/placement.lua, shared/keys.lua,
-- server/config.lua and server/adblock.lua, and names the root the server half is
-- loaded from in `OPEN77_REPO_ROOT`)
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

local Keys = Open77MediaKeys
check(type(Keys) == "table", "shared/keys.lua publishes Open77MediaKeys")
if type(Keys) ~= "table" then
    _G.TestResult = { passed = passed, failed = #failures, failures = failures }
    return
end

-- =============================================================================
-- The table
-- =============================================================================

-- The host's allowlist (`ResourceHost.cpp`, `ValidActionKey`), restated. Every
-- key the panel takes must be on it.
local hostKeys = {
    space = true, enter = true, ["return"] = true, tab = true, shift = true,
    ctrl = true, control = true, alt = true, capslock = true, backspace = true,
    insert = true, delete = true, home = true, ["end"] = true,
    pageup = true, pagedown = true, up = true, down = true, left = true, right = true,
}
for index = 1, 12 do hostKeys["f" .. index] = true end
for byte = string.byte("a"), string.byte("z") do hostKeys[string.char(byte)] = true end
for byte = string.byte("0"), string.byte("9") do hostKeys[string.char(byte)] = true end

local strays = {}
local taken = 0
local candidates = { "insert", "delete", "home", "end", "pageup", "pagedown", "space", "enter",
    "tab", "shift", "ctrl", "alt", "up", "down", "left", "right", "numpad5", "f13", "escape" }
for index = 1, 12 do candidates[#candidates + 1] = "f" .. index end
for byte = string.byte("a"), string.byte("z") do candidates[#candidates + 1] = string.char(byte) end
for byte = string.byte("0"), string.byte("9") do candidates[#candidates + 1] = string.char(byte) end
for _, name in ipairs(candidates) do
    if Keys.Valid(name) then
        taken = taken + 1
        if not hostKeys[name] then strays[#strays + 1] = name end
    end
end
check(#strays == 0, "every key the panel takes is one the host reads: " .. table.concat(strays, ", "))
check(taken == 12 + 26 - 6 + 10 + 6,
    "and it takes F1-F12, the letters but six, the digits and the six named keys (" .. taken .. ")")

check(Keys.DEFAULT == "f5" and Keys.Valid(Keys.DEFAULT), "F5 is the default, and a key the panel takes")

-- What a player types, and what it means.
local typed = {
    { "F5", "f5" }, { "f5", "f5" }, { "g", "g" }, { "G", "g" }, { "7", "7" },
    { "F12", "f12" }, { "F 12", "f12" }, { "Page Up", "pageup" }, { "page_up", "pageup" },
    { "PageDown", "pagedown" }, { "pgdn", "pagedown" }, { "PgUp", "pageup" },
    { "del", "delete" }, { "Insert", "insert" }, { "ins", "insert" }, { "End", "end" },
    { "reset", "f5" }, { "DEFAULT", "f5" },
}
for _, case in ipairs(typed) do
    local name = Keys.Parse(case[1])
    check(name == case[2], string.format("'%s' is the key %s (got %s)", case[1], case[2], tostring(name)))
end

-- Refused, and each refusal is a sentence a player can act on.
local function refused(text, says)
    local name, why = Keys.Parse(text)
    check(name == nil, string.format("'%s' is refused", tostring(text)))
    check(type(why) == "string" and string.find(why, says, 1, true) ~= nil,
        string.format("'%s' is refused with '%s' (got %s)", tostring(text), says, tostring(why)))
end
refused("w", "W walks forward")
refused("A", "A walks left")
refused("s", "S walks back")
refused("d", "D walks right")
refused("m", "mute")
refused("t", "chat")
refused("space", Keys.CHOICES)
refused("numpad5", Keys.CHOICES)
refused("f13", Keys.CHOICES)
refused("", "name a key")
refused(nil, "name a key")
local longName, longWhy = Keys.Parse(string.rep("x", 200))
check(longName == nil and #longWhy < 200, "a very long name is refused, and not echoed back whole")

check(Keys.Label("f5") == "F5" and Keys.Label("g") == "G" and Keys.Label("7") == "7",
    "a key is named as it is printed on the key")
check(Keys.Label("pageup") == "Page Up" and Keys.Label("pagedown") == "Page Down"
    and Keys.Label("end") == "End" and Keys.Label("delete") == "Delete",
    "and the named keys by their names")
check(Keys.Valid("w") == false and Keys.Valid(5) == false and Keys.Valid(nil) == false,
    "a stored value that is not a key the panel takes is not valid")

-- =============================================================================
-- /tv and /tvkey, from the server's side
-- =============================================================================

local commands = {}
local clientEvents = {}
_G.RegisterCommand = function(name, handler, restricted)
    commands[name] = { handler = handler, restricted = restricted }
end
_G.RegisterNetEvent = function() end
_G.AddEventHandler = function() end
_G.TriggerClientEvent = function(name, target, ...)
    clientEvents[#clientEvents + 1] = { name = name, target = target, args = table.pack(...) }
end
_G.GetCurrentResourceName = function() return "open77_media" end
-- Nothing of the host's is needed to register a command; the server half reads
-- `Open77.io` at load and does without it.
_G.Open77 = {}

local repoRoot = type(OPEN77_REPO_ROOT) == "string" and OPEN77_REPO_ROOT or "."
local serverPath = repoRoot .. "/resources/system/open77_media/server/main.lua"
local server = assert(loadfile(serverPath), serverPath .. " not found")
local realPrint = print
_G.print = function() end
local loaded, loadError = pcall(server)
_G.print = realPrint
check(loaded, "the server half loads: " .. tostring(loadError))

check(commands.tv ~= nil and commands.tv.restricted == false,
    "/tv is registered, and for every player: it opens the caller's own panel")
check(commands.tvkey ~= nil and commands.tvkey.restricted == false,
    "/tvkey too: it moves the caller's own key")

---Runs a command as a player (or as the console, source 0) and returns what the
---server sent.
local function run(name, source, ...)
    clientEvents = {}
    local args = table.pack(...)
    local raw = "/" .. name
    for index = 1, args.n do raw = raw .. " " .. tostring(args[index]) end
    if commands[name] ~= nil then
        local realPrintRun = print
        _G.print = function() end
        commands[name].handler(source, args, raw)
        _G.print = realPrintRun
    end
    return clientEvents
end

local function sent(events, name)
    for _, event in ipairs(events) do
        if event.name == name then return event end
    end
    return nil
end

local function onlyTo(events, source)
    for _, event in ipairs(events) do
        if event.target ~= source then return false end
    end
    return #events > 0
end

-- /tv
local events = run("tv", 7)
check(sent(events, "open77:media:remote:toggle") ~= nil, "/tv toggles the panel")
check(onlyTo(events, 7), "of the player who typed it, and of nobody else")
events = run("tv", 0)
check(sent(events, "open77:media:remote:toggle") == nil, "the console has no panel to open")

-- /tvkey <key>
events = run("tvkey", 7, "G")
local keyEvent = sent(events, "open77:media:remote:key")
check(keyEvent ~= nil and type(keyEvent.args[1]) == "table" and keyEvent.args[1].key == "g",
    "/tvkey G sends the caller's client the host's name for the key")
local result = sent(events, "open77:command:result")
check(result ~= nil and result.args[2] == true and string.find(tostring(result.args[3]), "G", 1, true) ~= nil,
    "and answers the command as accepted, naming the key")
local toast = sent(events, "opx:net:runtime:commandAnswer")
check(toast ~= nil and toast.args[2] == "success" and toast.args[4] == false,
    "and on the channel a chat that only draws refusals draws an accepted answer from")
check(onlyTo(events, 7), "every one of them to the caller alone")

events = run("tvkey", 7, "page", "up")
keyEvent = sent(events, "open77:media:remote:key")
check(keyEvent ~= nil and keyEvent.args[1].key == "pageup", "/tvkey page up is the Page Up key")

events = run("tvkey", 7, "reset")
keyEvent = sent(events, "open77:media:remote:key")
check(keyEvent ~= nil and keyEvent.args[1].key == "f5", "/tvkey reset is F5")

-- Refused, by the server, where the chat can draw it.
events = run("tvkey", 7, "w")
check(sent(events, "open77:media:remote:key") == nil, "a refused key is not sent to the client")
result = sent(events, "open77:command:result")
check(result ~= nil and result.args[2] == false
    and string.find(tostring(result.args[3]), "W walks forward", 1, true) ~= nil,
    "it is refused, with the reason, on the channel the chat draws refusals from")
check(sent(events, "opx:net:runtime:commandAnswer") == nil, "and is not also announced as a success")

-- /tvkey alone: the panel's chooser.
events = run("tvkey", 7)
keyEvent = sent(events, "open77:media:remote:key")
check(keyEvent ~= nil and keyEvent.args[1].choose == true,
    "/tvkey with no key opens the caller's panel on its key chooser")

events = run("tvkey", 0, "g")
check(sent(events, "open77:media:remote:key") == nil, "and the console has no key to move")

_G.TestResult = { passed = passed, failed = #failures, failures = failures }
