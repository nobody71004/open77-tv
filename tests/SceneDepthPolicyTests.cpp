// Pins the two decisions SceneDepth makes without Direct3D: which of the game's
// depth buffers is the scene's, and what its values mean.
//
// The first is ReShade's generic-depth rule (the buffer that took the most work,
// among those shaped like the picture), widened for an upscaler's smaller depth.
// The cases are the buffers a frame of this game draws into: shadow maps (square,
// many draws), the scene (picture-shaped, most vertices), an interface stencil at
// the output size, a half-size buffer, and the same scene buffer drawn into the
// corner of a larger texture.
//
// The second is the calibration: B in device = A + B / z, measured from the points
// the overlay samples on a screen. A hand in front of some points, the sky (the
// clear value), a hand held over the whole middle for a while, and a camera whose
// near plane really changes are each pinned.

#include "webui/SceneDepthPolicy.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace Policy = op77::WebUI::SceneDepthPolicy;

#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::cerr << "Failed line " << __LINE__ << ": " #x "\n";                                                   \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

namespace
{
Policy::Candidate Buffer(const uint64_t aId, const uint32_t aWidth, const uint32_t aHeight, const uint32_t aDraws,
                         const uint64_t aVertices)
{
    Policy::Candidate candidate;
    candidate.id = aId;
    candidate.width = aWidth;
    candidate.height = aHeight;
    candidate.format = Policy::Dxgi::kR32G8X24Typeless;
    candidate.depthStencil = true;
    candidate.twoDimensional = true;
    candidate.draws = aDraws;
    candidate.vertices = aVertices;
    return candidate;
}

void Formats()
{
    CHECK(Policy::FamilyOf(Policy::Dxgi::kD32FloatS8X24Uint) == Policy::Family::D32S8);
    CHECK(Policy::FamilyOf(Policy::Dxgi::kR32Typeless) == Policy::Family::D32);
    CHECK(Policy::FamilyOf(Policy::Dxgi::kD24UnormS8Uint) == Policy::Family::D24S8);
    CHECK(Policy::FamilyOf(Policy::Dxgi::kD16Unorm) == Policy::Family::D16);
    CHECK(Policy::FamilyOf(28) == Policy::Family::None); // R8G8B8A8_UNORM
    CHECK(Policy::CopyFormat(Policy::Family::D32S8) == Policy::Dxgi::kR32G8X24Typeless);
    CHECK(Policy::ReadFormat(Policy::Family::D32S8) == Policy::Dxgi::kR32FloatX8X24Typeless);
    CHECK(Policy::ReadFormat(Policy::Family::D24S8) == Policy::Dxgi::kR24UnormX8Typeless);
    CHECK(Policy::PlaneCount(Policy::Family::D32S8) == 2 && Policy::PlaneCount(Policy::Family::D32) == 1);

    // A float, a 24-bit and a 16-bit texel, laid out as a copy lays them out.
    const float value = 0.0625F;
    std::array<uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &value, 4);
    CHECK(Policy::DecodeTexel(Policy::Dxgi::kR32Typeless, bytes.data()) == 0.0625);
    const std::array<uint8_t, 4> d24{0xFF, 0xFF, 0x7F, 0xAB}; // stencil byte ignored
    CHECK(std::abs(Policy::DecodeTexel(Policy::Dxgi::kR24G8Typeless, d24.data()) - (8388607.0 / 16777215.0)) < 1e-12);
    const std::array<uint8_t, 2> d16{0x00, 0x80};
    CHECK(std::abs(Policy::DecodeTexel(Policy::Dxgi::kR16Unorm, d16.data()) - (32768.0 / 65535.0)) < 1e-12);
    CHECK(Policy::DecodeTexel(28, bytes.data()) < 0.0);
}

void Shapes()
{
    // 2560x1600 output: native, DLSS performance (half), ultra performance (a
    // third), and DLAA-like supersampling are all picture-shaped.
    CHECK(Policy::PictureShaped({2560, 1600}, 2560, 1600));
    CHECK(Policy::PictureShaped({1280, 800}, 2560, 1600));
    CHECK(Policy::PictureShaped({853, 533}, 2560, 1600));
    CHECK(Policy::PictureShaped({3840, 2400}, 2560, 1600));
    // Not: a square shadow map, a cube face, a quarter-size buffer, a 4:3 buffer.
    CHECK(!Policy::PictureShaped({2048, 2048}, 2560, 1600));
    CHECK(!Policy::PictureShaped({640, 400}, 2560, 1600));
    CHECK(!Policy::PictureShaped({1600, 1200}, 2560, 1600));
    CHECK(!Policy::PictureShaped({0, 0}, 2560, 1600));
}

void Ranking()
{
    // A frame: two shadow cascades with many draws, the scene, an interface
    // stencil at the output size with many small draws, a half-size buffer.
    std::vector<Policy::Candidate> frame = {
        Buffer(1, 2048, 2048, 3000, 9000000),
        Buffer(2, 4096, 4096, 2000, 6000000),
        Buffer(3, 1280, 800, 2500, 7000000), // the scene, under DLSS performance
        Buffer(4, 2560, 1600, 900, 5400),     // the interface's stencil
        Buffer(5, 640, 400, 10, 60),
    };
    const auto ranked = Policy::Rank(frame, 2560, 1600);
    CHECK(ranked.size() == 2);
    CHECK(frame[ranked[0]].id == 3);
    CHECK(frame[ranked[1]].id == 4);
    CHECK(Policy::Judge(frame[0], 2560, 1600) == Policy::Verdict::WrongShape);
    CHECK(Policy::Judge(frame[4], 2560, 1600) == Policy::Verdict::WrongShape);
    CHECK(Policy::Choose(frame, ranked, 0, -1) == ranked[0]);

    // Draws counted mostly by indirect calls: compared by draws, not vertices.
    std::vector<Policy::Candidate> indirect = {Buffer(10, 2560, 1600, 400, 1000), Buffer(11, 2560, 1600, 300, 900000)};
    indirect[0].indirectDraws = 380;
    CHECK(!Policy::CompareByVertices(indirect));
    const auto byDraws = Policy::Rank(indirect, 2560, 1600);
    CHECK(indirect[byDraws[0]].id == 10);

    // Not drawn, not depth, multisampled, mipped: refused with the reason.
    Policy::Candidate idle = Buffer(20, 2560, 1600, 0, 0);
    CHECK(Policy::Judge(idle, 2560, 1600) == Policy::Verdict::NotDrawn);
    Policy::Candidate colour = Buffer(21, 2560, 1600, 100, 1000);
    colour.format = 28;
    CHECK(Policy::Judge(colour, 2560, 1600) == Policy::Verdict::NotDepth);
    Policy::Candidate noFlag = Buffer(22, 2560, 1600, 100, 1000);
    noFlag.depthStencil = false;
    CHECK(Policy::Judge(noFlag, 2560, 1600) == Policy::Verdict::NotDepth);
    Policy::Candidate msaa = Buffer(23, 2560, 1600, 100, 1000);
    msaa.samples = 4;
    CHECK(Policy::Judge(msaa, 2560, 1600) == Policy::Verdict::Multisampled);
    Policy::Candidate mipped = Buffer(24, 2560, 1600, 100, 1000);
    mipped.mipLevels = 2;
    CHECK(Policy::Judge(mipped, 2560, 1600) == Policy::Verdict::NotDepth);

    // Dynamic resolution: a texture the output's size, drawn into its top-left
    // two thirds. Shaped by what was drawn, and that is the extent the pass reads.
    Policy::Candidate dynamic = Buffer(30, 2560, 1600, 2000, 5000000);
    dynamic.viewportWidth = 1706.0F;
    dynamic.viewportHeight = 1066.0F;
    const Policy::Extent extent = Policy::DrawnExtent(dynamic);
    CHECK(extent.width == 1706.0F && extent.height == 1066.0F);
    CHECK(Policy::Judge(dynamic, 2560, 1600) == Policy::Verdict::Ok);
    // A viewport larger than the texture is clamped to it.
    dynamic.viewportWidth = 9999.0F;
    CHECK(Policy::DrawnExtent(dynamic).width == 2560.0F);
}

void Stickiness()
{
    // Two similar buffers (a game that alternates): the one in use is kept while it
    // still does at least half the work of the best, and left when it does not.
    std::vector<Policy::Candidate> frame = {Buffer(1, 2560, 1600, 1000, 4000000), Buffer(2, 2560, 1600, 1100, 4400000)};
    auto ranked = Policy::Rank(frame, 2560, 1600);
    CHECK(frame[ranked[0]].id == 2);
    CHECK(frame[Policy::Choose(frame, ranked, 1, -1)].id == 1);
    frame[0].vertices = 1000000;
    ranked = Policy::Rank(frame, 2560, 1600);
    CHECK(frame[Policy::Choose(frame, ranked, 1, -1)].id == 2);
    // The previous one not drawn this frame at all: the best.
    frame[0].draws = 0;
    ranked = Policy::Rank(frame, 2560, 1600);
    CHECK(frame[Policy::Choose(frame, ranked, 1, -1)].id == 2);
    // The "next candidate" key walks the ranked list and wraps.
    std::vector<Policy::Candidate> three = {Buffer(1, 2560, 1600, 100, 3000), Buffer(2, 2560, 1600, 100, 2000),
                                            Buffer(3, 2560, 1600, 100, 1000)};
    ranked = Policy::Rank(three, 2560, 1600);
    CHECK(three[Policy::Choose(three, ranked, 0, 0)].id == 1);
    CHECK(three[Policy::Choose(three, ranked, 0, 1)].id == 2);
    CHECK(three[Policy::Choose(three, ranked, 0, 2)].id == 3);
    CHECK(three[Policy::Choose(three, ranked, 0, 3)].id == 1);
    // Nothing usable: the size of the list, which callers read as "none".
    std::vector<Policy::Candidate> none = {Buffer(1, 2048, 2048, 100, 1000)};
    ranked = Policy::Rank(none, 2560, 1600);
    CHECK(Policy::Choose(none, ranked, 0, -1) == none.size());
}

/// One present's choice over the history, as SceneDepth makes it.
uint64_t ChooseOver(const Policy::History& aHistory, const uint64_t aPrevious)
{
    const auto& summed = aHistory.Candidates();
    const auto ranked = Policy::Rank(summed, 2560, 1600);
    const size_t index = Policy::Choose(summed, ranked, aPrevious, -1);
    return index < summed.size() ? summed[index].id : 0;
}

void FrameGeneration()
{
    // What the game's log showed with frame generation: about four presents for each
    // frame the game renders. The scene's buffer (D32_S8 at a third of 2560x1600,
    // ~2400 draws) is drawn between one pair of presents, a small pass's buffer
    // (D32, 119 draws) between the next, and nothing between the other two.
    Policy::Candidate scene = Buffer(1, 853, 533, 2390, 3700000);
    scene.indirectDraws = 42;
    scene.readByShaders = true;
    Policy::Candidate small = Buffer(2, 853, 533, 119, 21858);
    small.format = Policy::Dxgi::kR32Typeless;
    small.readByShaders = true;

    // One present at a time, the small pass's present chooses the small buffer:
    // the choice took turns with every present, and the copy with it.
    const std::vector<Policy::Candidate> alone = {small};
    const auto rankedAlone = Policy::Rank(alone, 2560, 1600);
    CHECK(alone[Policy::Choose(alone, rankedAlone, 1, -1)].id == 2);

    // Over the history it is the scene's buffer at every present, from the first.
    Policy::History history;
    uint64_t chosen = 0;
    for (int present = 0; present < 64; ++present)
    {
        std::vector<Policy::Candidate> now;
        if (present % 4 == 0)
        {
            now.push_back(scene);
        }
        else if (present % 4 == 1)
        {
            now.push_back(small);
        }
        history.Add(now);
        chosen = ChooseOver(history, chosen);
        CHECK(chosen == 1);
    }

    // A new scene buffer (the game changed resolution): the old one is left within
    // a few frames, and the new one kept from then on.
    Policy::Candidate next = Buffer(3, 1280, 800, 2390, 3700000);
    next.readByShaders = true;
    int switchedAt = -1;
    for (int present = 0; present < 48; ++present)
    {
        std::vector<Policy::Candidate> now;
        if (present % 4 == 0)
        {
            now.push_back(next);
        }
        history.Add(now);
        chosen = ChooseOver(history, chosen);
        if (chosen == 3 && switchedAt < 0)
        {
            switchedAt = present;
        }
        if (switchedAt >= 0)
        {
            CHECK(chosen == 3);
        }
    }
    CHECK(switchedAt >= 0 && switchedAt <= 16);

    // Gone buffers fade, and are forgotten once their work has shrunk to nothing.
    for (int present = 0; present < 96; ++present)
    {
        std::vector<Policy::Candidate> now;
        if (present % 4 == 0)
        {
            now.push_back(next);
        }
        history.Add(now);
        CHECK(ChooseOver(history, chosen) == 3);
    }
    CHECK(history.Candidates().size() == 1 && history.Candidates()[0].id == 3);
}

void HistoryKeeping()
{
    // A present's counts are added, earlier ones shrink by kKeep a present.
    Policy::History history;
    const std::vector<Policy::Candidate> one = {Buffer(5, 2560, 1600, 100, 8000)};
    history.Add(one);
    CHECK(history.Candidates().size() == 1 && history.Candidates()[0].draws == 100);
    history.Add({});
    CHECK(history.Candidates()[0].draws == 88); // 87.5, rounded
    CHECK(history.Candidates()[0].vertices == 7000);

    // The viewport of the last present that drew is kept through presents that did not.
    std::vector<Policy::Candidate> drawn = {Buffer(6, 2560, 1600, 50, 5000)};
    drawn[0].viewportWidth = 1706.0F;
    drawn[0].viewportHeight = 1066.0F;
    history.Add(drawn);
    history.Add({});
    for (const auto& candidate : history.Candidates())
    {
        if (candidate.id == 6)
        {
            CHECK(candidate.viewportWidth == 1706.0F && candidate.viewportHeight == 1066.0F);
        }
    }

    // Another buffer made where a gone one was (same address, another shape): its
    // counts start again.
    const std::vector<Policy::Candidate> reshaped = {Buffer(5, 1280, 800, 10, 300)};
    history.Add(reshaped);
    for (const auto& candidate : history.Candidates())
    {
        if (candidate.id == 5)
        {
            CHECK(candidate.width == 1280 && candidate.draws == 10);
        }
    }

    // A buffer drawn once stops counting as drawn after a few presents, and is
    // forgotten after a few more.
    Policy::History once;
    once.Add(std::vector<Policy::Candidate>{Buffer(7, 2560, 1600, 1, 6)});
    for (int present = 0; present < 6; ++present)
    {
        once.Add({});
    }
    CHECK(once.Candidates().size() == 1);
    CHECK(Policy::Judge(once.Candidates()[0], 2560, 1600) == Policy::Verdict::NotDrawn);
    for (int present = 0; present < 20; ++present)
    {
        once.Add({});
    }
    CHECK(once.Candidates().empty());

    // At most kCapacity remembered.
    Policy::History many;
    std::vector<Policy::Candidate> crowd;
    for (uint64_t id = 1; id <= Policy::History::kCapacity + 8; ++id)
    {
        crowd.push_back(Buffer(id, 2560, 1600, static_cast<uint32_t>(id), id * 10));
    }
    many.Add(crowd);
    CHECK(many.Candidates().size() == Policy::History::kCapacity);
}

void CopyTiming()
{
    Policy::CopyInputs finished;
    finished.seenThisPresent = true;
    finished.writtenSinceCopy = true;
    finished.everSettled = true;
    finished.settledSinceClear = true;
    CHECK(Policy::ShouldCopy(finished));

    // Not used since the last present (it may be gone), not changed since the last
    // copy, or its memory given away: the copy the overlay has is kept.
    Policy::CopyInputs inputs = finished;
    inputs.seenThisPresent = false;
    CHECK(!Policy::ShouldCopy(inputs));
    inputs = finished;
    inputs.writtenSinceCopy = false;
    CHECK(!Policy::ShouldCopy(inputs));
    inputs = finished;
    inputs.aliasedAway = true;
    CHECK(!Policy::ShouldCopy(inputs));

    // Mid-frame (cleared and drawn into, not yet done with): kept, unless it has been
    // mid-frame at so many presents in a row that a game with an odd order would
    // never get a copy.
    Policy::CopyInputs midFrame = finished;
    midFrame.settledSinceClear = false;
    midFrame.waits = 1;
    CHECK(!Policy::ShouldCopy(midFrame));
    midFrame.readOnlyNow = true; // left where nothing writes it
    CHECK(Policy::ShouldCopy(midFrame));
    midFrame.readOnlyNow = false;
    midFrame.waits = Policy::kForceCopyAfterWaits;
    CHECK(Policy::ShouldCopy(midFrame));

    // A game that never reads its depth: copied whenever it changed.
    Policy::CopyInputs neverRead = finished;
    neverRead.everSettled = false;
    neverRead.settledSinceClear = false;
    CHECK(Policy::ShouldCopy(neverRead));
    // A copy outlives the presents a forced one can take to come.
    CHECK(Policy::kCopyFreshForPresents > Policy::kForceCopyAfterWaits * 2);
}

/// The points one screen gives: the glass at `aDepth` metres (reversed, infinite
/// far, near plane `aNear`), some of them covered by something at `aCover` metres.
std::vector<Policy::Sample> Screen(const double aNear, const double aDepth, const size_t aCovered, const double aCover)
{
    std::vector<Policy::Sample> samples;
    for (size_t index = 0; index < Policy::kSamplesPerScreen; ++index)
    {
        const double seen = index < aCovered ? aCover : aDepth;
        samples.push_back({1.0 / aDepth, aNear / seen});
    }
    return samples;
}

void Calibration()
{
    // Unknown direction: nothing is measured.
    {
        Policy::Calibration calibration;
        CHECK(!calibration.AddGroup(Screen(0.1, 2.0, 0, 0.0)));
    }
    CHECK(Policy::FromClear(0.0F) == Policy::Convention::Reversed);
    CHECK(Policy::FromClear(1.0F) == Policy::Convention::Conventional);
    CHECK(Policy::FromClear(0.5F) == Policy::Convention::Unknown);

    // Reversed depth, near plane 0.1 m, screens between 1.5 and 3 m with a hand
    // (0.5 m) over a third of the points: locks on B = 0.1 after enough groups.
    Policy::Calibration calibration;
    calibration.SetConvention(Policy::Convention::Reversed);
    for (size_t group = 0; group < Policy::Calibration::kToLock - 1; ++group)
    {
        CHECK(calibration.AddGroup(Screen(0.1, 1.5 + (0.1 * static_cast<double>(group % 15)), 3, 0.5)));
        CHECK(!calibration.Locked());
    }
    CHECK(calibration.AddGroup(Screen(0.1, 2.0, 3, 0.5)));
    CHECK(calibration.Locked());
    CHECK(std::abs(calibration.B() - 0.1) < 1e-9);
    CHECK(calibration.A() == 0.0);

    // The sky (the clear value) and nonsense are skipped, not taken as B = 0.
    std::vector<Policy::Sample> sky = Screen(0.1, 2.0, 0, 0.0);
    sky[0].device = 0.0;
    sky[1].device = -1.0;
    sky[2].inverseDepth = 0.0;
    CHECK(calibration.AddGroup(sky));
    CHECK(std::abs(calibration.B() - 0.1) < 1e-9);
    std::vector<Policy::Sample> allSky(Policy::kSamplesPerScreen, Policy::Sample{0.5, 0.0});
    CHECK(!calibration.AddGroup(allSky));

    // A hand held over the whole middle of a screen for a while: those groups say
    // B is four times larger. A window that is partly them does not agree, so B
    // stays where it is...
    for (size_t group = 0; group < Policy::Calibration::kWindow / 2; ++group)
    {
        CHECK(calibration.AddGroup(Screen(0.1, 2.0, Policy::kSamplesPerScreen, 0.5)));
    }
    CHECK(std::abs(calibration.B() - 0.1) < 1e-9);
    // ...and goes back to agreeing once the hand is gone.
    for (size_t group = 0; group < Policy::Calibration::kWindow; ++group)
    {
        CHECK(calibration.AddGroup(Screen(0.1, 2.0, 0, 0.0)));
    }
    CHECK(std::abs(calibration.B() - 0.1) < 1e-9);

    // A real change (a camera with another near plane): a whole window agreeing on
    // the new value moves it.
    for (size_t group = 0; group < Policy::Calibration::kWindow; ++group)
    {
        CHECK(calibration.AddGroup(Screen(0.05, 2.0, 0, 0.0)));
    }
    CHECK(std::abs(calibration.B() - 0.05) < 1e-9);

    // Conventional depth: device = 1 - near / z (far at infinity). B comes out
    // negative, as the shader's model wants it.
    Policy::Calibration conventional;
    conventional.SetConvention(Policy::Convention::Conventional);
    for (size_t group = 0; group < Policy::Calibration::kToLock; ++group)
    {
        std::vector<Policy::Sample> samples;
        for (size_t index = 0; index < Policy::kSamplesPerScreen; ++index)
        {
            const double z = index < 2 ? 0.4 : 2.5;
            samples.push_back({1.0 / 2.5, 1.0 - (0.07 / z)});
        }
        CHECK(conventional.AddGroup(samples));
    }
    CHECK(conventional.Locked());
    CHECK(std::abs(conventional.B() + 0.07) < 1e-9);
    CHECK(conventional.A() == 1.0);

    // Changing direction starts again.
    conventional.SetConvention(Policy::Convention::Reversed);
    CHECK(!conventional.Locked() && conventional.Groups() == 0);

    // Values no camera has are not used: a "near plane" of 5 m.
    Policy::Calibration absurd;
    absurd.SetConvention(Policy::Convention::Reversed);
    CHECK(!absurd.AddGroup(Screen(5.0, 2.0, 0, 0.0)));

    // Groups that do not agree (noise of 20 %) never lock.
    Policy::Calibration noisy;
    noisy.SetConvention(Policy::Convention::Reversed);
    for (size_t group = 0; group < Policy::Calibration::kWindow; ++group)
    {
        const double near = (group % 2) == 0 ? 0.08 : 0.12;
        noisy.AddGroup(Screen(near, 2.0, 0, 0.0));
    }
    CHECK(!noisy.Locked());
}
} // namespace

int main()
{
    Formats();
    Shapes();
    Ranking();
    Stickiness();
    FrameGeneration();
    HistoryKeeping();
    CopyTiming();
    Calibration();
    std::cout << "SceneDepthPolicy: buffer choice, copy timing and depth calibration hold\n";
    return 0;
}
