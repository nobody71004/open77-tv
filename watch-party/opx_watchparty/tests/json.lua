-- opx_watchparty tests: a small JSON for the stand-in platform (the real one has `json`).
local Json = {}
do
    local function esc(s)
        return (s:gsub('[%c"\\]', function(c)
            local map = { ['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }
            return map[c] or string.format("\\u%04x", c:byte())
        end))
    end
    local function isArray(t)
        local n = 0
        for k in pairs(t) do
            if type(k) ~= "number" then return false end
            n = n + 1
        end
        return n == #t
    end
    function Json.encode(v)
        local t = type(v)
        if v == nil then return "null" end
        if t == "boolean" then return v and "true" or "false" end
        if t == "number" then
            if v ~= v or v == math.huge or v == -math.huge then return "null" end
            if math.type(v) == "integer" or v == math.floor(v) then return string.format("%d", v) end
            return string.format("%.17g", v)
        end
        if t == "string" then return '"' .. esc(v) .. '"' end
        if t == "table" then
            local out = {}
            if next(v) ~= nil and isArray(v) then
                for _, x in ipairs(v) do out[#out + 1] = Json.encode(x) end
                return "[" .. table.concat(out, ",") .. "]"
            end
            local keys = {}
            for k in pairs(v) do keys[#keys + 1] = tostring(k) end
            table.sort(keys)
            for _, k in ipairs(keys) do out[#out + 1] = '"' .. esc(k) .. '":' .. Json.encode(v[k]) end
            return "{" .. table.concat(out, ",") .. "}"
        end
        error("cannot encode " .. t)
    end
    function Json.decode(s)
        local i = 1
        local function ws() i = s:find("[^ \t\r\n]", i) or #s + 1 end
        local value
        local function str()
            local out = {}
            i = i + 1
            while true do
                local c = s:sub(i, i)
                if c == "" then error("unterminated string") end
                if c == '"' then i = i + 1; break end
                if c == "\\" then
                    local n = s:sub(i + 1, i + 1)
                    local map = { n = "\n", r = "\r", t = "\t", ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f" }
                    if n == "u" then
                        out[#out + 1] = utf8.char(tonumber(s:sub(i + 2, i + 5), 16)); i = i + 6
                    else
                        out[#out + 1] = map[n] or n; i = i + 2
                    end
                else
                    out[#out + 1] = c; i = i + 1
                end
            end
            return table.concat(out)
        end
        function value()
            ws()
            local c = s:sub(i, i)
            if c == "{" then
                local obj = {}
                i = i + 1; ws()
                if s:sub(i, i) == "}" then i = i + 1; return obj end
                while true do
                    ws(); local k = str(); ws()
                    assert(s:sub(i, i) == ":", "colon expected"); i = i + 1
                    obj[k] = value(); ws()
                    local d = s:sub(i, i); i = i + 1
                    if d == "}" then return obj end
                    assert(d == ",", "comma expected")
                end
            elseif c == "[" then
                local arr = {}
                i = i + 1; ws()
                if s:sub(i, i) == "]" then i = i + 1; return arr end
                while true do
                    arr[#arr + 1] = value(); ws()
                    local d = s:sub(i, i); i = i + 1
                    if d == "]" then return arr end
                    assert(d == ",", "comma expected")
                end
            elseif c == '"' then return str()
            elseif s:sub(i, i + 3) == "true" then i = i + 4; return true
            elseif s:sub(i, i + 4) == "false" then i = i + 5; return false
            elseif s:sub(i, i + 3) == "null" then i = i + 4; return nil
            else
                local num = s:match("^-?%d+%.?%d*[eE]?[-+]?%d*", i)
                if num == nil or num == "" then error("unexpected " .. c .. " at " .. i) end
                i = i + #num
                return tonumber(num)
            end
        end
        local v = value()
        ws()
        if i <= #s then error("trailing characters") end
        return v
    end
end
return Json
