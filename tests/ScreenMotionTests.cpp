// Pins the arithmetic that keeps a television's picture on its television.
//
// The failure this exists for cannot be asserted with a screenshot and cannot be
// logged: the screen is in the right place, the right size, on the right prop,
// and it is correct whenever nothing moves. It slides against the prop only
// while the world does -- because the corners are projected once per game update
// (measured at 24-69 Hz in a real session) and drawn once per presented frame
// (faster, and with frame generation, on frames interpolated between them).
//
// So what is pinned here is that the frame between two samples draws where the
// quad was between them, that a frame that cannot place itself in the interval
// draws what the game thread actually stated rather than the older sample, and
// that a held sample is not blended from once it is too old to be a previous
// frame of anything.
//
// The other half of the property is what this must NOT do: extrapolate. The
// fraction is clamped, so the quad can never be drawn beyond the newest corners
// the game thread produced -- an overshoot would be visible as the picture
// sailing off the television every time the player stopped.

#include "webui/ScreenMotion.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace op77::WorldOverlay::ScreenMotion;

#define CHECK(x)                                                                         \
    do                                                                                   \
    {                                                                                    \
        if (!(x))                                                                        \
        {                                                                                \
            std::cerr << "Failed line " << __LINE__ << ": " #x "\n";                     \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

namespace
{
constexpr float kTolerance = 1.0e-5F;

bool Near(const float aLeft, const float aRight)
{
    return std::abs(aLeft - aRight) <= kTolerance;
}

std::array<float, 8> Corners(const float aBias)
{
    return {0.0F + aBias, 1.0F + aBias, 2.0F + aBias, 3.0F + aBias,
            4.0F + aBias, 5.0F + aBias, 6.0F + aBias, 7.0F + aBias};
}
} // namespace

int main()
{
    // ---------------------------------------------------------------- fraction
    // The presented frame sits halfway between two ticks: the quad that tick 1
    // published and tick 2 will replace is drawn halfway from one to the other.
    CHECK(Near(Fraction(8.0, 16.0), 0.5F));

    // A frame that has run past the newer sample draws the newer sample. This is
    // today's behaviour and it has to survive unchanged, because a session where
    // the producer and the display agree takes this branch every time.
    CHECK(Near(Fraction(16.0, 16.0), 1.0F));
    CHECK(Near(Fraction(21.5, 16.0), 1.0F));

    // Before the interval, and a degenerate or nonsense interval: the newest
    // statement wins. Inventing 0.0 would draw a corner set the game thread has
    // already replaced.
    CHECK(Near(Fraction(-3.0, 16.0), 0.0F));
    CHECK(Near(Fraction(5.0, 0.0), 1.0F));
    CHECK(Near(Fraction(5.0, -16.0), 1.0F));
    CHECK(Near(Fraction(std::nan(""), 16.0), 1.0F));
    CHECK(Near(Fraction(5.0, std::nan("")), 1.0F));
    CHECK(Near(Fraction(std::numeric_limits<double>::infinity(), 16.0), 1.0F));

    // ------------------------------------------------------------------- blend
    const auto from = Corners(0.0F);
    const auto to = Corners(10.0F);

    // The two ends are returned exactly, not approximately: a fraction of 1 must
    // reproduce the producer's own numbers bit for bit, or every screen in the
    // session shifts by a rounding step the moment this seam is added.
    const auto newest = Blend(from, to, 1.0F);
    for (std::size_t index = 0; index < newest.size(); ++index)
    {
        CHECK(newest[index] == to[index]);
    }
    const auto oldest = Blend(from, to, 0.0F);
    for (std::size_t index = 0; index < oldest.size(); ++index)
    {
        CHECK(oldest[index] == from[index]);
    }

    // Halfway is halfway, per component.
    const auto middle = Blend(from, to, 0.5F);
    for (std::size_t index = 0; index < middle.size(); ++index)
    {
        CHECK(Near(middle[index], (from[index] + to[index]) * 0.5F));
    }

    // Monotone in the fraction, and never outside the two samples: the clamp is
    // what makes extrapolation impossible, and extrapolation is the one failure
    // that would be *worse* than holding the older sample.
    for (float fraction = 0.0F; fraction <= 1.0001F; fraction += 0.1F)
    {
        const auto placed = Blend(from, to, fraction);
        for (std::size_t index = 0; index < placed.size(); ++index)
        {
            CHECK(placed[index] >= std::min(from[index], to[index]) - kTolerance);
            CHECK(placed[index] <= std::max(from[index], to[index]) + kTolerance);
        }
    }

    // A fraction outside 0..1 -- which `Fraction` cannot produce, and which a
    // future caller might -- is clamped rather than extrapolated.
    const auto beyond = Blend(from, to, 4.0F);
    for (std::size_t index = 0; index < beyond.size(); ++index)
    {
        CHECK(beyond[index] == to[index]);
    }
    const auto before = Blend(from, to, -4.0F);
    for (std::size_t index = 0; index < before.size(); ++index)
    {
        CHECK(before[index] == from[index]);
    }

    // ------------------------------------------------------------- held sample
    // A previous sample is only a previous frame of something while it is recent.
    // A screen switched off for a second and switched back on must be drawn where
    // it is, not swept in from where it used to be.
    CHECK(UsableAsPrevious(0.0));
    CHECK(UsableAsPrevious(16.0));
    CHECK(UsableAsPrevious(kMaximumHoldMilliseconds));
    CHECK(!UsableAsPrevious(kMaximumHoldMilliseconds + 1.0));
    CHECK(!UsableAsPrevious(-1.0));
    CHECK(!UsableAsPrevious(std::nan("")));

    std::cout << "Open77 screen motion tests passed\n";
    return 0;
}
