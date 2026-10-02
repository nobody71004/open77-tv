# Open77 Watch Party -- the browser extension (Edge or Chrome), 1.0.1

Keeps **your own Netflix** in step with an Open77 watch party: when the party plays,
pauses or jumps, your browser does too; when you play, pause or seek in your browser
(with your own key), the party follows you. The film plays on **your** Netflix
account in **your** browser. Nothing of the film is captured or shared -- only the
title number and the time go to the party server.

## Install

From the store, once it is there: Microsoft Edge Add-ons and the Chrome Web Store
(submitted as "Open77 Watch Party"; links follow when they are approved). Until
then, by hand:

### By hand (once, about a minute)

1. Unzip `Open77-Watch-Party-extension.zip` somewhere that stays (for example
   `Documents\Open77 Watch Party`).
2. Edge: open `edge://extensions` -- Chrome: open `chrome://extensions`.
3. Turn on **Developer mode** (Edge: left side; Chrome: top right).
4. **Load unpacked** -> pick the unzipped folder (the one with `manifest.json`).
5. Optional: pin it (puzzle icon -> pin) to see its status.

Edge/Chrome 111 or newer. Your Netflix plan and sign-in are yours as usual.

## Use

1. In game, at the TV: `/watch` -> **Join** (or **Start** with a Netflix link).
2. Press **Copy browser link** and paste it into the browser's address bar.
3. Netflix opens the party's title at the party's moment, and a small badge at the
   top right says `Party ABC123 - in step`. Click it for details or **Leave party**.

The link holds your own key (it lets your browser move the party). It is wiped from
the address bar as soon as the page opens. Do not share it; others copy their own.
Without a key (code only, typed in the popup) the browser only follows.

## Files

`manifest.json`, `background.js` (the only network access: the watch party server,
`https://xbuniverse.duckdns.org/`), `content.js` (the sync loop and badge),
`page.js` (Netflix's own player controls: where the film is, play, pause, seek -- the
same calls its buttons make), `sync-core.js` (the decisions, shared with the
server's rules), `popup.*`, `icons/`.

Tests: `node tests/sync-core.test.js` (the decisions, against the server's own
cases) and `node tests/e2e.mjs <opx_watchparty folder>` (a real Chromium with this
extension, a stand-in Netflix player and the server's Lua rules: 12 checks).

Privacy: [PRIVACY.md](../PRIVACY.md). Store listing and images: [store/](../store/).
