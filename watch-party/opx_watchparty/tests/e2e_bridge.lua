-- opx_watchparty tests: the party server's own rules behind a pipe, for the
-- browser extension's end-to-end test (opx_watchparty_extension/tests/e2e.mjs).
--
-- One JSON request per line on stdin, one JSON answer per line on stdout. The
-- caller hands in the time (`now`, ms) with every request, so this process and
-- the browser share one clock. Run from the resource folder:
--   lua tests/e2e_bridge.lua
local Json = dofile("tests/json.lua")
dofile("shared/playhead.lua")
dofile("shared/netflix.lua")
dofile("server/party.lua")
dofile("server/http.lua")
dofile("server/tvpage.lua")

local now = 0
local registry = WatchParty.new({ clock = function() return now end })
local env = {
    version = "e2e", encode = Json.encode, decode = Json.decode,
    tvPage = function(code) return WatchTvPage.html(code) end,
}

local function answer(value)
    io.write(Json.encode(value), "\n")
    io.flush()
end

for line in io.lines() do
    local ok, msg = pcall(Json.decode, line)
    if not ok or type(msg) ~= "table" then
        answer({ error = "bad_request" })
    else
        now = tonumber(msg.now) or now
        if msg.op == "start" then
            local party, key = registry:start(msg.player, { netflixId = msg.netflixId, title = msg.title, name = msg.name })
            answer({ code = party and party.code, key = key })
        elseif msg.op == "join" then
            local party, key = registry:join(msg.player, msg.code, msg.name)
            answer({ code = party and party.code, key = key })
        elseif msg.op == "control" then
            local party = registry:get(msg.code)
            local done, reason = registry:control(party, msg.action, { positionMs = msg.positionMs, deltaMs = msg.deltaMs })
            answer({ ok = done == true, reason = reason, state = registry:state(party) })
        elseif msg.op == "title" then
            local party = registry:get(msg.code)
            registry:setTitle(party, msg.netflixId, msg.title)
            answer({ state = registry:state(party) })
        elseif msg.op == "state" then
            local party = registry:get(msg.code)
            answer({ state = party and registry:state(party) or false })
        elseif msg.op == "http" then
            local status, body, headers = WatchHttp.handle(registry, msg.req, env)
            answer({ status = status, body = body, headers = headers })
        else
            answer({ error = "unknown_op" })
        end
    end
end
