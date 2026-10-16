-- opx_watchparty -- Netflix watch parties, in sync, on everyone's own account.
--
-- WHAT IT IS. A party is a Netflix title and one shared playhead. The film
-- plays in each viewer's OWN browser (Edge or Chrome), signed in to their OWN
-- Netflix account, through the "Open77 Watch Party" browser extension, which
-- keeps that browser's player where the party is: play, pause and seek follow
-- the party, and a member's own play/pause/seek in the browser moves the party.
-- In the game, the television the party is on shows what is playing, where the
-- film is, and how many browsers are in step; the /watch panel is the remote.
--
-- WHAT IT IS NOT, ON PURPOSE. Nothing of the film is captured, re-encoded or
-- relayed onto a game surface or to another player. Netflix's video is
-- protected, and this resource never works around that: the only things a party
-- shares are the title number and where the film is.
--
-- HOW BROWSERS REACH IT. The server's own inbound HTTP (`Open77.http.listen`,
-- `httpHandlers` in server.jsonc, loopback) serves `/opx_watchparty/v1/...`; the
-- web server in front of it publishes that under HTTPS (the convar
-- `opx_watchparty_public_base`, default
-- https://xbuniverse.duckdns.org/opx-watch-xb-staging). Without the listener
-- the parties still run in game (the panel and the chat commands), but the
-- television cannot show its page (it is served from there) and browsers cannot
-- follow: the start banner says which.
--
-- USING IT. /watch opens the panel. At a TV: paste a Netflix link (open the film
-- in your browser, copy the address) and Start; everyone else at the TV presses
-- Join. Each viewer presses "Copy browser link" and opens it in their browser
-- with the extension installed. Chat: /watch netflix <link> [title], /watch
-- play | pause | seek 1:02:03 | +30 | -10 | join <code> | leave | stop | link | status.
--
-- ITS OWN CINEMA. /watch cinema puts the player's party on a 150 ft watch-party
-- panel (100 ft with "100", and a Cinema screen button in the panel) -- the
-- `cinema.150ft.watchparty` / `cinema.100ft.watchparty` records in open77_media,
-- which show the party's screen through `linkFrom = "opx_watchparty"`.
--
-- SIGNING IN, SECURELY. Nobody signs in anywhere shared: each viewer signs in to
-- their OWN Netflix account in their OWN browser on their OWN machine, and the
-- extension only keeps that browser's player where the party is. What crosses to
-- the server is the title number, the playhead and the member's own key -- never
-- credentials, cookies, history or anything of the film. The key travels in the
-- link's fragment (never sent over the wire), is wiped from the address bar on
-- arrival, dies when the membership ends, and moves the party at most 8 times
-- in 10 s. Party codes and keys come from a generator seeded at resource start
-- (see server/main.lua): the sandbox withholds os/crypto, so this is prediction
-- resistance for short-lived single-scope secrets, documented as not-a-CSPRNG.
resource "opx_watchparty"
version "1.1.0"
open77_version ">=0.0.1"
auto_start true

shared_script "shared/playhead.lua"
shared_script "shared/netflix.lua"

server_script "server/party.lua"
server_script "server/http.lua"
server_script "server/tvpage.lua"
server_script "server/main.lua"

client_script "client/main.lua"

web_ui_page "web/panel.html"
web_ui_auto_create false
web_files { "web/**" }

-- Carried, not executed (run with `lua tests/run.lua` from this folder).
files { "tests/run.lua", "tests/json.lua", "tests/decide_vectors.lua", "tests/export_vectors.lua", "tests/e2e_bridge.lua" }

permissions {
    -- The panel's requests and the party state pushed back down.
    "network.events",
    -- The browsers' side: `Open77.http.listen` on the server's httpHandlers listener.
    "http.serve",
    -- "Copy browser link": the link goes to the player's own clipboard.
    "clipboard.write",
}
