-- =============================================================================
-- open77_media -- shared/acl.lua
-- =============================================================================
-- "May this player do it?" -- asked of Open77's own ACL, once, for both halves.
--
-- WHY THIS FILE EXISTS. The panel used to ask `IsPlayerAceAllowed`, which is
-- FiveM's function and does not exist on this runtime. The guard around it
-- (`type(...) ~= "function"`) was therefore true on every server, and the answer
-- was false for EVERY player: the owner, holding `command.media.lock`, opened the
-- panel and was told the panel was admin-only. The ACE was granted, the ACL was
-- right, and the name of the function was wrong. It is one line of question and
-- it is asked in three places, so it lives here and is pinned by its own suite.
--
-- THE HOST'S GATE IS `Open77.acl.isAllowed(source, 'command.<word>')` -- the same
-- call every restricted command goes through (`core/server/commands.lua`) and the
-- same ACL that hands out the ACE. FiveM's `IsPlayerAceAllowed` is kept as a
-- fallback so a FiveM-shaped host answers sensibly too.
--
-- THE CALL IS INSIDE THE pcall, NEVER HANDED TO IT. `pcall(Open77.acl.isAllowed, …)`
-- resolves the field before pcall runs and raises on a host with no `acl` table at
-- all -- the trap core's own command gate documents. Wrapped, a host with neither
-- API answers false: a panel nobody may use is a support ticket; a resource that
-- raises on every control is a dead server.
--
-- AND THE TABLE ONLY EXISTS IF THE MANIFEST ASKS FOR IT. Fixing the name was not
-- enough: the host installs `Open77.acl` for a resource that declares the
-- `acl.read` permission and for no one else, so with the grant missing this pcall
-- raised, the FiveM fallback was absent too, and every player -- the owner
-- holding `command.media.lock` included -- was still told the panel was
-- admin-only. That is a line in `open77.lua`, which is why it went unnoticed by
-- this suite: the resource's manifests are checked by `tools/run-suite.py` now.
--
-- THE CONSOLE IS ALWAYS AN ADMIN. `source` at or below zero is the console (and
-- the tests), which has no player row to ask about and is the operator by
-- definition -- the same rule the screen-lock already carried.

local M = {}

--- Whether `source` holds `ace`.
-- @param source integer the player id; 0 or below is the console
-- @param ace string a permission name, e.g. "command.media.lock"
-- @return boolean
function M.Allowed(source, ace)
    if source == nil or source <= 0 then return true end
    if type(ace) ~= "string" or ace == "" then return false end

    local ok, allowed = pcall(function() return Open77.acl.isAllowed(source, ace) end)
    if ok then return allowed == true end

    local fivem = rawget(_G, "IsPlayerAceAllowed")
    if type(fivem) == "function" then
        local asked, answer = pcall(fivem, source, ace)
        return asked and answer == true
    end
    return false
end

Open77MediaAcl = M
return M
