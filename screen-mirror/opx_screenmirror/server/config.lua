-- opx_screenmirror -- server/config.lua
--
-- Written on the server at deploy (root-only) with the screen-mirror's viewer
-- link. This copy is the placeholder a bundle carries: with no link, the room's
-- Mirror app says the mirror is not set up here.
OpxScreenMirrorConfig = {
    -- The relay's viewer page for the channel the remote PC publishes to. A link
    -- ending in the mirror mark, or one without it (the mark is added for you).
    --
    -- The room service on this host reads this file directly: the Mirror app on
    -- the room's home screen opens exactly this page, so the channel and token
    -- live in one place -- here -- and never in the room's HTML. There are no
    -- capture cinema records any more: the room is the screen (open77_media's
    -- `cinema.100ft` / `cinema.150ft` show the room's own page).
    url = nil,
}
