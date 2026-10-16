#!/usr/bin/env node
// open77 screen-mirror token generator.
//
// The relay authenticates a join in two ways, both keyed off the same secret
// (the MIRROR_TOKEN env the relay runs with):
//   * plain shared secret:  a client sends `?token=<MIRROR_TOKEN>`
//   * signed scoped token:  <channel>.<role>.<expiry>.<b64url-hmac>
//
// This tool mints those. A signed token is bound to one channel + role and can
// expire, so a leaked link stops working on its own and each sender can be told
// apart (the watchdog can then say *which* sender is flapping auth_failed).
//
// Usage:
//   node token.mjs secret [--length 24]
//       Print a fresh random shared secret (set it as MIRROR_TOKEN on the relay).
//
//   node token.mjs mint [--channel WORK] [--role sender|receiver|*] [--hours N]
//                       [--secret S] [--base https://host/screen-mirror/]
//       Mint a signed token and print the ready-to-use sender + watch links.
//       The secret comes from --secret or the MIRROR_TOKEN env.
//
//   node token.mjs validate <token> [--channel WORK] [--role sender] [--secret S]
//       Check a token and report whether the relay would accept it.
//
// The hmac is HMAC-SHA256(secret, "<channel>.<role>.<expiry>") in base64url, so
// the relay can verify it without storing anything per token.

import { createHmac, randomBytes, timingSafeEqual } from "node:crypto";

const args = process.argv.slice(2);
const cmd = args[0];

function opt(name, def) {
  const i = args.indexOf("--" + name);
  return i >= 0 && args[i + 1] !== undefined ? args[i + 1] : def;
}
function sanitize(ch) {
  return String(ch).replace(/[^a-z0-9_-]/gi, "").slice(0, 64) || "default";
}
function signToken(secret, ch, role, exp) {
  return createHmac("sha256", secret).update(`${ch}.${role}.${exp}`).digest("base64url");
}
function mint(secret, ch, role, exp) {
  return `${ch}.${role}.${exp}.${signToken(secret, ch, role, exp)}`;
}
function validate(secret, token, ch, role) {
  const parts = String(token).split(".");
  if (parts.length !== 4) return { ok: false, why: "not a signed token (expected 4 dot-separated parts)" };
  const [tch, trole, exp, sig] = parts;
  if (tch !== ch) return { ok: false, why: `token is for channel "${tch}", not "${ch}"` };
  if (trole !== "*" && trole !== role) return { ok: false, why: `token role is "${trole}", not "${role}"` };
  const expiry = Number(exp) || 0;
  if (expiry && Date.now() / 1000 > expiry) return { ok: false, why: "token expired at " + new Date(expiry * 1000).toISOString() };
  const expect = signToken(secret, tch, trole, exp);
  const a = Buffer.from(expect), b = Buffer.from(String(sig));
  const ok = a.length === b.length && timingSafeEqual(a, b);
  return ok ? { ok: true, expires: expiry ? new Date(expiry * 1000).toISOString() : "never" }
            : { ok: false, why: "signature does not match the secret" };
}

const secret = opt("secret", process.env.MIRROR_TOKEN || "");

if (cmd === "secret") {
  const n = Number(opt("length", "24"));
  console.log(randomBytes(n).toString("base64url"));
}
else if (cmd === "mint") {
  const ch = sanitize(opt("channel", "WORK"));
  const role = opt("role", "*");
  if (!["sender", "receiver", "*"].includes(role)) { console.error("--role must be sender, receiver, or *"); process.exit(1); }
  const hours = Number(opt("hours", "0"));
  const exp = hours > 0 ? Math.floor(Date.now() / 1000 + hours * 3600) : 0;
  if (!secret) { console.error("no secret: set MIRROR_TOKEN env or pass --secret"); process.exit(1); }
  const token = mint(secret, ch, role, exp);
  const base = opt("base", "https://xbuniverse.duckdns.org/screen-mirror/");
  console.log("token   : " + token);
  console.log("expires : " + (exp ? new Date(exp * 1000).toISOString() : "never"));
  console.log("sender  : " + base + "?channel=" + encodeURIComponent(ch) + "&token=" + encodeURIComponent(token));
  console.log("watch   : " + base + "receiver?channel=" + encodeURIComponent(ch) + "&token=" + encodeURIComponent(token));
}
else if (cmd === "validate") {
  const token = args[1];
  const ch = sanitize(opt("channel", "WORK"));
  const role = opt("role", "sender");
  if (!token) { console.error("usage: node token.mjs validate <token> [--channel CH] [--role R]"); process.exit(1); }
  if (!secret) { console.error("no secret: set MIRROR_TOKEN env or pass --secret"); process.exit(1); }
  const r = validate(secret, token, ch, role);
  if (r.ok) { console.log("ACCEPTED for " + ch + "/" + role + " (expires: " + r.expires + ")"); }
  else { console.log("REJECTED: " + r.why); process.exitCode = 1; }
}
else {
  console.log("usage:");
  console.log("  node token.mjs secret [--length 24]");
  console.log("  node token.mjs mint [--channel WORK] [--role sender|receiver|*] [--hours N] [--secret S] [--base URL]");
  console.log("  node token.mjs validate <token> [--channel WORK] [--role sender] [--secret S]");
  process.exit(cmd ? 1 : 0);
}
