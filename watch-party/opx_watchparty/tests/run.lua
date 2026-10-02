--- opx_watchparty tests: `lua tests/run.lua` from the resource folder.
--
-- The pure parts (playhead, Netflix links, the party registry, the HTTP routes)
-- directly, then both main.lua files booted against a stand-in of the platform:
-- a party started from chat on the television in front of the player, moved from
-- the panel and from a browser by key, followed by a browser report, ended.

local failures, checks = 0, 0
local function check(label, ok, detail)
    checks = checks + 1
    if ok then
        print("  ok   " .. label)
    else
        failures = failures + 1
        print("  FAIL " .. label .. (detail ~= nil and ("  -- " .. tostring(detail)) or ""))
    end
end
local function section(name) print("\n== " .. name) end

local Json = dofile("tests/json.lua")

dofile("shared/playhead.lua")
dofile("shared/netflix.lua")
dofile("server/party.lua")
dofile("server/http.lua")
dofile("server/tvpage.lua")

-- =========================================================== playhead ======
section("the shared playhead")
local P = WatchPlayhead
local p = P.new(1000)
check("a new playhead is paused at the start", P.position(p, 99999) == 0 and select(2, P.position(p, 99999)) == false)
P.play(p, 1000)
check("playing, it moves one second per second", P.position(p, 6000) == 5000)
P.pause(p, 6000)
check("paused, it stays", P.position(p, 60000) == 5000)
P.seek(p, 60000, 90000)
check("a seek keeps pause as it was", P.position(p, 70000) == 90000 and select(2, P.position(p, 70000)) == false)
P.play(p, 70000)
P.nudge(p, 75000, -30000)
check("a nudge moves from where the film is (95 s - 30 s)", P.position(p, 75000) == 65000)
P.nudge(p, 75000, -999999)
check("never below zero", P.position(p, 75000) == 0)
P.toggle(p, 75000)
check("toggle pauses a playing film", select(2, P.position(p, 75000)) == false)
P.toggle(p, 76000)
check("and plays a paused one", select(2, P.position(p, 77000)) == true and P.position(p, 77000) == 1000)
check("a duration under a second is refused", P.setDuration(p, 77000, 500) == false)
check("a duration is learnt", P.setDuration(p, 77000, 10000) == true and p.durationMs == 10000)
check("and a report within a second of it changes nothing", P.setDuration(p, 77000, 10400) == false)
local endMs, endPlaying = P.position(p, 120000)
check("the film stops at its end", endMs == 10000 and endPlaying == false, endMs)
P.play(p, 120000)
check("play at the end starts it over", P.position(p, 120000) == 0 and select(2, P.position(p, 121000)) == true)
P.seek(p, 121000, 999999)
check("a seek past the end lands on the end", P.position(p, 121000) == 10000)

local DECIDE = dofile("tests/decide_vectors.lua")
local decideOk, decideDetail = true, nil
for _, v in ipairs(DECIDE) do
    local got = P.decide(v.localMs, v.localPlaying, v.targetMs, v.targetPlaying, v.toleranceMs)
    if got.seekMs ~= v.want.seekMs or (got.play == true) ~= (v.want.play == true)
        or (got.pause == true) ~= (v.want.pause == true) or got.driftMs ~= v.want.driftMs then
        decideOk, decideDetail = false, v.name
        break
    end
end
check(("a follower's decision, %d cases shared with the browser extension"):format(#DECIDE), decideOk, decideDetail)

local times = {
    { "1:02:03", 0, 3723000 }, { "62:03", 0, 3723000 }, { "95", 0, 95000 }, { "95s", 0, 95000 },
    { "12m", 0, 720000 }, { "1h", 0, 3600000 }, { "1h2m3s", 0, 3723000 }, { "+30", 10000, 40000 },
    { "-10", 25000, 15000 }, { "-10", 5000, 0 }, { "+1:30", 0, 90000 }, { " 2:00 ", 0, 120000 },
}
local timesOk, timesDetail = true, nil
for _, t in ipairs(times) do
    local got = P.parseTime(t[1], t[2])
    if got ~= t[3] then timesOk, timesDetail = false, t[1] .. " -> " .. tostring(got) end
end
check("times as players type them: 1:02:03, 62:03, 95, 95s, 12m, 1h2m3s, +30, -10", timesOk, timesDetail)
check("and refuses what is not a time",
    P.parseTime("abc") == nil and P.parseTime("1:75") == nil and P.parseTime("") == nil
        and P.parseTime("13h") == nil and P.parseTime(nil) == nil and P.parseTime("1:02:99") == nil)
check("times are written back the same way",
    P.format(3723000) == "1:02:03" and P.format(65000) == "1:05" and P.format(0) == "0:00" and P.format(-5) == "0:00")

-- ============================================================ netflix ======
section("Netflix links")
local N = WatchNetflix
local links = {
    { "https://www.netflix.com/watch/80057281?trackId=14170286", "80057281" },
    { "https://www.netflix.com/title/80057281", "80057281" },
    { "www.netflix.com/gb/title/80057281", "80057281" },
    { "netflix.com/watch/81231974", "81231974" },
    { "https://www.netflix.com/browse?jbv=80057281", "80057281" },
    { "80057281", "80057281" },
    { "  https://WWW.NETFLIX.COM/WATCH/80057281  ", "80057281" },
}
local linksOk, linksDetail = true, nil
for _, l in ipairs(links) do
    if N.parse(l[1]) ~= l[2] then linksOk, linksDetail = false, l[1] end
end
check("every shape of a Netflix link gives its title number", linksOk, linksDetail)
check("other sites, and links with no title, are refused",
    select(2, N.parse("https://www.youtube.com/watch?v=abc")) == "not_a_netflix_link"
        and select(2, N.parse("https://evilnetflix.com/watch/80057281")) == "not_a_netflix_link"
        and select(2, N.parse("https://www.netflix.com/browse")) == "no_title_in_link"
        and select(2, N.parse("")) == "netflix_link_expected" and N.parse("123") == nil)
check("the browser link carries the time and, in the fragment, the code and key",
    N.watchUrl("80057281", 95500, "ABC234", "k3ykeyk3ykey") == "https://www.netflix.com/watch/80057281?t=95#opxwatch=ABC234.k3ykeyk3ykey"
        and N.watchUrl("80057281", 0) == "https://www.netflix.com/watch/80057281")

-- ========================================================== registry =======
section("the parties")
local clock = 1000000
local seed = 0
local reg = WatchParty.new({ clock = function() return clock end, random = function(n)
    seed = (seed * 1103515245 + 12345) % 2147483648
    return seed % n + 1
end })
local party, key, how = reg:start(7, { netflixId = "80057281", title = "  Stranger \n Things  ", tvId = 3, name = "Matt" })
check("a party starts on a television, with its host and a browser key",
    party ~= nil and how == "started" and WatchParty.validCode(party.code) and WatchParty.validKey(key)
        and party.host == 7 and party.hostName == "Matt" and reg:atTv(3) == party and reg:partyOf(7) == party)
check("its title is cleaned (no control characters, no runs of spaces)", party.title == "Stranger Things", party.title)
local party2, key2 = reg:join(8, party.code, "Ana")
check("a second player joins by code and gets a key of their own",
    party2 == party and WatchParty.validKey(key2) and key2 ~= key and party.memberCount == 2)
local same = reg:start(8, { netflixId = "81231974", title = "", tvId = 3, name = "Ana" })
check("starting at a television that has a party switches that party to the new title, from the start",
    same == party and party.netflixId == "81231974" and party.host == 8 and party.title == ""
        and P.position(party.playhead, clock) == 0)
reg:control(party, "play", {})
clock = clock + 5000
check("the party plays", reg:state(party).positionMs == 5000 and reg:state(party).playing == true)
check("a seek outside the film is refused", select(2, reg:control(party, "seek", { positionMs = -5 })) == "position_out_of_range")
check("an unknown action is refused", select(2, reg:control(party, "rewind", {})) == "unknown_action")
local moved, why, by = reg:controlByKey(party.code, key, "pause", {})
check("a member's browser pauses the party by its key", moved == party and by == 7 and reg:state(party).playing == false)
check("a key from no party, or no key, moves nothing",
    select(2, reg:controlByKey(party.code, "aaaaaaaaaaaa", "play", {})) == "key_not_in_party"
        and select(2, reg:controlByKey(party.code, nil, "play", {})) == "key_required")
local limited
for _ = 1, 10 do
    local ok2, reason = reg:controlByKey(party.code, key, "toggle", {})
    if ok2 == nil then limited = reason; break end
end
check("and a browser is held to 8 commands in 10 s", limited == "too_many_commands")
clock = clock + 11000
check("then allowed again", reg:controlByKey(party.code, key, "pause", {}) == party)

-- reports
reg:control(party, "seek", { positionMs = 60000 })
reg:control(party, "play", {})
local changed = reg:report(party.code, key2, true, { netflixId = "81231974", positionMs = 60500, playing = true,
    durationMs = 3000000, title = "Wednesday" })
local st = reg:state(party)
check("a member's browser teaches the party the film's length and, unnamed, its title",
    changed == true and st.durationMs == 3000000 and st.title == "Wednesday" and st.browsers == 1 and st.inSync == 1)
reg:report(party.code, "follower-0001", false, { netflixId = "81231974", positionMs = 20000, playing = true,
    durationMs = 99, title = "Spoof" })
st = reg:state(party)
check("a follower (no key) is counted, out of step, and teaches nothing",
    st.browsers == 2 and st.inSync == 1 and st.title == "Wednesday" and st.durationMs == 3000000)
check("a follower needs an id of its own",
    select(2, reg:report(party.code, "x", false, {})) == "viewer_id_expected")
reg:setTitle(party, "81231974", "Wednesday S1")
reg:report(party.code, key2, true, { netflixId = "81231974", positionMs = 60500, playing = true, title = "Other" })
check("a title a player gave is not overwritten by a browser", reg:state(party).title == "Wednesday S1")

-- leaving, hosts, ending
reg:leave(8)
check("when the host leaves, the next member hosts", party.host == 7 and party.hostName == "Matt" and party.memberCount == 1)
check("and their key is dead", select(2, reg:controlByKey(party.code, key2, "play", {})) == "key_not_in_party")
reg:leave(7)
check("a party left empty waits", reg:get(party.code) == party and party.emptySince == clock)
clock = clock + WatchParty.EMPTY_GRACE_MS + 1
local ended = reg:sweep()
check("and ends two minutes later, taking its television binding with it",
    #ended == 1 and reg:get(party.code) == nil and reg:atTv(3) == nil and reg.count == 0)
local t = reg:start(-1, { netflixId = "80057281", title = "Self test", test = true, name = "self-test" })
clock = clock + WatchParty.TEST_LIFETIME_MS + 1
check("a self-test party ends after two minutes", t ~= nil and #reg:sweep() == 1 and reg.count == 0)

-- ============================================================== http =======
section("what a browser can ask")
local env = { version = "test", encode = Json.encode, decode = Json.decode,
    tvPage = function(code) return WatchTvPage.html(code) end,
    selftest = function() return reg:start(-1, { netflixId = "80057281", title = "Self test", test = true }) end }
local function call(method, path, body, extra)
    local req = { method = method, path = path, body = body and Json.encode(body) or "", remoteAddress = "203.0.113.9",
        headers = {} }
    for k, v in pairs(extra or {}) do req[k] = v end
    local status, text, headers, chg = WatchHttp.handle(reg, req, env)
    local decoded = nil
    if headers["Content-Type"]:find("json", 1, true) and text ~= "" then decoded = Json.decode(text) end
    return status, decoded, headers, chg, text
end
local hp, hkey = reg:start(11, { netflixId = "80057281", title = "Dark", tvId = 9, name = "Lee" })
local s1, b1, h1 = call("GET", "/v1/party/" .. hp.code)
check("anyone with the code reads the party, with the server's clock and CORS open",
    s1 == 200 and b1.code == hp.code and b1.title == "Dark" and b1.serverMs == clock and b1.key == nil
        and h1["Access-Control-Allow-Origin"] == "*")
check("lower-case codes and the resource prefix are accepted",
    call("GET", "/opx_watchparty/v1/party/" .. hp.code:lower()) == 200)
check("an unknown party is a 404", call("GET", "/v1/party/ZZZZZZ") == 404)
check("a preflight is answered", call("OPTIONS", "/v1/party/" .. hp.code .. "/control") == 204)
local s2, b2, _, c2 = call("POST", "/v1/party/" .. hp.code .. "/control", { key = hkey, action = "seek", positionMs = 120000 })
check("a member's browser seeks the party by key (and the server re-broadcasts)",
    s2 == 200 and b2.positionMs == 120000 and c2 == true)
check("without a key it is 403, with an unknown action 400",
    call("POST", "/v1/party/" .. hp.code .. "/control", { action = "play" }) == 403
        and call("POST", "/v1/party/" .. hp.code .. "/control", { key = hkey, action = "explode" }) == 400)
check("a body that is not JSON is a 400",
    call("POST", "/v1/party/" .. hp.code .. "/control", nil, { body = "not json" }) == 400)
local s3, b3 = call("POST", "/v1/party/" .. hp.code .. "/report", { viewer = "browser-abcdef12", netflixId = "80057281",
    positionMs = 120300, playing = false })
check("a follower's report is answered with the party", s3 == 200 and b3.browsers == 1 and b3.inSync == 1)
local s4, _, h4, _, page = call("GET", "/v1/tv/" .. hp.code)
check("the television page is HTML that polls its own party",
    s4 == 200 and h4["Content-Type"]:find("text/html", 1, true) ~= nil and page:find('"../party/" + CODE', 1, true) ~= nil
        and page:find(hp.code, 1, true) ~= nil and page:find("__CODE__", 1, true) == nil)
check("and shows no picture of the film: no video, no frame, no capture",
    page:find("<video", 1, true) == nil and page:find("<iframe", 1, true) == nil
        and page:find("getDisplayMedia", 1, true) == nil and page:find("captureStream", 1, true) == nil)
check("a self-test party is refused from outside", call("POST", "/v1/selftest", {}) == 403)
check("and through the web server, even from this machine",
    call("POST", "/v1/selftest", {}, { remoteAddress = "127.0.0.1", headers = { ["X-Forwarded-For"] = "203.0.113.9" } }) == 403)
local s5, b5 = call("POST", "/v1/selftest", {}, { remoteAddress = "127.0.0.1" })
check("but made on this machine, with a key to drive it", s5 == 200 and b5.test == true and WatchParty.validKey(b5.key))
check("health answers", call("GET", "/v1/health") == 200)

-- ======================================================= server main =======
section("the server, booted against a stand-in platform")
local Host = { events = {}, clientEvents = {}, commands = {}, threads = {}, prints = {}, http = nil }
local function resetGlobals()
    _G.source = nil
    _G.json = Json
    _G.GetGameTimer = function() return clock end
    _G.Open77 = {
        time = { unix = function() return clock / 1000 end },
        players = { all = function() return { 7, 8 } end },
        http = { listen = function(prefix, fn) Host.http = fn; return "/opx_watchparty" .. prefix end },
        chat = { send = function() return true end },
    }
    _G.GetPlayerName = function(id) return ({ [7] = "Matt", [8] = "Ana" })[id] or ("p" .. tostring(id)) end
    _G.GetCurrentResourceName = function() return "opx_watchparty" end
    _G.RegisterNetEvent = function(name, fn) Host.events[name] = fn end
    _G.AddEventHandler = function(name, fn) Host.events[name] = fn end
    _G.TriggerClientEvent = function(name, target, ...) Host.clientEvents[#Host.clientEvents + 1] = { name = name, target = target, args = { ... } } end
    _G.RegisterCommand = function(name, fn, restricted) Host.commands[name] = { fn = fn, restricted = restricted } end
    _G.CreateThread = function(fn) Host.threads[#Host.threads + 1] = fn end
    _G.Wait = function() end
end
resetGlobals()
local realPrint = print
_G.print = function(...) local parts = {} for i = 1, select("#", ...) do parts[#parts + 1] = tostring(select(i, ...)) end
    Host.prints[#Host.prints + 1] = table.concat(parts, " ") end
dofile("server/party.lua")
dofile("server/http.lua")
dofile("server/main.lua")
Host.events.onResourceStart("opx_watchparty")
_G.print = realPrint
local function lastClient(name, target)
    for i = #Host.clientEvents, 1, -1 do
        local e = Host.clientEvents[i]
        if e.name == name and (target == nil or e.target == target) then return e end
    end
end
local banner = Host.prints[#Host.prints] or ""
check("it serves browsers on start and says where", banner:find("ready 1.0.1: browsers reach https://xbuniverse.duckdns.org/opx-watch-xb-staging", 1, true) ~= nil, banner)
check("/watch is a command every player may use", Host.commands.watch ~= nil and Host.commands.watch.restricted == false)
Host.clientEvents = {}
Host.commands.watch.fn(7, { "netflix", "https://www.netflix.com/watch/80057281", "Dark", "S1" }, "watch netflix ...")
local pick = lastClient("opx:watch:pick", 7)
check("/watch netflix asks the player's client which television is in front of them",
    pick ~= nil and pick.args[1].netflixId == "80057281" and pick.args[1].title == "Dark S1")
Host.clientEvents = {}
_G.source = 7
Host.events["opx:watch:start"]({ netflixId = "80057281", title = "Dark S1", tvId = 4 })
local you = lastClient("opx:watch:you", 7)
local bind = lastClient("opx:watch:bindtv", 7)
local stateEv = lastClient("opx:watch:state", 8)
check("the party starts on that television: the player gets their code and key, the set gets the page",
    you ~= nil and WatchParty.validCode(you.args[1].code) and WatchParty.validKey(you.args[1].key)
        and bind ~= nil and bind.args[1].tvId == 4
        and bind.args[1].url == "https://xbuniverse.duckdns.org/opx-watch-xb-staging/v1/tv/" .. you.args[1].code)
check("and every player is told about it", stateEv ~= nil and stateEv.args[1].parties[1].code == you.args[1].code
    and stateEv.args[1].parties[1].tvId == 4)
local code, mkey = you.args[1].code, you.args[1].key
Host.clientEvents = {}
Host.commands.watch.fn(7, { "play" }, "watch play")
clock = clock + 3000
Host.commands.watch.fn(7, { "+30" }, "watch +30")
local st2 = lastClient("opx:watch:state", 7).args[1].parties[1]
check("/watch play and /watch +30 move it", st2.playing == true and st2.positionMs == 33000, st2.positionMs)
Host.clientEvents = {}
Host.commands.watch.fn(8, { "pause" }, "watch pause")
local refused = lastClient("open77:command:result", 8)
check("a player not in the party cannot move it", refused ~= nil and refused.args[2] == false)
local resStatus, resBody, resHeaders
Host.http({ method = "POST", path = "/v1/party/" .. code .. "/control", remoteAddress = "127.0.0.1",
    headers = { ["X-Real-IP"] = "198.51.100.4" }, body = Json.encode({ key = mkey, action = "pause" }) },
    { send = function(s, b, h) resStatus, resBody, resHeaders = s, b, h end })
check("a browser pauses it over HTTP, by key, and every client hears of it",
    resStatus == 200 and Json.decode(resBody).playing == false and lastClient("opx:watch:state", 8) ~= nil)
Host.clientEvents = {}
_G.source = 8
Host.events["opx:watch:join"]({ code = code })
check("Ana joins from the panel", lastClient("opx:watch:you", 8) ~= nil and lastClient("opx:watch:you", 8).args[1].code == code)
Host.events["opx:watch:stop"]()
check("only the host ends it for everyone", lastClient("opx:watch:result", 8).args[1].ok == false)
Host.clientEvents = {}
_G.source = 7
Host.events["opx:watch:stop"]()
local release = lastClient("opx:watch:releasetv", 7)
check("the host ends it: both are told, and the set is handed back",
    release ~= nil and release.args[1].tvId == 4 and lastClient("opx:watch:you", 8).args[1].code == false)
check("the server's clock thread is running", #Host.threads == 1)

-- ======================================================= client main =======
section("the client, booted against a stand-in platform")
local C = { events = {}, server = {}, page = nil, clipboard = nil, focus = nil, pageEvents = {}, sent = {} }
_G.source = nil
_G.GetGameTimer = function() return clock end
_G.Open77 = {
    character = { position = function() return 10.0, 0.0, 0.0 end },
    session = { playerId = function() return 7 end },
    clipboard = { setText = function(text) C.clipboard = text; return true end },
}
_G.WebUI = { create = function(spec)
    C.page = { spec = spec, on = function(self, name, fn) C.pageEvents[name] = fn end,
        send = function(self, name, payload) C.sent[#C.sent + 1] = { name = name, payload = payload } end,
        setFocus = function(self, a, b) C.focus = a; return true end, destroy = function() end }
    return C.page
end }
_G.RegisterNetEvent = function(name, fn) C.events[name] = fn end
_G.AddEventHandler = function(name, fn) C.events[name] = fn end
_G.TriggerServerEvent = function(name, ...) C.server[#C.server + 1] = { name = name, args = { ... } } end
_G.CreateThread = function() end
_G.print = function() end
dofile("client/main.lua")
C.events.onClientResourceStart("opx_watchparty")
_G.print = realPrint
local function lastServer(name)
    for i = #C.server, 1, -1 do if C.server[i].name == name then return C.server[i] end end
end
check("on start it says hello and asks open77_media for its sets",
    lastServer("opx:watch:hello") ~= nil and lastServer("open77:media:ready") ~= nil and C.page.spec.entry == "web/panel.html")
C.events["open77:media:snapshot"]({
    { id = 4, position = { x = 11.0, y = 0.0, z = 0.0 }, reach = 2.0, label = "TV", url = "" },
    { id = 5, position = { x = 40.0, y = 0.0, z = 0.0 }, reach = 30.0, label = "Cinema", url = "" },
})
C.events["opx:watch:pick"]({ netflixId = "80057281", title = "Dark" })
local startEv = lastServer("opx:watch:start")
check("a pick goes to the nearest set the player is within reach of",
    startEv ~= nil and startEv.args[1].tvId == 4 and startEv.args[1].netflixId == "80057281")
C.events["opx:watch:bindtv"]({ tvId = 4, url = "https://example.test/v1/tv/ABC234" })
local ctl = lastServer("open77:media:control")
check("the set is pointed at the party's page through open77_media's own control",
    ctl ~= nil and ctl.args[1] == "url" and ctl.args[2].id == 4 and ctl.args[2].url == "https://example.test/v1/tv/ABC234")
C.events["opx:watch:you"]({ code = "ABC234", key = "k3ykeyk3ykey" })
C.events["opx:watch:state"]({ parties = { { code = "ABC234", tvId = 4, netflixId = "80057281", title = "Dark",
    playing = true, positionMs = 95000, members = 1, browsers = 0, inSync = 0, host = 7, hostName = "Matt" } } })
clock = clock + 2000
C.events["opx:watch:copylink"]()
check("Copy browser link puts the title, the time and the player's own code and key on the clipboard",
    C.clipboard == "https://www.netflix.com/watch/80057281?t=97#opxwatch=ABC234.k3ykeyk3ykey", C.clipboard)
C.events["opx:watch:panel"](true)
local last = C.sent[#C.sent]
check("/watch opens the panel with the keyboard, on the party and as its host",
    C.focus == true and last.name == "watch:state" and last.payload.open == true and last.payload.party.code == "ABC234"
        and last.payload.isHost == true)
C.pageEvents["watch:action"]({ action = "nudge", deltaMs = -10000 })
check("the panel's buttons are requests to the server",
    lastServer("opx:watch:control").args[1].action == "nudge" and lastServer("opx:watch:control").args[1].deltaMs == -10000)
C.events["open77:pauseKey"]()
check("Escape closes it and gives the keyboard back", C.focus == false and C.sent[#C.sent].payload.open == false)
C.events["opx:watch:releasetv"]({ tvId = 4, url = "https://example.test/v1/tv/OTHER2" })
check("a set showing something else is left alone when a party ends",
    lastServer("open77:media:control").args[2].url == "https://example.test/v1/tv/ABC234")

print(("\n%d checks, %d failed"):format(checks, failures))
os.exit(failures == 0 and 0 or 1)
