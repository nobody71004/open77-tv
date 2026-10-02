# Watch parties: Netflix in step, on everyone's own account

A television in Open77 cannot play Netflix: its video is protected, and a game
surface has no way to decrypt it (see "What actually plays" in the top README).
A watch party does the honest thing instead. Everyone plays the film in their own
browser, signed in to their own Netflix account, and the party keeps those
browsers in step: play, pause and seek from the game, from chat or from any
member's own player, and everyone follows within about two seconds. The
television the party is on shows what is playing, where the film is and how
many browsers are in step. Nothing of the film is captured, re-encoded or relayed
to the game or to another player; the party shares the title and the time.

```
opx_watchparty/   the server resource: parties, the /watch panel, the page a
                  television shows, and the small HTTP API browsers use
                  (opx_watchparty/README.md)
extension/        the Edge / Chrome extension that keeps a viewer's Netflix tab
                  in step (extension/README.md)
store/            the Edge Add-ons and Chrome Web Store listing: package, text,
                  permission justifications, images (store/SUBMIT.md)
PRIVACY.md        the extension's privacy policy
```

## Tests

```bash
cd opx_watchparty && lua5.4 tests/run.lua         # 75 checks: playhead, links, parties, HTTP, both main.lua files
cd extension && node tests/sync-core.test.js      # the follower's decisions, against the server's own cases
cd extension && node tests/e2e.mjs                # a real Chromium with the extension, a stand-in player
                                                  # page and the server's Lua rules (needs Playwright, lua5.4)
```

`store/tools/shots.mjs` makes the store images from the same pieces:
`node store/tools/shots.mjs extension opx_watchparty <out dir>`.

## Where it runs

The resource is on XBUNIVERSE staging, behind the web server at
`https://xbuniverse.duckdns.org/opx-watch-xb-staging/`, which is the address the
extension talks to by default (its popup can be pointed at another path on the
same host). The extension is ready for the stores (1.0.1); until it is listed,
it is installed by hand (extension/README.md).
