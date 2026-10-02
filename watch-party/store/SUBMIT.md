# Putting Open77 Watch Party on the Edge and Chrome stores

You do this part: both stores need your own developer account. Everything to
paste and upload is in this folder; `LISTING.md` has every field.

Before you start, the privacy policy must be online. It is
`watch-party/PRIVACY.md` in the open77-tv repository, at
https://github.com/nobody71004/open77-tv/blob/main/watch-party/PRIVACY.md once
that push is done. Check the link opens in a private window.

## Microsoft Edge Add-ons (free)

1. Go to https://partner.microsoft.com/dashboard/microsoftedge and sign in with
   a Microsoft account. The first time, register as an individual developer
   (free; it asks for your name, country and an email for store messages).
2. **Create new extension** -> upload `Open77-Watch-Party-1.0.1.zip`.
3. **Availability**: Public, all markets (or Hidden: only people with the link).
4. **Properties**: category Entertainment; privacy policy URL (above); website
   and support URLs from `LISTING.md`. "Does it access, collect or transmit
   personal information?" Yes - describe as in `LISTING.md`, Data usage.
5. **Store listings** -> English: description, short description, store logo
   `store-logo-300x300.png`, the four `screenshot-*` images, optionally the two
   promo tiles.
6. **Submit**: paste "Notes for the reviewers" from `LISTING.md` into *Notes for
   certification*. Review usually takes a few business days; you get an email.

## Chrome Web Store (one-off 5 US$ registration)

1. Go to https://chrome.google.com/webstore/devconsole, sign in with a Google
   account, accept the agreement and pay the registration fee. Verify the
   contact email it asks for.
2. **New item** -> upload `Open77-Watch-Party-1.0.1.zip`.
3. **Store listing**: description, category Entertainment, language English,
   the four screenshots, small promo tile `promo-small-440x280.png` (required),
   marquee `promo-marquee-1400x560.png` (optional), homepage and support URLs.
4. **Privacy practices**: single purpose, the three permission justifications,
   "No" to remote code, the data usage ticks and the three certifications, the
   privacy policy URL - all in `LISTING.md`.
5. **Distribution**: Public (or Unlisted). Free. All regions.
6. **Submit for review**. A first review can take a few days to a few weeks.

## After they are approved

- Each store gives the extension a page and an id. Send me both links: I put
  them in the watch party README in open77-tv, the in-game panel's help text,
  the TV page, and the launcher, and drop the "Load unpacked" steps there.
- The stores update installed copies by themselves. A new version: raise
  `version` in `manifest.json`, zip the folder (manifest at the top of the
  zip), upload it as a new package in both dashboards.

## What changed for the stores (1.0.1)

- The popup's heading used a red "N" like Netflix's own logo; stores refuse an
  extension that looks like it comes from the brand it works with. It is now
  the extension's own icon and "Open77 Watch party". The same in the in-game
  panel and on the TV page (opx_watchparty 1.0.1).
- The manifest description is 125 characters (the Chrome store allows 132) and
  it has a homepage link.
- Nothing about how it works changed: the end-to-end test (a real Chromium, the
  extension, the server's own rules) passes all 12 checks, and the server's
  tests all 75.
