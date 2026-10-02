#pragma once

// Which of the game's depth buffers holds the scene, and what its numbers mean.
//
// The overlay draws a television's picture after the game has finished its frame,
// so the only thing that can say "V's hand is in front of this screen" is the depth
// buffer the game drew the frame with. Two questions decide whether that works, and
// both are answered here without Direct3D, so that the tests can pin them:
//
//   * Which buffer. A game draws into several depth buffers a frame: shadow maps,
//     the scene, sometimes a buffer for the interface. SceneDepth.cpp counts, per
//     buffer, the draws the game makes while it is bound (the method ReShade's
//     generic depth add-on uses, which works on this game); `History` sums that
//     work over the last several presents (a frame can span several), and `Rank`
//     keeps the ones shaped like the picture and orders them by it. `ShouldCopy`
//     says when the chosen buffer holds a finished frame worth copying.
//   * What a value means. Every perspective projection stores device = A + B / z
//     for view depth z. The direction (A) follows from the value the buffer is
//     cleared to; B is the camera's near plane, which nothing in the overlay
//     knows. `Calibration` measures it from the screens themselves: the producer
//     publishes each screen's own view depth, so wherever the game drew the
//     screen's glass, device depth times that view depth is B. Something in front
//     of the glass only makes that product larger, so the lowest product several
//     points of a screen agree on is the glass -- and a value is only taken from
//     screens seen from different distances, which a body in front of the
//     screen cannot fake. Reversed depth starts from this game's near plane.
//
// Formats are DXGI numbers (dxgiformat.h), so this header needs no Windows
// headers; SceneDepth.cpp checks them against the real enum at compile time.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace op77::WebUI::SceneDepthPolicy
{
// -- formats ------------------------------------------------------------------------

enum class Family : uint8_t
{
    None,
    D32S8, ///< 32-bit float depth and 8-bit stencil, two planes
    D32,   ///< 32-bit float depth
    D24S8, ///< 24-bit normalised depth and 8-bit stencil, two planes
    D16,   ///< 16-bit normalised depth
};

namespace Dxgi
{
inline constexpr uint32_t kR32G8X24Typeless = 19;
inline constexpr uint32_t kD32FloatS8X24Uint = 20;
inline constexpr uint32_t kR32FloatX8X24Typeless = 21;
inline constexpr uint32_t kX32TypelessG8X24Uint = 22;
inline constexpr uint32_t kR32Typeless = 39;
inline constexpr uint32_t kD32Float = 40;
inline constexpr uint32_t kR32Float = 41;
inline constexpr uint32_t kR24G8Typeless = 44;
inline constexpr uint32_t kD24UnormS8Uint = 45;
inline constexpr uint32_t kR24UnormX8Typeless = 46;
inline constexpr uint32_t kX24TypelessG8Uint = 47;
inline constexpr uint32_t kR16Typeless = 53;
inline constexpr uint32_t kD16Unorm = 55;
inline constexpr uint32_t kR16Unorm = 56;
} // namespace Dxgi

[[nodiscard]] constexpr Family FamilyOf(const uint32_t aFormat)
{
    switch (aFormat)
    {
    case Dxgi::kR32G8X24Typeless:
    case Dxgi::kD32FloatS8X24Uint:
    case Dxgi::kR32FloatX8X24Typeless:
    case Dxgi::kX32TypelessG8X24Uint: return Family::D32S8;
    case Dxgi::kR32Typeless:
    case Dxgi::kD32Float:
    case Dxgi::kR32Float: return Family::D32;
    case Dxgi::kR24G8Typeless:
    case Dxgi::kD24UnormS8Uint:
    case Dxgi::kR24UnormX8Typeless:
    case Dxgi::kX24TypelessG8Uint: return Family::D24S8;
    case Dxgi::kR16Typeless:
    case Dxgi::kD16Unorm:
    case Dxgi::kR16Unorm: return Family::D16;
    default: return Family::None;
    }
}

/// The format the overlay's copy of the buffer is made in: the buffer's own family,
/// typeless, so a plain copy fills it and a shader can read it.
[[nodiscard]] constexpr uint32_t CopyFormat(const Family aFamily)
{
    switch (aFamily)
    {
    case Family::D32S8: return Dxgi::kR32G8X24Typeless;
    case Family::D32: return Dxgi::kR32Typeless;
    case Family::D24S8: return Dxgi::kR24G8Typeless;
    case Family::D16: return Dxgi::kR16Typeless;
    case Family::None: break;
    }
    return 0;
}

/// The format a shader reads the depth plane of that copy through.
[[nodiscard]] constexpr uint32_t ReadFormat(const Family aFamily)
{
    switch (aFamily)
    {
    case Family::D32S8: return Dxgi::kR32FloatX8X24Typeless;
    case Family::D32: return Dxgi::kR32Float;
    case Family::D24S8: return Dxgi::kR24UnormX8Typeless;
    case Family::D16: return Dxgi::kR16Unorm;
    case Family::None: break;
    }
    return 0;
}

[[nodiscard]] constexpr uint32_t PlaneCount(const Family aFamily)
{
    return (aFamily == Family::D32S8 || aFamily == Family::D24S8) ? 2U : 1U;
}

[[nodiscard]] constexpr const char* Describe(const Family aFamily)
{
    switch (aFamily)
    {
    case Family::D32S8: return "D32_S8";
    case Family::D32: return "D32";
    case Family::D24S8: return "D24_S8";
    case Family::D16: return "D16";
    case Family::None: break;
    }
    return "not depth";
}

/// One texel of the depth plane, as the copy to a readback buffer lays it out
/// (`aFootprintFormat` is what GetCopyableFootprints says the plane is). Negative
/// when the layout is not one this reads.
[[nodiscard]] inline double DecodeTexel(const uint32_t aFootprintFormat, const uint8_t* const aBytes)
{
    switch (aFootprintFormat)
    {
    case Dxgi::kR32Typeless:
    case Dxgi::kR32Float:
    case Dxgi::kD32Float:
    case Dxgi::kR32G8X24Typeless:
    case Dxgi::kR32FloatX8X24Typeless:
    case Dxgi::kD32FloatS8X24Uint:
    {
        float value = 0.0F;
        static_assert(sizeof(value) == 4);
        std::copy_n(aBytes, 4, reinterpret_cast<uint8_t*>(&value));
        return std::isfinite(value) ? static_cast<double>(value) : -1.0;
    }
    case Dxgi::kR24G8Typeless:
    case Dxgi::kR24UnormX8Typeless:
    case Dxgi::kD24UnormS8Uint:
    {
        const uint32_t bits = static_cast<uint32_t>(aBytes[0]) | (static_cast<uint32_t>(aBytes[1]) << 8U) |
                              (static_cast<uint32_t>(aBytes[2]) << 16U);
        return static_cast<double>(bits) / 16777215.0;
    }
    case Dxgi::kR16Typeless:
    case Dxgi::kR16Unorm:
    case Dxgi::kD16Unorm:
    {
        const uint32_t bits = static_cast<uint32_t>(aBytes[0]) | (static_cast<uint32_t>(aBytes[1]) << 8U);
        return static_cast<double>(bits) / 65535.0;
    }
    default: return -1.0;
    }
}

// -- which buffer -------------------------------------------------------------------

/// A depth buffer the game drew with in the frame just presented.
struct Candidate
{
    uint64_t id{};          ///< the resource's address: identity only
    uint32_t width{};       ///< the texture
    uint32_t height{};
    uint32_t format{};      ///< DXGI
    uint32_t samples{1};
    uint32_t mipLevels{1};
    uint32_t arraySize{1};
    bool depthStencil{};    ///< made with ALLOW_DEPTH_STENCIL
    bool twoDimensional{};  ///< a 2-D texture
    uint32_t draws{};       ///< draws while it was bound, indirect ones counted by their maximum
    uint32_t indirectDraws{};
    uint64_t vertices{};    ///< vertices (or indices) times instances of the direct draws
    float viewportWidth{};  ///< the largest viewport those draws used; 0 when none was seen
    float viewportHeight{};
    bool readByShaders{};   ///< moved into a shader-readable state this frame
};

enum class Verdict : uint8_t
{
    Ok,
    NotDepth,       ///< not a depth format, not made for depth, or not a plain 2-D texture
    Multisampled,
    NotDrawn,       ///< nothing was drawn with it this frame
    WrongShape,     ///< not the picture's shape: a shadow map, a cube face, a small buffer
};

[[nodiscard]] constexpr const char* Describe(const Verdict aVerdict)
{
    switch (aVerdict)
    {
    case Verdict::Ok: return "ok";
    case Verdict::NotDepth: return "not_depth";
    case Verdict::Multisampled: return "multisampled";
    case Verdict::NotDrawn: return "not_drawn";
    case Verdict::WrongShape: return "wrong_shape";
    }
    return "unknown";
}

/// The part of the texture the game drew into: its viewport when that is known and
/// fits, otherwise the whole texture.
struct Extent
{
    float width{};
    float height{};
};

[[nodiscard]] inline Extent DrawnExtent(const Candidate& aCandidate)
{
    Extent extent{static_cast<float>(aCandidate.width), static_cast<float>(aCandidate.height)};
    if (aCandidate.viewportWidth >= 1.0F && aCandidate.viewportHeight >= 1.0F)
    {
        extent.width = std::min(extent.width, std::floor(aCandidate.viewportWidth + 0.5F));
        extent.height = std::min(extent.height, std::floor(aCandidate.viewportHeight + 0.5F));
    }
    return extent;
}

/// Shaped like the picture: the same proportions within 0.1 (as ReShade allows),
/// and between a third of the picture's size (an upscaler at its most aggressive
/// renders depth at a third of the output) and a little over twice it (a game
/// rendering above the output resolution).
[[nodiscard]] inline bool PictureShaped(const Extent aExtent, const float aOverlayWidth, const float aOverlayHeight)
{
    if (!(aExtent.width >= 1.0F) || !(aExtent.height >= 1.0F) || !(aOverlayWidth >= 1.0F) ||
        !(aOverlayHeight >= 1.0F))
    {
        return false;
    }
    const float aspectDelta = std::abs((aExtent.width / aExtent.height) - (aOverlayWidth / aOverlayHeight));
    const float widthRatio = aOverlayWidth / aExtent.width;
    const float heightRatio = aOverlayHeight / aExtent.height;
    return aspectDelta <= 0.1F && widthRatio >= 0.45F && widthRatio <= 3.6F && heightRatio >= 0.45F &&
           heightRatio <= 3.6F;
}

[[nodiscard]] inline Verdict Judge(const Candidate& aCandidate, const float aOverlayWidth, const float aOverlayHeight)
{
    if (FamilyOf(aCandidate.format) == Family::None || !aCandidate.depthStencil || !aCandidate.twoDimensional ||
        aCandidate.mipLevels != 1 || aCandidate.arraySize != 1)
    {
        return Verdict::NotDepth;
    }
    if (aCandidate.samples != 1)
    {
        return Verdict::Multisampled;
    }
    if (aCandidate.draws == 0 || (aCandidate.vertices <= 3 && aCandidate.indirectDraws == 0))
    {
        return Verdict::NotDrawn;
    }
    if (!PictureShaped(DrawnExtent(aCandidate), aOverlayWidth, aOverlayHeight))
    {
        return Verdict::WrongShape;
    }
    return Verdict::Ok;
}

/// ReShade's rule: compare vertices, which favour the scene over the many small
/// passes a frame also draws, unless indirect draws (whose vertex counts are not
/// known on the CPU) are a third or more of the work, then compare draws.
[[nodiscard]] inline bool CompareByVertices(std::span<const Candidate> aCandidates)
{
    uint64_t draws = 0;
    uint64_t indirect = 0;
    for (const auto& candidate : aCandidates)
    {
        draws += candidate.draws;
        indirect += candidate.indirectDraws;
    }
    return indirect * 3U < draws;
}

[[nodiscard]] inline double Score(const Candidate& aCandidate, const bool aByVertices)
{
    const double work = aByVertices ? static_cast<double>(aCandidate.vertices) : static_cast<double>(aCandidate.draws);
    // A buffer the game also samples (ambient occlusion, lighting, fog, the
    // upscaler) is the scene's far more often than not: a small edge for ties.
    return aCandidate.readByShaders ? work * 1.05 : work;
}

/// The indices of the usable candidates, best first.
[[nodiscard]] inline std::vector<size_t> Rank(std::span<const Candidate> aCandidates, const float aOverlayWidth,
                                              const float aOverlayHeight)
{
    std::vector<size_t> ranked;
    for (size_t index = 0; index < aCandidates.size(); ++index)
    {
        if (Judge(aCandidates[index], aOverlayWidth, aOverlayHeight) == Verdict::Ok)
        {
            ranked.push_back(index);
        }
    }
    const bool byVertices = CompareByVertices(aCandidates);
    std::stable_sort(ranked.begin(), ranked.end(), [&](const size_t aLeft, const size_t aRight) {
        return Score(aCandidates[aLeft], byVertices) > Score(aCandidates[aRight], byVertices);
    });
    return ranked;
}

/// The choice for this frame. `aPrevious` (an id, 0 for none) is kept while it is
/// still usable and does at least half the work of the best, so that two similar
/// buffers do not take turns. `aOverride` (-1 for none) picks the n-th ranked one,
/// wrapping, for the in-game "next candidate" key. Returns the index into
/// `aCandidates`, or `aCandidates.size()` when there is nothing to choose.
[[nodiscard]] inline size_t Choose(std::span<const Candidate> aCandidates, const std::vector<size_t>& aRanked,
                                   const uint64_t aPrevious, const int aOverride)
{
    if (aRanked.empty())
    {
        return aCandidates.size();
    }
    if (aOverride >= 0)
    {
        return aRanked[static_cast<size_t>(aOverride) % aRanked.size()];
    }
    const bool byVertices = CompareByVertices(aCandidates);
    const size_t best = aRanked.front();
    if (aPrevious != 0)
    {
        for (const size_t index : aRanked)
        {
            if (aCandidates[index].id == aPrevious &&
                Score(aCandidates[index], byVertices) * 2.0 >= Score(aCandidates[best], byVertices))
            {
                return index;
            }
        }
    }
    return best;
}

/// Each buffer's work over the last several presents, rather than in one.
///
/// A game whose frames span several presents draws into its scene's buffer in some
/// of them only. With frame generation the game renders one frame for every two to
/// four images presented, and its work lands between whichever presents it falls
/// between: the scene's in one, a small pass's in the next, nothing in a third.
/// Judged one present at a time, the choice took turns between the scene and the
/// small pass, and had nothing at all in between. Here every remembered buffer's
/// counts shrink by `kKeep` at each present and that present's are added: the
/// scene's buffer stays the one that did the most work, and a present with no depth
/// work changes nothing. `Candidates()` are these sums, for `Rank` and `Choose`.
class History
{
public:
    static constexpr double kKeep = 0.875;  ///< per present: the work of about the last eight
    static constexpr double kForget = 0.05; ///< a buffer whose draws have shrunk below this is forgotten
    static constexpr size_t kCapacity = 32;

    /// One present's candidates: what each buffer did since the previous present.
    void Add(std::span<const Candidate> aPresent)
    {
        for (Entry& entry : m_entries)
        {
            entry.draws *= kKeep;
            entry.indirect *= kKeep;
            entry.vertices *= kKeep;
            entry.read *= kKeep;
        }
        for (const Candidate& candidate : aPresent)
        {
            Entry* entry = Find(candidate.id);
            if (entry != nullptr && !SameShape(entry->latest, candidate))
            {
                *entry = Entry{}; // another buffer, made where a gone one was
            }
            if (entry == nullptr)
            {
                entry = Make();
            }
            const float viewportWidth = candidate.viewportWidth >= 1.0F ? candidate.viewportWidth
                                                                        : entry->latest.viewportWidth;
            const float viewportHeight = candidate.viewportHeight >= 1.0F ? candidate.viewportHeight
                                                                          : entry->latest.viewportHeight;
            entry->latest = candidate;
            entry->latest.viewportWidth = viewportWidth;
            entry->latest.viewportHeight = viewportHeight;
            entry->draws += candidate.draws;
            entry->indirect += candidate.indirectDraws;
            entry->vertices += static_cast<double>(candidate.vertices);
            entry->read += candidate.readByShaders ? 1.0 : 0.0;
        }
        std::erase_if(m_entries, [](const Entry& aEntry) { return aEntry.draws < kForget; });
        m_candidates.clear();
        for (const Entry& entry : m_entries)
        {
            Candidate sum = entry.latest;
            sum.draws = Round32(entry.draws);
            sum.indirectDraws = Round32(entry.indirect);
            sum.vertices = static_cast<uint64_t>(std::min(std::floor(entry.vertices + 0.5), 1.8e19));
            sum.readByShaders = entry.read >= 0.25;
            m_candidates.push_back(sum);
        }
    }

    [[nodiscard]] const std::vector<Candidate>& Candidates() const { return m_candidates; }

    void Clear()
    {
        m_entries.clear();
        m_candidates.clear();
    }

private:
    struct Entry
    {
        Candidate latest; ///< its shape, format and viewport, from its last use
        double draws{};
        double indirect{};
        double vertices{};
        double read{};
    };

    [[nodiscard]] static bool SameShape(const Candidate& aLeft, const Candidate& aRight)
    {
        return aLeft.width == aRight.width && aLeft.height == aRight.height && aLeft.format == aRight.format &&
               aLeft.samples == aRight.samples && aLeft.mipLevels == aRight.mipLevels &&
               aLeft.arraySize == aRight.arraySize && aLeft.depthStencil == aRight.depthStencil &&
               aLeft.twoDimensional == aRight.twoDimensional;
    }

    [[nodiscard]] static uint32_t Round32(const double aValue)
    {
        return static_cast<uint32_t>(std::min(std::floor(aValue + 0.5), 4294967295.0));
    }

    Entry* Find(const uint64_t aId)
    {
        for (Entry& entry : m_entries)
        {
            if (entry.latest.id == aId)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    /// Room for a buffer not seen before: a new entry, or the idlest one's.
    Entry* Make()
    {
        if (m_entries.size() < kCapacity)
        {
            return &m_entries.emplace_back();
        }
        Entry* idlest = &m_entries.front();
        for (Entry& entry : m_entries)
        {
            if (entry.draws < idlest->draws)
            {
                idlest = &entry;
            }
        }
        *idlest = Entry{};
        return idlest;
    }

    std::vector<Entry> m_entries;
    std::vector<Candidate> m_candidates;
};

// -- when to copy -------------------------------------------------------------------

/// What is known at a present about the chosen buffer.
struct CopyInputs
{
    bool seenThisPresent{};   ///< the lists run since the last present drew into it, cleared it or moved it
    bool writtenSinceCopy{};  ///< drawn into or cleared since the overlay last copied it (or never copied)
    bool aliasedAway{};       ///< its memory belongs to another resource at present
    bool everSettled{};       ///< the game has been seen to move it into a read-only state
    bool settledSinceClear{}; ///< ...since its last clear: this frame's depth is finished
    bool readOnlyNow{};       ///< in a read-only state at present
    uint32_t waits{};         ///< presents since the last copy at which it had changed but was mid-frame
};

/// A buffer found mid-frame at this many presents in a row is copied anyway, so a
/// game with an order this does not expect loses steadiness, not the test.
inline constexpr uint32_t kForceCopyAfterWaits = 8;

/// A copy is used for the screens drawn at every present until a newer one is made,
/// for at most this many presents: several frames of a game showing four images per
/// frame it renders, and a fraction of a second at any rate, so a scene the game has
/// stopped drawing does not keep hiding screens.
inline constexpr uint64_t kCopyFreshForPresents = 30;

/// Copy the chosen buffer at this present, or keep the copy the overlay has.
///
/// Only a buffer the game used since the last present is touched (one it has let go
/// of may be gone), and only when it changed since the last copy. A buffer the game
/// reads afterwards, as a scene's depth is read by its lighting, ambient occlusion
/// and upscaler, is copied only once its frame's depth is finished: moved into a
/// read-only state since its last clear, or left in one. A frame whose work spans
/// presents would otherwise be copied half drawn, and whatever was drawn last (a
/// hand, a body) would flicker in front of the screen.
[[nodiscard]] inline bool ShouldCopy(const CopyInputs& aInputs)
{
    if (!aInputs.seenThisPresent || !aInputs.writtenSinceCopy || aInputs.aliasedAway)
    {
        return false;
    }
    if (!aInputs.everSettled || aInputs.settledSinceClear || aInputs.readOnlyNow)
    {
        return true;
    }
    return aInputs.waits >= kForceCopyAfterWaits;
}

// -- what a value means -------------------------------------------------------------

enum class Convention : uint8_t
{
    Unknown,
    Reversed,     ///< cleared to 0: near is 1, far is 0 (A = 0)
    Conventional, ///< cleared to 1: near is 0, far is 1 (A = 1)
};

[[nodiscard]] constexpr const char* Describe(const Convention aConvention)
{
    switch (aConvention)
    {
    case Convention::Reversed: return "reversed";
    case Convention::Conventional: return "conventional";
    case Convention::Unknown: break;
    }
    return "unknown";
}

/// The convention a clear value says the buffer runs in.
[[nodiscard]] constexpr Convention FromClear(const float aClearDepth)
{
    if (aClearDepth == 0.0F)
    {
        return Convention::Reversed;
    }
    if (aClearDepth == 1.0F)
    {
        return Convention::Conventional;
    }
    return Convention::Unknown;
}

/// One point on a screen: the inverse of the screen's own view depth there, from
/// the producer, and the device depth the game drew at that pixel.
struct Sample
{
    double inverseDepth{};
    double device{};
};

/// Measures B (device = A + B / z) from screens the player looks at.
///
/// Each screen drawn in a frame contributes a group: a few points in the middle
/// of its glass. For each point |device - A| times the screen's own view depth is
/// B where the game drew the glass, more wherever something is in front of it
/// (nearer means a larger |device - A|), and less where the point sees past the
/// screen. Where the glass is, every point says the same B however the screen is
/// turned, so a group counts only when several of its points agree, and it counts
/// the lowest value they agree on: points covered by something nearer only ever
/// say more.
///
/// That a group agrees with itself is not enough on its own. A screen whose panel
/// the game did not draw that session (a cinema prop the engine left out of its
/// depth) has no glass behind its points, and in third person what is in front of
/// the middle of a screen is the player's own body: points on the back of a torso
/// agree with each other, and a player who stands watching agrees with himself
/// for as long as he stands there. One session locked on ten times the real B
/// that way, and every screen nearer than about thirty metres was drawn over the
/// player. What tells them apart is distance. The body stays where it is in front
/// of the camera while the screen does not, so its value follows the screen's
/// distance; the glass's does not. A value is therefore only taken from groups
/// that saw their screens from distances at least `kDepthSpan` apart and still
/// agreed, which no body, hand or wall in front of a screen can do.
///
/// Reversed depth starts from this game's own near plane, `kAssumedReversedB`
/// (2 cm: every lock measured in game was 0.01985 to 0.01998), so the test is right
/// from the first frame and a screen with no panel cannot talk it into anything
/// else. A measurement replaces it by the rule above, and after that it moves only
/// to a value a whole window agrees on, so a hand held over a screen for a few
/// seconds cannot drag it. A value is used only between 5 mm and 2 m (any real
/// camera's near plane).
class Calibration
{
public:
    static constexpr size_t kWindow = 96;      ///< groups remembered
    static constexpr size_t kToLock = 16;      ///< groups that must agree first
    static constexpr double kAgreement = 0.04; ///< interquartile range / median
    static constexpr double kMove = 0.03;      ///< a locked value moves only by more than this
    static constexpr double kMinimum = 0.005;
    static constexpr double kMaximum = 2.0;
    /// Points of one group that must agree with each other for the group to count...
    static constexpr size_t kAgreeingPoints = 4;
    /// ...to within this fraction (the glass's points agree to a fraction of that).
    static constexpr double kPointAgreement = 0.02;
    /// The farthest screen of the groups a value is taken from, over the nearest.
    static constexpr double kDepthSpan = 1.2;
    /// Where reversed depth starts: this game's near plane, in metres.
    static constexpr double kAssumedReversedB = 0.02;

    /// Starts again when the convention changes: the products mean something else.
    /// Reversed depth starts from the assumed near plane, conventional unmeasured.
    void SetConvention(const Convention aConvention)
    {
        if (aConvention != m_convention)
        {
            m_convention = aConvention;
            m_groups.clear();
            m_assumed = aConvention == Convention::Reversed;
            m_locked = m_assumed;
            m_b = m_assumed ? kAssumedReversedB : 0.0;
            m_spread = 0.0;
            m_median = 0.0;
            m_nearest = 0.0;
            m_farthest = 0.0;
        }
    }

    [[nodiscard]] Convention Direction() const { return m_convention; }

    /// A = 0 for reversed depth, 1 for conventional.
    [[nodiscard]] double A() const { return m_convention == Convention::Conventional ? 1.0 : 0.0; }

    /// B as the shader wants it: signed (negative for conventional depth). Only
    /// meaningful when `Locked()`.
    [[nodiscard]] double B() const { return m_convention == Convention::Conventional ? -m_b : m_b; }

    /// There is a value to use: the assumed one or a measured one.
    [[nodiscard]] bool Locked() const { return m_locked; }
    /// The value in use is the assumed near plane: nothing has measured it yet.
    [[nodiscard]] bool Assumed() const { return m_assumed; }
    [[nodiscard]] size_t Groups() const { return m_groups.size(); }
    [[nodiscard]] double LastSpread() const { return m_spread; }
    [[nodiscard]] double LastMedian() const { return m_median; }
    /// The nearest and farthest screen (metres) of the groups last looked at.
    [[nodiscard]] double LastNearest() const { return m_nearest; }
    [[nodiscard]] double LastFarthest() const { return m_farthest; }

    /// One screen's points from one frame. Returns whether the group was used.
    bool AddGroup(std::span<const Sample> aSamples)
    {
        if (m_convention == Convention::Unknown)
        {
            return false;
        }
        const double a = A();
        // One screen gives kSamplesPerScreen points; more than this many are not read.
        std::array<Point, 16> points{};
        size_t count = 0;
        for (const auto& sample : aSamples)
        {
            if (count == points.size())
            {
                break;
            }
            if (!std::isfinite(sample.inverseDepth) || !(sample.inverseDepth > 0.0) || !std::isfinite(sample.device) ||
                sample.device < 0.0 || sample.device > 1.0)
            {
                continue;
            }
            const double separation = std::abs(sample.device - a);
            // Exactly the clear value: nothing was drawn there (or the sky).
            if (!(separation > 1.0e-7))
            {
                continue;
            }
            points[count++] = Point{separation / sample.inverseDepth, 1.0 / sample.inverseDepth};
        }
        std::sort(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(count),
                  [](const Point& aLeft, const Point& aRight) { return aLeft.product < aRight.product; });
        // The lowest value enough points agree on. A lone point lower than the rest
        // (past an edge of the glass) is not it, points that see far past the
        // screen (below any near plane) are not it, and covered points are higher.
        for (size_t first = 0; first + kAgreeingPoints <= count; ++first)
        {
            const double value = points[first].product;
            if (!(value >= kMinimum))
            {
                continue;
            }
            if (!(value <= kMaximum))
            {
                return false;
            }
            size_t agreeing = 0;
            for (size_t other = first; other < count && points[other].product <= value * (1.0 + kPointAgreement);
                 ++other)
            {
                ++agreeing;
            }
            if (agreeing < kAgreeingPoints)
            {
                continue;
            }
            m_groups.push_back(Group{value, points[first].depth});
            if (m_groups.size() > kWindow)
            {
                m_groups.erase(m_groups.begin());
            }
            Update();
            return true;
        }
        return false;
    }

private:
    struct Point
    {
        double product{};
        double depth{};
    };

    struct Group
    {
        double value{};
        double depth{};
    };

    void Update()
    {
        if (m_groups.size() < kToLock)
        {
            return;
        }
        // Until something is measured every group so far decides (at least kToLock
        // of them); after that only a whole window moves the value.
        const bool measured = m_locked && !m_assumed;
        if (measured && m_groups.size() < kWindow)
        {
            return;
        }
        std::vector<double> sorted;
        sorted.reserve(m_groups.size());
        for (const auto& group : m_groups)
        {
            sorted.push_back(group.value);
        }
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&sorted](const double aFraction) {
            const double position = aFraction * static_cast<double>(sorted.size() - 1);
            const auto low = static_cast<size_t>(std::floor(position));
            const size_t high = std::min(sorted.size() - 1, low + 1);
            const double t = position - static_cast<double>(low);
            return sorted[low] + ((sorted[high] - sorted[low]) * t);
        };
        const double median = at(0.5);
        const double spread = (at(0.75) - at(0.25)) / median;
        m_median = median;
        m_spread = spread;
        if (!(spread <= kAgreement))
        {
            return;
        }
        // How far apart the screens of the groups that agree were: a quarter of the
        // groups may be something else, and they must not supply the distances.
        double nearest = 0.0;
        double farthest = 0.0;
        for (const auto& group : m_groups)
        {
            if (std::abs(group.value - median) <= kAgreement * median)
            {
                nearest = nearest == 0.0 ? group.depth : std::min(nearest, group.depth);
                farthest = std::max(farthest, group.depth);
            }
        }
        m_nearest = nearest;
        m_farthest = farthest;
        if (!(nearest > 0.0) || !(farthest >= nearest * kDepthSpan))
        {
            return;
        }
        if (!measured)
        {
            m_b = median;
            m_locked = true;
            m_assumed = false;
            return;
        }
        if (std::abs(median - m_b) > kMove * m_b)
        {
            m_b = median;
        }
    }

    Convention m_convention{Convention::Unknown};
    std::vector<Group> m_groups;
    bool m_locked{};
    bool m_assumed{};
    double m_b{};
    double m_spread{};
    double m_median{};
    double m_nearest{};
    double m_farthest{};
};

/// Where on a screen the calibration looks: a 3 x 3 grid over the middle of the
/// glass (texture coordinates), well inside its edges, so that a quad drawn a
/// little larger than the prop's glass never samples the wall behind it.
inline constexpr std::array<float, 3> kSampleCoordinates{0.35F, 0.5F, 0.65F};
inline constexpr size_t kSamplesPerScreen = kSampleCoordinates.size() * kSampleCoordinates.size();
} // namespace op77::WebUI::SceneDepthPolicy
