-- opx_watchparty -- server/http.lua
--
-- What a browser can ask. Served through `Open77.http.listen` on the server's own
-- listener (`httpHandlers`, loopback), which the web server in front of it
-- publishes under HTTPS; this file only maps a request to an answer, so the
-- tests drive it with plain tables.
--
--   GET  /v1/health                    is it up, which version
--   GET  /v1/party/<CODE>              the party: title, where the film is, the
--                                      server's clock (anyone with the code)
--   POST /v1/party/<CODE>/report       a browser says where ITS player is; the
--                                      answer is the party (followers too)
--   POST /v1/party/<CODE>/control      a member's browser moves the party
--                                      (play, pause, seek), by its key
--   GET  /v1/tv/<CODE>                 the page a television shows
--   POST /v1/selftest                  a two-minute test party; from this machine
--                                      only (not through the web server)
--
-- Bodies are JSON, sent as text/plain by the extension so no browser preflight
-- is needed. Every answer carries `Access-Control-Allow-Origin: *`: reading a
-- party is public by design, and moving one needs a key.

WatchHttp = {}

local JSON_HEADERS = {
    ["Content-Type"] = "application/json; charset=utf-8",
    ["Access-Control-Allow-Origin"] = "*",
    ["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS",
    ["Access-Control-Allow-Headers"] = "Content-Type",
    ["Access-Control-Max-Age"] = "600",
}

local HTML_HEADERS = {
    ["Content-Type"] = "text/html; charset=utf-8",
    ["Access-Control-Allow-Origin"] = "*",
    -- The television frames this page; nothing else needs to.
    ["Referrer-Policy"] = "no-referrer",
}

local ACTIONS = { play = true, pause = true, toggle = true, seek = true, nudge = true }

---A request header, whatever shape the host hands them in (a map, case-insensitive
---or not, or a list of pairs).
function WatchHttp.header(req, name)
    local headers = req and req.headers
    if type(headers) ~= "table" then return nil end
    local lower = name:lower()
    local direct = headers[name] or headers[lower]
    if direct ~= nil then return direct end
    for k, v in pairs(headers) do
        if type(k) == "string" and k:lower() == lower then return v end
        if type(v) == "table" then
            local n = v.name or v.key or v[1]
            if type(n) == "string" and n:lower() == lower then return v.value or v[2] end
        end
    end
    return nil
end

---True for a request made on this machine and not passed on by the web server.
function WatchHttp.isLocal(req)
    local remote = tostring(req.remoteAddress or "")
    local loopback = remote == "127.0.0.1" or remote == "::1" or remote == "::ffff:127.0.0.1"
    if not loopback then return false end
    return WatchHttp.header(req, "X-Forwarded-For") == nil and WatchHttp.header(req, "X-Real-IP") == nil
end

---@param env table { encode, decode, version, tvPage = function(code) -> html, selftest = function() -> party, key }
---@return number status, string body, table headers, boolean changed
function WatchHttp.handle(registry, req, env)
    local function json(status, value)
        return status, env.encode(value), JSON_HEADERS, false
    end
    local method = tostring(req.method or "GET"):upper()
    local path = tostring(req.path or "/")
    path = path:gsub("[?#].*$", "")
    path = path:gsub("^/opx_watchparty", "")
    if path == "" then path = "/" end
    if #path > 1 then path = path:gsub("/+$", "") end

    if method == "OPTIONS" then
        return 204, "", JSON_HEADERS, false
    end

    if path == "/v1/health" or path == "/" or path == "/v1" then
        if method ~= "GET" then return json(405, { ok = false, error = "method_not_allowed" }) end
        return json(200, { ok = true, service = "opx_watchparty", version = env.version,
            parties = registry.count, serverMs = registry:now() })
    end

    if path == "/v1/selftest" then
        if method ~= "POST" then return json(405, { ok = false, error = "method_not_allowed" }) end
        if not WatchHttp.isLocal(req) or env.selftest == nil then
            return json(403, { ok = false, error = "local_only" })
        end
        local party, key = env.selftest()
        if party == nil then return json(503, { ok = false, error = tostring(key) }) end
        local state = registry:state(party)
        state.ok = true
        state.key = key
        return 200, env.encode(state), JSON_HEADERS, true
    end

    local code, rest = path:match("^/v1/party/([A-Za-z2-9]+)(.*)$")
    if code == nil then
        local tvCode = path:match("^/v1/tv/([A-Za-z2-9]+)$")
        if tvCode ~= nil then
            if method ~= "GET" then return json(405, { ok = false, error = "method_not_allowed" }) end
            tvCode = tvCode:upper()
            if not WatchParty.validCode(tvCode) then return json(404, { ok = false, error = "no_such_party" }) end
            -- The page is served for an unknown code too: it says so itself, and
            -- picks the party up if it starts later under that code.
            return 200, env.tvPage(tvCode), HTML_HEADERS, false
        end
        return json(404, { ok = false, error = "not_found" })
    end
    code = code:upper()
    if not WatchParty.validCode(code) then return json(404, { ok = false, error = "no_such_party" }) end
    local party = registry:get(code)

    if rest == "" then
        if method ~= "GET" then return json(405, { ok = false, error = "method_not_allowed" }) end
        if party == nil then return json(404, { ok = false, error = "no_such_party" }) end
        local state = registry:state(party)
        state.ok = true
        return json(200, state)
    end

    if method ~= "POST" then return json(405, { ok = false, error = "method_not_allowed" }) end
    if party == nil then return json(404, { ok = false, error = "no_such_party" }) end
    local decoded = nil
    if type(req.body) == "string" and req.body ~= "" then
        local ok, value = pcall(env.decode, req.body)
        if ok and type(value) == "table" then decoded = value end
    end
    if decoded == nil then return json(400, { ok = false, error = "json_body_expected" }) end
    local key = decoded.key
    if key ~= nil and type(key) ~= "string" then key = nil end

    if rest == "/report" then
        local changed, reason
        if key ~= nil and key ~= "" then
            changed, reason = registry:report(code, key, true, decoded)
        else
            changed, reason = registry:report(code, decoded.viewer, false, decoded)
        end
        if changed == nil then return json(reason == "no_such_party" and 404 or 403, { ok = false, error = reason }) end
        local state = registry:state(party)
        state.ok = true
        return 200, env.encode(state), JSON_HEADERS, changed == true
    end

    if rest == "/control" then
        local action = decoded.action
        if type(action) ~= "string" or not ACTIONS[action] then
            return json(400, { ok = false, error = "unknown_action" })
        end
        local moved, reason = registry:controlByKey(code, key, action,
            { positionMs = tonumber(decoded.positionMs), deltaMs = tonumber(decoded.deltaMs) })
        if moved == nil then
            local status = 400
            if reason == "key_required" or reason == "key_not_in_party" then status = 403 end
            if reason == "too_many_commands" then status = 429 end
            return json(status, { ok = false, error = reason })
        end
        local state = registry:state(moved)
        state.ok = true
        return 200, env.encode(state), JSON_HEADERS, true
    end

    return json(404, { ok = false, error = "not_found" })
end
