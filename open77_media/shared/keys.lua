-- =============================================================================
-- open77_media -- shared/keys.lua
-- =============================================================================
-- The keys the television panel may be opened with, what each one is called,
-- and the one rule that turns whatever a player typed or pressed into one of
-- them.
--
-- Shared because both halves ask the same question. The CLIENT reads the key and
-- keeps the player's choice (`client/main.lua`); the SERVER answers `/tvkey`, and
-- a refused command is only drawn by the chat when the SERVER refuses it -- so the
-- server has to be able to say "W walks forward" without asking the client. One
-- table, loaded by both, is what keeps the two answers the same answer.
--
-- The names are the host's own. `Open77.input.isDown` takes a lower-case name
-- from a fixed allowlist (`ResourceHost.cpp`, `ValidActionKey`) and refuses
-- anything else with `unsupported_action_key`, which is a key that silently never
-- fires. This file allows a SUBSET of that list: the keys a player can press
-- without the press doing something else first.

Open77MediaKeys = {}
local Keys = Open77MediaKeys

-- F5, as it has always been. A default and not a fixture: `/tvkey` and the
-- panel's CHANGE button move it, per player and per server.
Keys.DEFAULT = "f5"

-- What a refusal offers instead, in the words a player reads.
Keys.CHOICES = "a letter, a number, F1-F12, Insert, Delete, Home, End, Page Up or Page Down"

local allowed = {}
for index = 1, 12 do allowed["f" .. index] = true end
for byte = string.byte("a"), string.byte("z") do allowed[string.char(byte)] = true end
for byte = string.byte("0"), string.byte("9") do allowed[string.char(byte)] = true end
for _, name in ipairs({ "insert", "delete", "home", "end", "pageup", "pagedown" }) do
    allowed[name] = true
end

-- In the host's list, and refused anyway, each with the reason the player is
-- told. The four movement keys would open the panel every time the player
-- walked. M is the panel's own mute key, so a panel opened with it would be a
-- panel whose first press mutes the set. T opens the chat on this runtime
-- (`opx_infinity`, `KEYS.OPEN`), and a key that opens two things opens them in
-- whichever order the frame happens to decide.
local reserved = {
    w = "W walks forward",
    a = "A walks left",
    s = "S walks back",
    d = "D walks right",
    m = "M is the panel's own mute key",
    t = "T opens the chat",
}

-- The shorter names people type for the keys that have long ones.
local aliases = {
    pgup = "pageup", pgdn = "pagedown", pgdown = "pagedown",
    del = "delete", ins = "insert",
}

local labels = {
    insert = "Insert", delete = "Delete", home = "Home", ["end"] = "End",
    pageup = "Page Up", pagedown = "Page Down",
}

-- The words that mean "put it back".
local resets = { reset = true, default = true }

---The name a player reads for a key: F5, G, 7, Page Up.
---@param name string the host's name for the key
---@return string
function Keys.Label(name)
    name = tostring(name or "")
    return labels[name] or name:upper()
end

---Whether a name is one the panel may be opened with.
---
---Used on a STORED name as well as a chosen one: a value written by an older
---build, or by hand, is dropped rather than polled for ever.
---@param name any
---@return boolean
function Keys.Valid(name)
    return type(name) == "string" and allowed[name] == true and reserved[name] == nil
end

---Turns what a player typed or pressed into the host's name for a key, or says
---why it cannot be one.
---
---Case, spaces, dashes and underscores are ignored, so `F5`, `f5`, `Page Up` and
---`page_up` are all the same key; `reset` and `default` are the default key.
---@param text any
---@return string|nil name the host's own name for the key
---@return string|nil reason for the player, when there is no name
function Keys.Parse(text)
    local typed = tostring(text or "")
    local cleaned = (typed:lower():gsub("[%s_%-]", ""))
    if cleaned == "" then return nil, "name a key: " .. Keys.CHOICES end
    if resets[cleaned] then return Keys.DEFAULT end
    cleaned = aliases[cleaned] or cleaned
    if reserved[cleaned] then return nil, reserved[cleaned] .. "; pick another key" end
    if not allowed[cleaned] then
        -- Named back, shortened: the refusal is read in a toast, and what was
        -- typed is the only way to tell a typo from a key that cannot be used.
        if #typed > 24 then typed = typed:sub(1, 24) .. "..." end
        return nil, string.format("%s can't open the panel; use %s", typed, Keys.CHOICES)
    end
    return cleaned
end
