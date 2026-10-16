-- opx_screenmirror -- the remote screen capture on a television.
--
-- WHAT IT IS. The screen-mirror's viewer link, held on the server and handed to
-- open77_media when a capture cinema is put up, so a "Capture cinema, 100/150 ft"
-- shows a live, view-only picture of a remote PC's screen (a sender on that PC
-- publishes frames to the screen-mirror relay; this just names the viewer page).
--
-- WHAT IT IS NOT. It carries no DRM and captures nothing here: it is only the
-- link. The remote PC's sender captures its normal desktop output, so protected
-- video (Netflix etc.) comes through black by design. View-only: no input travels
-- back to the remote PC.
--
-- USING IT. Spawn "Capture cinema, 100 ft" or "150 ft" from the TV menu (or
-- `media.spawn cinema.100ft.capture` / `cinema.150ft.capture`). The link (with
-- its channel + token) lives in server/config.lua, written on the server at
-- deploy and never shipped to clients as a file -- though a client whose screen
-- shows it holds the link, which is what the token is scoped for.
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
