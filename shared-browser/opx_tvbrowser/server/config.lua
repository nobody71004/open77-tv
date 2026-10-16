-- opx_tvbrowser -- server/config.lua
--
-- Written on the server at deploy from the shared browser's own settings
-- (/opt/open77-tvbrowser/tv-url.secret, root-only). This copy is the placeholder
-- a bundle carries: with no link, /browser says the browser is not set up here.
OpxTvBrowserConfig = {
    url = nil,
    cinemaRecord = "cinema.150ft.browser",
    cinema100Record = "cinema.100ft.browser",

    -- WHO MAY PUT THE SHARED BROWSER ON A SCREEN, and who may be handed its link.
    --
    -- The link CARRIES the browser's viewer password: it is the whole of that
    -- session's credentials, and anyone who has it can open the room's desktop from
    -- their own PC and use whatever is signed in there. `/browser` hands that link
    -- out, so it is admin-only -- the same ACE open77_media's screen-lock and its
    -- `adminOnlyControl` use, `command.media.lock`, which an admin has anyway. A
    -- player who is not an admin is told so instead of being given the link.
    --
    -- The television's own path is unaffected: the browser cinemas (`cinema.*.browser`)
    -- get the link from this resource's `link` export, which answers open77_media and
    -- nobody else. The screens still work; what is gone is a chat command that handed
    -- the credentials to whoever typed it.
    adminAce = "command.media.lock",
}
