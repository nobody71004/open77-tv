-- =============================================================================
-- open77_media -- server/config.lua
-- =============================================================================
-- The operator-editable half of the television feature: what the ad blocklist
-- starts as, and where an operator's changes are kept.
--
-- -----------------------------------------------------------------------------
-- THE SEED, NOT A LAYER
-- -----------------------------------------------------------------------------
-- The live list has exactly one owner at a time:
--
--   * `data/adblock.json` once anything has been changed -- by `media.adblock.*`
--     or by hand -- because that is where a server that has been told something
--     keeps it;
--   * the `hosts` / `tokens` keys below until then, which is what a fresh
--     install reads so a network can ship a default list to a whole fleet by
--     editing this file in the resource.
--
-- It is deliberately NOT a second layer added on top of the stored list. Two
-- lists that both apply is a state where removing a rule appears to do nothing
-- because the rule is still in the file you forgot about, and an operator
-- debugging that has no way to see which of the two is winning without reading
-- the resource's own source.
--
-- So: to change a live server, use the commands, or edit `data/adblock.json` and
-- run `media.adblock.reload`. To change what a NEW server starts with, edit this
-- file. `media.adblock.reload` re-reads the store, never this file, which is why
-- the two never argue.
--
-- -----------------------------------------------------------------------------
-- WHAT THE RULES MAY BE
-- -----------------------------------------------------------------------------
--   * a HOST rule covers that host and its subdomains: `ads.example.com` refuses
--     `ads.example.com` and `cdn.ads.example.com`, and nothing that merely ends
--     with those characters (`notads.example.com` and
--     `ads.example.com.evil.test` both still load).
--   * a TOKEN rule matches anywhere inside a host, which is how a campaign that
--     rotates through fresh domains faster than a list can be written is caught:
--     the token `popads` refuses `popads-delivery-7.anything.test`.
--
-- A rule is refused -- with a reason, at the console, when you type it -- if it
-- is a registry rather than a company (`com`, `co.uk`), if it contains anything
-- other than letters, digits, dots and inner hyphens, or if a token would match
-- the whole internet. `server/adblock.lua` is the grammar and the refusals; it is
-- the file to read before arguing with one.
--
-- -----------------------------------------------------------------------------
-- AND WHAT A CLIENT DOES WITH THEM
-- -----------------------------------------------------------------------------
-- The rules are pushed to each client, which hands them to the process that
-- enforces them. That layer only ever ADDS to the blocklist compiled into the
-- client -- nothing here can unblock something the build refuses, which is the
-- whole reason a server is allowed to push a list at all. A rule this file
-- accepts and that host refuses comes back on the receipt, named, with the
-- host's reason: `media.adblock.status` shows it.
-- =============================================================================

MediaServerConfig = {
    -- WHO MAY DRIVE A TELEVISION.
    --
    -- True (the default) is admin-only TV control: every panel action -- the link,
    -- the volume, the mute, the pause, the curtain, the placement, and removing a
    -- set -- needs the same ACE the screen-lock does, `command.media.lock`. That is
    -- the ACE an admin is given anyway, so granting it is the whole story; a player
    -- without it sees a panel that reads as locked and one line saying why, and the
    -- refusal is enforced on the SERVER, in the one handler every control passes
    -- through (`open77:media:control`), not in the page.
    --
    -- False opens the panel back up to everyone: any player may drive any set that
    -- no admin has locked with `media.lock`. Both answers are real ones -- one
    -- server's television is its admin's, another's is a toy for whoever finds it --
    -- and the screen-lock still gates a locked set either way. The console is always
    -- an admin, so `media.*` commands keep working with this on.
    --
    -- The value travels to each client with its snapshot, so a client can grey the
    -- controls it may not use; only the server can read a player's ACEs.
    adminOnlyControl = true,

    -- The layer as a whole. Off means nothing is read or pushed from here; the
    -- compiled list still protects every client, because that half is not this
    -- server's to turn off.
    enabled = true,

    -- Relative to the resource's own `data/` directory.
    dataFile = "adblock.json",

    -- Where a venue screen's SAVED placement is kept, relative to `data/`.
    --
    -- `MediaVenueSets` below is the survey; this file is what an operator last
    -- said about where one of those screens actually belongs. The panel's SAVE
    -- THIS SPOT writes it; the next start reads it and spawns the venue there
    -- instead of at the coordinates below. Deleting the file is how an operator
    -- goes back to the survey -- there is no second copy of the saved numbers in
    -- this file, on purpose (see the ad-blocklist note above: two owners is how
    -- removing a rule appears to do nothing).
    venueDataFile = "venue-sets.json",

    -- The seed. One entry per line, quoted, exactly as an operator would type it
    -- at `media.adblock.add`. Empty by default: a server that has not been told
    -- anything should not be blocking anything, and every entry here is one an
    -- operator has to be able to explain.
    hosts = {
        -- "ads.example.com",
        -- "tracker.example.net",
    },

    -- The seed's token half. A token is matched anywhere inside a host, so these
    -- are the ones worth writing down: a network's own name, not a domain it
    -- happens to be using this month.
    tokens = {
        -- "popads",
    },
}

-- =============================================================================
-- SCREENS THAT ARE PART OF THE WORLD
-- =============================================================================
-- Every other television in this resource is a SESSION: a player opens the panel
-- and puts one down, and it is gone at the next restart. That is right for a TV
-- somebody rolls into a garage and wrong for a place -- a venue's screen is
-- scenery, not an action, and a venue whose screen has to be re-placed after
-- every restart is a venue with a daily chore.
--
-- VENUE_SETS are spawned once, at start, at the coordinates written here. They
-- are ordinary sets afterwards: `media.list` numbers them, the panel drives them,
-- `media.place` nudges them, and a watch party claims one exactly as it claims a
-- hand-placed screen. Nothing else changes, which is why this is a config list and
-- not a second kind of television.
--
-- HOW TO CAPTURE ONE. Stand where the screen should go, FACE THE WAY THE PICTURE
-- SHOULD LOOK, and copy the position and heading the panel shows (or run
-- `/opx.admin.self.pos`); a set faces along its own +Y, so a screen meant to be
-- looked at from the south takes a yaw pointing north. The menu spot of a venue's
-- garage in `config/garages.lua` is the audience's side, so the panel's yaw is
-- that heading PLUS 180 -- a screen at the arrival point faced the wrong way is a
-- black rectangle the audience has to walk behind.
--
-- A set whose record is `cinema.*.watchparty` shows the watch party's page: leave
-- `url` out, and the party that is started on it supplies the picture.
--
-- Any argument that is not a finite number, or a record the catalogue does not
-- carry, is refused with a line at start -- the same rule every other config here
-- follows: a typo costs a boot line, never a screen that silently is not there.
MediaVenueSets = {
    -- THE WATCH-PARTY VENUE, on the hill south-east of the arrival point.
    --
    -- The captured spot is the AUDIENCE's: -55.30, 2008.49, 101.02, facing
    -- 141.3 (the same point `config/spawn.lua` and `config/garages.lua` hold).
    -- A 45.7 m panel centred on it would swallow the players standing there, so
    -- the hang point is derived the way the panel path derives one: the record's
    -- own hanging distance (30.56 m -- `Open77MediaPlacement.FacingDistance` for
    -- this quad) along the arrival facing, and `yaw` = that facing + 180, which
    -- is `FacingYaw` for a +Y panel. The ground at the hang point was read from
    -- the world editor (101.22 m, against 101.02 at the audience).
    --
    -- LINE IT UP FROM THE F5 PANEL, THEN SAVE IT: the panel's SAVE THIS SPOT
    -- action takes where the screen stands now (and which way it faces) and
    -- writes it to `data/venue-sets.json`, which is read at the next start in
    -- place of the numbers below. `media.move`/`media.rotate` line it up without
    -- saving; the panel is the one step that makes it permanent.
    --
    -- The numbers below are still the SURVEY: what a fresh install -- or one
    -- whose `data/venue-sets.json` was deleted -- starts from.
    {
        record = "cinema.150ft.watchparty",
        x = -74.41, y = 1984.64, z = 101.22,
        yaw = 321.3,
    },
}
