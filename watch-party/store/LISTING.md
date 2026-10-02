# Store listing - Open77 Watch Party 1.0.1

Everything the two stores ask for, ready to paste. The same text works for both;
where a store has its own field, it says so.

Package: `Open77-Watch-Party-1.0.1.zip` (this folder). Images: `images/`.

## Name

    Open77 Watch Party

## Short description

Chrome Web Store "Summary" (at most 132 characters; it is also the manifest's
description), Edge "Short description":

    Keeps your own Netflix in step with an Open77 watch party. Only the playback time is shared; nothing of the film is captured.

## Description

    Open77 Watch Party keeps your own Netflix in step with a watch party in Open77, the multiplayer mod for Cyberpunk 2077.

    HOW IT WORKS
    - In game, at a TV, start a watch party with /watch and a Netflix link, or join the one playing there.
    - Press "Copy browser link" and paste it into the address bar of Edge or Chrome. Netflix opens the party's title at the party's moment.
    - From then on, play, pause and seek follow the party within about two seconds, and your own play, pause and seek move the party for everyone. (A party code typed in the popup, without your key, only follows.)
    - A small badge on the player page says you are in step. Click it to see the party, or to leave.

    YOUR ACCOUNT, YOUR FILM
    Everyone watches on their own Netflix account, in their own browser. The extension does not capture, record, copy or stream any video, audio or picture. It shares only what a party needs: which title, where the film is, and whether it is playing.

    WHAT IT USES
    - Netflix player pages: to read where the player in your tab is and move it, with the player's own play, pause and seek.
    - xbuniverse.duckdns.org: the watch party server, the only server it talks to.
    - Storage: your party code, your key and a random viewer number, on this computer only.
    It does nothing until you join a party.

    NEEDS
    Open77 for Cyberpunk 2077 and a server that runs the watch party; a Netflix subscription; Edge or Chrome 111 or newer.

    Not made, endorsed or supported by Netflix or CD PROJEKT. Netflix is a trademark of Netflix, Inc.; Cyberpunk 2077 is a trademark of CD PROJEKT S.A.

## Category, language, links

| Field | Chrome Web Store | Edge Add-ons |
|---|---|---|
| Category | Entertainment | Entertainment |
| Language | English | English |
| Homepage / website | https://github.com/nobody71004/open77-tv/tree/main/watch-party | same |
| Support | https://github.com/nobody71004/open77-tv/issues | same |
| Privacy policy URL | https://github.com/nobody71004/open77-tv/blob/main/watch-party/PRIVACY.md | same |
| Mature content | No | - |
| Visibility | Public (or Unlisted, then only people with the link find it) | Public / Hidden |
| Price | Free | Free |

## Images

| File | Chrome Web Store | Edge Add-ons |
|---|---|---|
| `store-logo-300x300.png` | - (the 128 px icon comes from the package) | Store logo (300 x 300) |
| `screenshot-1-in-step-1280x800.png` | Screenshot 1 | Screenshot 1 |
| `screenshot-2-popup-1280x800.png` | Screenshot 2 | Screenshot 2 |
| `screenshot-3-in-game-1280x800.png` | Screenshot 3 | Screenshot 3 |
| `screenshot-4-tv-1280x800.png` | Screenshot 4 | Screenshot 4 |
| `promo-small-440x280.png` | Small promo tile (required) | Small promotional tile (optional) |
| `promo-marquee-1400x560.png` | Marquee promo tile (optional) | Large promotional tile (optional) |

The player page in screenshot 1 is a neutral stand-in, not Netflix's interface;
the badge, the popup, the in-game panel and the TV page are the real ones,
rendered by `tools/shots.mjs` from the extension and the watch party server's own
code.

## Single purpose (Chrome: "Privacy practices" tab)

    Keeps the Netflix player in the user's own browser in step with an Open77 watch party (play, pause and seek), and lets the user's own player controls move the party.

## Permission justifications (Chrome: "Privacy practices" tab)

**storage**

    Remembers, on this device only, the party code the user joined, the user's own party key from the in-game link (if any), a random viewer number the extension makes so the party can count browsers in step, the watch party server address, and the last status for the popup. "Leave party" clears the code and key.

**Host permission: https://xbuniverse.duckdns.org/***

    This is the watch party server, the only server the extension contacts. While the user is in a party, about once a second, it sends where the user's Netflix player is (title number, position, playing or paused) and reads where the party is; when the user plays, pauses or seeks with a key, it sends that action. Requests go from the background service worker, which only allows this host.

**Content scripts on https://www.netflix.com/***

    On Netflix player pages, the content script reads where the player in that tab is (position, playing or paused, length and title number) and moves it with the player's own play, pause and seek, so the tab follows the party; it shows a small status badge. A second script in the page's own context calls the Netflix player's existing controls, the same ones its buttons use. Neither changes or sends anything until the user has joined a party. Nothing on the page is captured or sent anywhere else.

**Remote code**

    No. All JavaScript is in the package; nothing is downloaded and run.

## Data usage (Chrome: "Privacy practices" tab; Edge: privacy fields)

What the extension collects - tick only:

- [x] **Website content** - the Netflix title number and the title as the player
  shows it, the playback position and whether it is playing, sent to the watch
  party server while the user is in a party.
- [ ] Personally identifiable information - no
- [ ] Health information - no
- [ ] Financial and payment information - no
- [ ] Authentication information - no (the party key is a per-party token from
  the game, not an account credential)
- [ ] Personal communications - no
- [ ] Location - no
- [ ] Web history - no (only the page the user is on, and only while in a party)
- [ ] User activity - no (no clicks, keystrokes, mouse or scrolling)

Certify all three:

- [x] I do not sell or transfer user data to third parties, outside of the
  approved use cases.
- [x] I do not use or transfer user data for purposes that are unrelated to my
  item's single purpose.
- [x] I do not use or transfer user data to determine creditworthiness or for
  lending purposes.

## Notes for the reviewers

Chrome: "Test instructions" on the Privacy / Distribution tab, where offered.
Edge: "Notes for certification".

    This extension works together with Open77, a multiplayer mod for the PC game Cyberpunk 2077: players at an in-game TV start a "watch party" and each one opens the party's Netflix title in their own browser with a link the game gives them (https://www.netflix.com/watch/<title>#opxwatch=<CODE>.<key>). The extension then keeps that tab's Netflix player at the party's position, play/pause state and title, and sends the viewer's own play/pause/seek back to the party.

    Without a party the extension is idle: it makes no network requests and changes nothing on Netflix pages. The popup lets you type a party code; codes come from the game.

    It needs a Netflix subscription to play anything. It never captures, records or streams the video: it only reads and sets the player's position through the player's own controls.

    The only server it talks to is the watch party server at https://xbuniverse.duckdns.org/ (path /opx-watch-xb-staging). Source code and tests: https://github.com/nobody71004/open77-tv/tree/main/watch-party
