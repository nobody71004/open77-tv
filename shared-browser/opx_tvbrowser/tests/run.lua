-- opx_tvbrowser -- tests/run.lua
--
-- The link rule, the server's commands and the client's choice of television,
-- against stubs of the host's calls (recorded, not simulated).
--
-- Standalone, from this folder:  lua tests/run.lua

local passed, failures = 0, {}
local function check(condition, message)
    if condition then passed = passed + 1 else failures[#failures + 1] = message end
end

local GOOD = "https://xbuniverse.duckdns.org/tv-browser-xb-staging/?usr=open77&pwd=abc&embed=1#open77-shared-browser"

-- ---------------------------------------------------------------------------
-- The link
-- ---------------------------------------------------------------------------
assert(loadfile("shared/link.lua"))()
local L = OpxTvBrowserLink
check(L.accept(GOOD) == GOOD, "a marked https link is accepted")
check(select(2, L.accept(nil)) == "not_configured", "no link is 'not configured'")
check(select(2, L.accept("http://x.example/#open77-shared-browser")) == "not_https", "plain http is refused")
check(select(2, L.accept("https://x.example/a")) == "no_mark", "a link without the mark is refused")
check(select(2, L.accept("https://x.example/a b#open77-shared-browser")) == "whitespace", "whitespace is refused")
check(L.isShared(GOOD) and not L.isShared("https://www.youtube.com/") and not L.isShared(nil), "isShared reads the mark")
check(L.verb({}) == "on" and L.verb({ "here" }) == "on" and L.verb({ "CINEMA" }) == "cinema" and L.verb({ "off" }) == "off",
    "verbs: none/here -> on, cinema, off")
check(L.verb({ "dance" }) == nil and select(2, L.verb({ "dance" })) == "dance", "an unknown verb is named")
check(L.cinemaSize({ "cinema" }) == "150" and L.cinemaSize({ "cinema", "150" }) == "150", "a cinema is 150 ft unless asked")
check(L.cinemaSize({ "cinema", "100" }) == "100" and L.cinemaSize({ "cinema", "100ft" }) == "100" and L.cinemaSize({ "cinema", "100 FT" }) == "100",
    "100 / 100ft / 100 FT ask for the 100 ft one")
check(L.cinemaSize({ "cinema", "7" }) == nil and select(2, L.cinemaSize({ "cinema", "7" })) == "7", "any other size is named")

-- ---------------------------------------------------------------------------
-- The server
-- ---------------------------------------------------------------------------
local function loadServer(url)
    local sent, commands, handlers, printed = {}, {}, {}, {}
    _G.TriggerClientEvent = function(name, target, ...) sent[#sent + 1] = { name = name, target = target, args = { ... } } end
    _G.RegisterCommand = function(name, fn) commands[name] = fn end
    _G.RegisterNetEvent = function(name, fn) handlers[name] = fn end
    _G.AddEventHandler = function() end
    _G.GetCurrentResourceName = function() return "opx_tvbrowser" end
    _G.print = function(text) printed[#printed + 1] = tostring(text) end
    OpxTvBrowserConfig = { url = url, cinemaRecord = "cinema.150ft.browser", cinema100Record = "cinema.100ft.browser" }
    assert(loadfile("server/main.lua"))()
    return sent, commands, handlers, printed
end
local realPrint = print

local sent, commands, handlers = loadServer(GOOD)
commands.browser(7, {}, "browser")
check(#sent == 1 and sent[1].name == "opx:tvbrowser:put" and sent[1].target == 7 and sent[1].args[1].url == GOOD,
    "/browser hands the link to the player who asked, and only to them")
sent[1] = nil
commands.browser(7, { "cinema" }, "browser cinema")
check(sent[1] and sent[1].name == "opx:tvbrowser:cinema" and sent[1].args[1].record == "cinema.150ft.browser", "/browser cinema asks for the 150 ft cinema")
sent[1] = nil
commands.browser(7, { "cinema", "100" }, "browser cinema 100")
check(sent[1] and sent[1].name == "opx:tvbrowser:cinema" and sent[1].args[1].record == "cinema.100ft.browser"
    and sent[1].args[1].url == GOOD, "/browser cinema 100 asks for the 100 ft browser cinema")
sent[1] = nil
commands.browser(7, { "cinema", "7" }, "browser cinema 7")
check(sent[1] and sent[1].name == "open77:command:result" and sent[1].args[2] == false
    and tostring(sent[1].args[3]):find("100", 1, true) ~= nil, "an unknown size is refused, naming the sizes")
for i = #sent, 1, -1 do sent[i] = nil end
commands.browser(7, { "off" }, "browser off")
check(sent[1] and sent[1].name == "opx:tvbrowser:off", "/browser off asks the client to clear its TV")
sent[1] = nil
commands.browser(7, { "dance" }, "browser dance")
check(sent[1] and sent[1].name == "open77:command:result" and sent[1].args[2] == false, "an unknown verb is refused in chat")
for i = #sent, 1, -1 do sent[i] = nil end
handlers["opx:tvbrowser:report"]({ ok = true, text = string.rep("x", 400) })
check(true, "a report without a source is ignored")

-- The link export: open77_media's browser cinema asks for it; nobody else is answered.
do
    local exported = {}
    _G.exports = function(name, fn) exported[name] = fn end
    local invoker = "open77_media"
    _G.GetInvokingResource = function() return invoker end
    loadServer(GOOD)
    check(type(exported.link) == "function", "the link is exported")
    check(exported.link and exported.link() == GOOD, "open77_media gets the link")
    invoker = "some_other_resource"
    check(exported.link and exported.link() == nil, "another resource does not")
    _G.exports = nil
end
do
    local s2, c2 = loadServer(nil)
    c2.browser(3, {}, "browser")
    check(s2[1] and s2[1].name == "open77:command:result" and s2[1].args[2] == false
        and tostring(s2[1].args[3]):find("not set up", 1, true) ~= nil,
        "without a link /browser says the browser is not set up, and sends no link")
    local s3, c3 = loadServer("https://x.example/no-mark")
    c3.browser(3, {}, "browser")
    check(s3[1] and s3[1].name == "open77:command:result" and s3[1].args[2] == false, "an unmarked configured link is not handed out")
end
print = realPrint

-- ---------------------------------------------------------------------------
-- The client
-- ---------------------------------------------------------------------------
local toServer, net = {}, {}
_G.TriggerServerEvent = function(name, ...) toServer[#toServer + 1] = { name = name, args = { ... } } end
_G.RegisterNetEvent = function(name, fn) net[name] = fn end
_G.AddEventHandler = function() end
_G.GetCurrentResourceName = function() return "opx_tvbrowser" end
local here = { x = 0, y = 0, z = 0 }
_G.Open77 = { character = {
    position = function() return here.x, here.y, here.z end,
    state = function() return { yaw = 90 } end,
} }
_G.print = function() end
assert(loadfile("client/main.lua"))()
print = realPrint

net["open77:media:snapshot"]({
    { id = 1, label = "Cinema screen, 150 ft", position = { x = 30, y = 0, z = 0 }, reach = 40, url = "" },
    { id = 2, label = "Television", position = { x = 2, y = 0, z = 0 }, reach = 3, url = "https://www.youtube.com/" },
})
local function lastServer(name)
    for i = #toServer, 1, -1 do if toServer[i].name == name then return toServer[i] end end
end

net["opx:tvbrowser:put"]({ url = GOOD })
local control = lastServer("open77:media:control")
check(control and control.args[1] == "url" and control.args[2].id == 2 and control.args[2].url == GOOD,
    "the nearest set in reach gets the link (the TV at 2 m, not the cinema at 30 m)")
local said = lastServer("opx:tvbrowser:report")
check(said and said.args[1].ok == true and said.args[1].text:find("F8", 1, true), "and the player is told to press F8")

toServer = {}
here.x = 100
net["opx:tvbrowser:put"]({ url = GOOD })
check(lastServer("open77:media:control") == nil, "no set in reach: nothing is changed")
said = lastServer("opx:tvbrowser:report")
check(said and said.args[1].ok == false and said.args[1].text:find("/browser cinema", 1, true), "and /browser cinema is suggested")

toServer = {}
here.x = 2
net["opx:tvbrowser:off"]({ url = GOOD })
check(lastServer("open77:media:control") == nil, "off refuses a set that does not show the shared browser")
net["open77:media:snapshot"]({
    { id = 2, label = "Television", position = { x = 2, y = 0, z = 0 }, reach = 3, url = GOOD },
})
net["opx:tvbrowser:off"]({ url = GOOD })
control = lastServer("open77:media:control")
check(control and control.args[2].id == 2 and control.args[2].url == "", "off clears a set that shows it")

toServer = {}
net["opx:tvbrowser:cinema"]({ url = GOOD, record = "cinema.150ft.browser" })
local spawn = lastServer("open77:media:spawn")
check(spawn and spawn.args[1].record == "cinema.150ft.browser" and spawn.args[1].url == GOOD and spawn.args[1].yaw == 90,
    "cinema spawns the 150 ft record with the link and the player's facing")
said = lastServer("opx:tvbrowser:report")
check(said and said.args[1].text:find("150 ft", 1, true), "and says 150 ft")
toServer = {}
net["opx:tvbrowser:cinema"]({ url = GOOD, record = "cinema.100ft.browser" })
spawn = lastServer("open77:media:spawn")
said = lastServer("opx:tvbrowser:report")
check(spawn and spawn.args[1].record == "cinema.100ft.browser" and said and said.args[1].text:find("100 ft", 1, true),
    "the 100 ft one spawns its own record and says 100 ft")
toServer = {}
net["opx:tvbrowser:put"]({ url = "https://evil.example/" })
check(#toServer == 0, "a link without the mark from anywhere is ignored by the client")

print(string.format("opx_tvbrowser: %d passed, %d failed", passed, #failures))
for _, f in ipairs(failures) do print("  FAIL " .. f) end
os.exit(#failures == 0 and 0 or 1)
