# Open77 Watch Party (browser extension) - privacy policy

Effective 1 October 2026. Applies to the "Open77 Watch Party" extension for
Microsoft Edge and Google Chrome, version 1.0.1 and later.

## What the extension is for

It keeps the Netflix player in your own browser in step with a watch party in
Open77, a multiplayer mod for Cyberpunk 2077: when the party plays, pauses or
jumps, your player does too, and when you do (with your own key), the party
follows. You watch on your own Netflix account. The extension does nothing
until you join a party.

## What it never does

- It never captures, records, copies or streams any video, audio or image of
  what you watch.
- It never reads your Netflix account, profile, viewing history, list,
  payment details or password.
- It never reads or changes any website other than Netflix's player pages, and
  it talks to no server other than the watch party server named below.
- No advertising, analytics or tracking of any kind. Nothing is sold or given to
  anyone.

## What it keeps on your computer

In the browser's extension storage (`chrome.storage.local`), on this device only:

- the party code you joined (six letters and digits) and, if your in-game link
  had one, your own party key (twelve letters and digits);
- a random viewer number made by the extension (24 hexadecimal digits), so the
  party can count browsers in step;
- the server address (always under `https://xbuniverse.duckdns.org/`) and the
  last status shown in the extension's popup.

"Leave party" deletes the code and key. Removing the extension deletes all of it.
The key is also taken out of the address bar and the browser history as soon
as the Netflix page opens.

## What it sends, and where

Only while you are in a party, about once a second, only to the watch party
server at `https://xbuniverse.duckdns.org/` (run by the XBUNIVERSE Open77
community server):

- the party code, your key if you have one, and the random viewer number;
- the Netflix title number of the page you are on (the number in its address),
  the title as the player shows it, where the player is in the film, its length,
  and whether it is playing;
- when you press play or pause or seek yourself (with a key): that action and
  the position.

The server answers with where the party is. Like any web server, it sees your IP
address while it answers; the web server in front of it may log requests (time,
address, path) to keep the service running and safe.

## How long the server keeps it

A party exists only in the game server's memory. What your browser reported is
dropped 15 seconds after your browser stops reporting; the party itself ends
when its host ends it, two minutes after its last player leaves it in game, or
when the server restarts, and is then gone. Nothing is written to a file or a
database.

## Who sees it

The players in the party see, in game and on the party's in-game television,
how many browsers are in step. Anyone who knows the party code can read the
party: its title and position, how many browsers are in step, and the in-game
names of its host and of whoever moved it last. That is by design: it is what
lets a browser follow without a key. Your key and viewer number are never shown
to anyone.

## Children

The extension is meant for players of Cyberpunk 2077, which is rated for adults.
It is not directed at children.

## Changes and contact

A change to this policy is published here, with a new date, before a version
of the extension that needs it. Questions or requests:
https://github.com/nobody71004/open77-tv/issues

Netflix is a trademark of Netflix, Inc.; Cyberpunk 2077 is a trademark of CD
PROJEKT S.A. This extension is not made, endorsed or supported by either.
