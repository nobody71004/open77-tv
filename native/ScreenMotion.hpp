#pragma once

// How a television's picture keeps up with the frame that presents it.
//
// ---------------------------------------------------------------------------
// THE PROBLEM THIS SOLVES
// ---------------------------------------------------------------------------
//
// A screen's four corners are projected on the game thread, once per
// `OnRunningUpdate`, because `gameCameraSystem::ProjectPoint` is an RTTI call
// that is only safe there. `WorldOverlay::Draw` then runs once per PRESENTED
// frame, which is not the same rate and not even close to it: a measured session
// logged
//
//     Open77 frame:  718 frames in 30020 ms = 23.9 Hz
//     Open77 frame: 1091 frames in 30011 ms = 36.4 Hz
//     Open77 frame: 2072 frames in 30026 ms = 69.0 Hz
//
// for the producer, against a display running faster than all three -- and with
// DLSS frame generation presenting interpolated frames in between. The world is
// drawn per presented frame, and the camera that draws it is interpolated
// between simulation states. Our rectangle was NOT: it held whichever corners
// the last game tick produced and repeated them until the next one.
//
// Which is exactly what was reported, and why it was reported the way it was:
// while anything moves, the picture slides and judders against the television
// cabinet it is supposed to sit on, and the moment the player stops both are
// still and it looks perfect. "The screen on the tv starts visually moving
// around" -- only with movement, gone when you stop.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES
// ---------------------------------------------------------------------------
//
// The render thread keeps the two most recent published corner sets for each
// surface and their arrival times, and draws the quad at the fraction of that
// interval the presented frame actually sits at. The blend is *interpolation
// only, clamped*: the fraction never leaves 0..1, so this can never extrapolate
// past what the game thread has said. A frame presented late draws the newest
// sample exactly, which is the behaviour that shipped before this existed; a
// frame presented between two samples draws where the quad was between them. On
// a session where the producer and the display already run at the same rate the
// fraction is 1 every time and nothing changes.
//
// What this does NOT do is remove the phase error between the last game tick and
// the presented frame. That floor is documented in `WorldOverlay.hpp`, and
// closing it needs the render thread's own camera, which the engine does not
// publish. What it removes is the stepping, which is the part that reads as
// movement because it is movement: a discrete jump of the picture every time a
// game tick lands, against a world that moves smoothly underneath it.
//
// The blend lives here, pure and free of the engine, for the same reason
// `api/ScreenQuad.hpp` does: it is arithmetic that a session cannot verify and a
// test can.

#include <array>
#include <cmath>
#include <cstddef>

namespace op77::WorldOverlay::ScreenMotion
{
/// How long a held sample may go unreplaced before it stops being blended from.
///
/// A screen that disappears and a surface id that is reused must not lerp from
/// where the old picture was: an interval longer than this is treated as "no
/// previous sample", and the quad is drawn where the newest tick put it. Five
/// game ticks at the slowest rate measured above.
inline constexpr double kMaximumHoldMilliseconds = 250.0;

/// Where the presented frame sits between two samples, in 0..1.
///
/// A degenerate or non-finite interval answers 1.0, i.e. "the newest sample",
/// rather than 0.0: the newest sample is the one the game thread actually
/// stated, and a frame that cannot place itself in the interval must draw the
/// real answer and not the previous one.
[[nodiscard]] inline float Fraction(const double aElapsedMilliseconds,
                                    const double aSpanMilliseconds)
{
    if (!std::isfinite(aElapsedMilliseconds) || !std::isfinite(aSpanMilliseconds) ||
        !(aSpanMilliseconds > 0.0))
    {
        return 1.0F;
    }
    const double fraction = aElapsedMilliseconds / aSpanMilliseconds;
    if (!(fraction > 0.0))
    {
        return 0.0F;
    }
    return static_cast<float>(fraction >= 1.0 ? 1.0 : fraction);
}

/// The corners the presented frame should draw, given the interval it sits in.
///
/// Component-wise and in the same normalised viewport space the producer
/// published. A fraction of exactly 1 returns `aTo` unchanged, which is what
/// keeps a session that is not rate-mismatched drawing precisely what it drew
/// before this seam existed.
[[nodiscard]] inline std::array<float, 8> Blend(const std::array<float, 8>& aFrom,
                                                const std::array<float, 8>& aTo,
                                                const float aFraction)
{
    std::array<float, 8> out = aTo;
    if (!(aFraction < 1.0F) || !(aFraction > 0.0F))
    {
        // 0 draws the previous sample, 1 the new one, and anything outside is
        // clamped by `Fraction` before it gets here.
        if (aFraction <= 0.0F)
        {
            return aFrom;
        }
        return aTo;
    }
    for (std::size_t index = 0; index < out.size(); ++index)
    {
        out[index] = aFrom[index] + ((aTo[index] - aFrom[index]) * aFraction);
    }
    return out;
}

/// Whether a held sample is recent enough to be blended from.
///
/// Stated as a predicate rather than written inline in the drawer so the rule is
/// testable: the failure it guards against -- lerping from a corner set the
/// player has not been able to see for a second -- is a picture that sweeps
/// across the world once when a television is switched back on.
[[nodiscard]] inline bool UsableAsPrevious(const double aAgeMilliseconds,
                                           const double aMaximumHold = kMaximumHoldMilliseconds)
{
    return std::isfinite(aAgeMilliseconds) && aAgeMilliseconds >= 0.0 &&
           aAgeMilliseconds <= aMaximumHold;
}
} // namespace op77::WorldOverlay::ScreenMotion
