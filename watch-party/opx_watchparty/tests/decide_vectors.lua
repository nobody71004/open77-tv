-- What a follower does about the party, one case per line. Shared by the server's
-- Lua playhead (tests/run.lua) and the browser extension (tests/sync-core.test.js
-- reads the same cases, exported by tests/export_vectors.lua): one rule, two ports.
return {
    { name = "in step, both playing", localMs = 60000, localPlaying = true, targetMs = 60800, targetPlaying = true,
      want = { driftMs = -800 } },
    { name = "in step at the edge (2 s)", localMs = 62000, localPlaying = true, targetMs = 60000, targetPlaying = true,
      want = { driftMs = 2000 } },
    { name = "just past the edge", localMs = 62001, localPlaying = true, targetMs = 60000, targetPlaying = true,
      want = { seekMs = 60000, driftMs = 2001 } },
    { name = "far behind", localMs = 10000, localPlaying = true, targetMs = 600000, targetPlaying = true,
      want = { seekMs = 600000, driftMs = -590000 } },
    { name = "party paused, viewer playing", localMs = 30000, localPlaying = true, targetMs = 30200, targetPlaying = false,
      want = { pause = true, driftMs = -200 } },
    { name = "party playing, viewer paused and behind", localMs = 0, localPlaying = false, targetMs = 45000, targetPlaying = true,
      want = { seekMs = 45000, play = true, driftMs = -45000 } },
    { name = "both paused, far apart", localMs = 5000, localPlaying = false, targetMs = 90000, targetPlaying = false,
      want = { seekMs = 90000, driftMs = -85000 } },
    { name = "a wider tolerance", localMs = 64000, localPlaying = true, targetMs = 60000, targetPlaying = true,
      toleranceMs = 5000, want = { driftMs = 4000 } },
    { name = "fractions round", localMs = 1000.4, localPlaying = false, targetMs = 9000.6, targetPlaying = false,
      want = { seekMs = 9001, driftMs = -8000 } },
}
