// node tests/sync-core.test.js -- the extension's decisions, against the same
// cases as the server's Lua playhead (decide_vectors.json is exported from
// opx_watchparty/tests/decide_vectors.lua).
"use strict";
const assert = require("assert");
const path = require("path");
const Core = require(path.join(__dirname, "..", "sync-core.js"));
const vectors = require(path.join(__dirname, "decide_vectors.json"));

let checks = 0, failures = 0;
function check(label, fn) {
  checks++;
  try { fn(); console.log("  ok   " + label); }
  catch (e) { failures++; console.log("  FAIL " + label + "  -- " + e.message); }
}

check(`a follower's decision: the ${vectors.length} cases the server's playhead is held to`, () => {
  for (const v of vectors) {
    const got = Core.decide(v.localMs, v.localPlaying, v.targetMs, v.targetPlaying, v.toleranceMs);
    assert.strictEqual(got.seekMs, v.want.seekMs, v.name + " seek");
    assert.strictEqual(!!got.play, !!v.want.play, v.name + " play");
    assert.strictEqual(!!got.pause, !!v.want.pause, v.name + " pause");
    assert.strictEqual(got.driftMs, v.want.driftMs, v.name + " drift");
  }
});

check("the link's fragment gives the party code and the viewer's key", () => {
  assert.deepStrictEqual(Core.parseFragment("#opxwatch=ABC234.k3ykeyk3ykey"), { code: "ABC234", key: "k3ykeyk3ykey" });
  assert.deepStrictEqual(Core.parseFragment("#foo=1&opxwatch=abc234"), { code: "ABC234", key: "" });
  assert.strictEqual(Core.parseFragment("#opxwatch=ABC"), null);
  assert.strictEqual(Core.parseFragment("#opxwatch=ABC234.BADKEY"), null);
  assert.strictEqual(Core.parseFragment(""), null);
});

check("and is wiped from the address, keeping anything else", () => {
  assert.strictEqual(Core.stripFragment("#opxwatch=ABC234.k3ykeyk3ykey"), "");
  assert.strictEqual(Core.stripFragment("#a=1&opxwatch=ABC234"), "#a=1");
});

check("the title of a watch page, and nothing else", () => {
  assert.strictEqual(Core.movieIdFromPath("/watch/80057281"), "80057281");
  assert.strictEqual(Core.movieIdFromPath("/watch/80057281/"), "80057281");
  assert.strictEqual(Core.movieIdFromPath("/browse"), null);
  assert.strictEqual(Core.movieIdFromPath("/title/80057281"), null);
});

check("where the party is now: its answer plus the time since, never past the end", () => {
  const party = { positionMs: 60000, playing: true, durationMs: 61000 };
  assert.strictEqual(Core.target(party, 1000, 1500), 60500);
  assert.strictEqual(Core.target(party, 1000, 9000), 61000);
  assert.strictEqual(Core.target({ positionMs: 60000, playing: false }, 1000, 9000), 60000);
});

check("a viewer's own play/pause/seek goes to the party; echoes, autoplay and agreement do not", () => {
  const party = { netflixId: "80057281" };
  const base = { synced: true, canControl: true, now: 20000, loadedAt: 0, party,
    movieId: "80057281", partyPlaying: true, partyPositionMs: 60000 };
  assert.strictEqual(Core.isViewerAction({ type: "pause" }, base), true);
  assert.strictEqual(Core.isViewerAction({ type: "play" }, base), false, "party already plays");
  assert.strictEqual(Core.isViewerAction({ type: "pause" }, Object.assign({}, base, { loadedAt: 18000 })), false, "autoplay window");
  assert.strictEqual(Core.isViewerAction({ type: "pause" }, Object.assign({}, base, { canControl: false })), false, "no key");
  assert.strictEqual(Core.isViewerAction({ type: "pause" }, Object.assign({}, base, { movieId: "1234567" })), false, "other title");
  assert.strictEqual(Core.isViewerAction({ type: "seeked", positionMs: 300000 }, base), true);
  assert.strictEqual(Core.isViewerAction({ type: "seeked", positionMs: 61000 }, base), false, "within tolerance");
});

check("this extension's own commands are recognised when they echo back, and only they", () => {
  const expected = [{ type: "seeked", positionMs: 600000, until: 5000 }, { type: "play", until: 5000 }];
  assert.strictEqual(Core.matchEcho({ type: "seeked", positionMs: 600900 }, expected, 1000), 0);
  assert.strictEqual(Core.matchEcho({ type: "seeked", positionMs: 900000 }, expected, 1000), -1, "a seek elsewhere is the viewer's");
  assert.strictEqual(Core.matchEcho({ type: "play" }, expected, 1000), 1);
  assert.strictEqual(Core.matchEcho({ type: "pause" }, expected, 1000), -1, "a pause right after our play is the viewer's");
  assert.strictEqual(Core.matchEcho({ type: "play" }, expected, 6000), -1, "expired");
});

check("times and links read the way the in-game ones do", () => {
  assert.strictEqual(Core.fmt(3723000), "1:02:03");
  assert.strictEqual(Core.fmt(65000), "1:05");
  assert.strictEqual(Core.watchUrl("80057281", 95500), "https://www.netflix.com/watch/80057281?t=95");
});

console.log(`\n${checks} checks, ${failures} failed`);
process.exit(failures ? 1 : 0);
