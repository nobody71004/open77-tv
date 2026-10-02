-- opx_tvbrowser -- the shared browser on a television.
--
-- WHAT IT IS. A real Chromium runs on the server (neko, in a container beside
-- the game server) and its picture and sound are streamed to a television over
-- WebRTC, as VP8 and Opus, which the game's own browser plays. Everyone at that
-- television sees the same browser; whoever presses F8 at the screen drives it
-- with the mouse and keyboard. Sites the game's browser cannot play (H.264
-- players, Twitch) play there.
--
-- WHAT IT IS NOT. It has no DRM module (the image's Widevine is removed and
-- component updates are off), so protected video does not play in it: nothing
-- protected is ever captured or streamed. It is shared: a site signed in to on
-- it is seen by everyone watching, and recreating the container wipes it.
--
-- USING IT. /browser at a television puts the browser on it; /browser cinema puts
-- up a 150 ft cinema in front of you showing it; /browser off takes it off the
-- set in front of you. The link (with the viewer password) lives in
-- server/config.lua, written on the server at deploy and never shipped to clients
-- as a file -- though a client whose television shows it holds the link, which is
-- what the viewer password is for.
resource "opx_tvbrowser"
version "1.0.0"
open77_version ">=0.0.1"
auto_start true

shared_script "shared/link.lua"

server_script "server/config.lua"
server_script "server/main.lua"

client_script "client/main.lua"

-- Carried, not executed (run with `lua tests/run.lua` from this folder).
files { "tests/run.lua" }

permissions {
    -- The commands' answers and the requests to open77_media.
    "network.events",
}
