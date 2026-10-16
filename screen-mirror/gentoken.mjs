// open77 screen-mirror token generator.
//
// The relay's shared secret (MIRROR_TOKEN) is what stops a stranger on the public
// VPS from watching or hijacking your channel. This generates a fresh
// cryptographically-random token and prints everything that has to change when
// you rotate it -- the relay's env, the game's capture link (opx_screenmirror's
// server/config.lua), and the sender/receiver URLs.
//
//   node gentoken.mjs                 # generate + show the apply steps
//   node gentoken.mjs --bytes 24      # longer token (default 16 bytes -> 32 hex)
//   node gentoken.mjs --quiet         # just the token, for scripting
//
// It never touches the server by itself. Rotate deliberately: the capture link
// on the televisions carries the token, so a new one has to be pushed to the
// game's opx_screenmirror config too or the capture cinema stops showing it.

import { randomBytes } from "node:crypto";

const argv = process.argv.slice(2);
const quiet = argv.includes("--quiet");
const bytesIdx = argv.indexOf("--bytes");
const bytes = bytesIdx >= 0 ? Math.max(8, Math.min(64, Number(argv[bytesIdx + 1]) || 16)) : 16;

const token = randomBytes(bytes).toString("hex");

if (quiet) {
  process.stdout.write(token + "\n");
  process.exit(0);
}

const channel = process.env.MIRROR_CHANNEL || "WORK";
const host = process.env.MIRROR_HOST || "xbuniverse.duckdns.org";

console.log(`# open77 screen-mirror token`);
console.log();
console.log(`TOKEN=${token}`);
console.log(`CHANNEL=${channel}`);
console.log();
console.log(`# 1. Sender (open this on the PC whose screen you mirror):`);
console.log(`https://${host}/screen-mirror/?channel=${channel}&token=${token}`);
console.log();
console.log(`# 2. Receiver (what a television frames -- the capture link):`);
console.log(`https://${host}/screen-mirror/receiver?channel=${channel}&token=${token}`);
console.log();
console.log(`# 3. Apply to the relay (its shared secret):`);
console.log(`#    edit the Environment=MIRROR_TOKEN=... line in`);
console.log(`#    /etc/systemd/system/open77-mirror.service, then:`);
console.log(`systemctl daemon-reload && systemctl restart open77-mirror`);
console.log();
console.log(`# 4. Apply to the game's capture link (so the televisions carry the new`);
console.log(`#    token): edit the url in`);
console.log(`#    /opt/open77-xbuniverse{,-staging}/resources/opx_screenmirror/server/config.lua`);
console.log(`#    to the receiver URL above (keep the trailing #open77-mirror off; the`);
console.log(`#    resource adds it), then restart the game servers.`);
console.log();
console.log(`# Keep this token secret. It is the only thing standing between your`);
console.log(`# channel and anyone on the internet watching or publishing to it.`);
