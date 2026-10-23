-- opx_screenmirror -- the remote screen capture on a television.
--
-- WHAT IT IS. The screen-mirror's viewer link, held on the server. The room
-- service on this host reads it and puts it behind the Mirror app on the room's
-- home screen, so the television shows a live, view-only picture of a remote PC's
-- screen (a sender on that PC publishes frames to the screen-mirror relay; this
-- just names the viewer page).
--
-- WHAT IT IS NOT. It carries no DRM and captures nothing here: it is only the
-- link. The remote PC's sender captures its normal desktop output, so protected
-- video (Netflix etc.) comes through black by design. View-only: no input travels
-- back to the remote PC.
--
-- USING IT. In game, the room's own screen: spawn one of the two open77-TV-ROOM
-- panels and press the Mirror app (or, where a record still names this resource,
-- the `link` export answers open77_media as before). The link (with its channel
-- + token) lives in server/config.lua, written on the server at deploy and never
-- shipped to clients as a file -- though the room's Chromium holds it, which is
-- what the token is scoped for.
resource "opx_screenmirror"
version "1.0.0"
open77_version ">=0.0.1"
auto_start true

server_script "server/config.lua"
server_script "server/main.lua"

client_script "client/main.lua"

permissions {
    -- The link hand-off to open77_media.
    "network.events",
}
