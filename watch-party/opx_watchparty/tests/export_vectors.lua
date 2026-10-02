-- Writes tests/decide_vectors.lua as JSON on stdout, for the browser extension's
-- tests: `lua tests/export_vectors.lua > ../opx_watchparty_extension/tests/decide_vectors.json`
local cases = dofile("tests/decide_vectors.lua")
local function enc(v)
    local t = type(v)
    if t == "number" then
        if math.type(v) == "integer" or v == math.floor(v) then return string.format("%d", v) end
        return string.format("%.17g", v)
    end
    if t == "boolean" then return tostring(v) end
    if t == "string" then return string.format("%q", v) end
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    if #v > 0 then
        local out = {}
        for _, x in ipairs(v) do out[#out + 1] = enc(x) end
        return "[" .. table.concat(out, ",") .. "]"
    end
    local out = {}
    for _, k in ipairs(keys) do out[#out + 1] = string.format("%q", k) .. ":" .. enc(v[k]) end
    return "{" .. table.concat(out, ",") .. "}"
end
io.write(enc(cases), "\n")
