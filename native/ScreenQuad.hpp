#pragma once

// The four world-space corners of a screen, and which pixel goes on each.
//
// ---------------------------------------------------------------------------
// WHY THIS IS A SEPARATE, PURE HEADER
// ---------------------------------------------------------------------------
//
// Getting this wrong is invisible in every way that matters: the screen is in
// the right place, the right size, on the right prop, drawing every frame -- and
// the picture on it is upside down, or mirrored, or both. Nothing logs it, no
// test downstream of the texture can see it, and the only instrument that can
// report it is a person reading text on a television. That is exactly the class
// of code that has to be pinned by a unit test rather than by a session, so the
// mapping lives here, free of the engine, and is tested directly
// (`client/tests/ScreenQuadTests.cpp`).
//
// ---------------------------------------------------------------------------
// WHAT WAS TRIED FIRST, AND WHY IT IS NOT HERE
// ---------------------------------------------------------------------------
//
// The first attempt encoded a measurement of the prop's own mesh: the vertex
// buffer of `television_a_16x9_screen_a` decodes to
//
//     local X = -0.580 (a side)   Z = 0.090 (a side)   u = 1.00   v = 0.00
//     local X = +0.580 (a side)   Z = 0.090 (a side)   u = 0.00   v = 0.00
//     local X = -0.580 (a side)   Z = 0.750 (a side)   u = 1.00   v = 1.00
//     local X = +0.580 (a side)   Z = 0.750 (a side)   u = 0.00   v = 1.00
//
// so the mesh's own texture coordinates run one way along X and the other along
// Z. That was read as "the picture's left is local -X and its top is local +Z",
// and the corners were ordered from it.
//
// It was wrong, and it was wrong in a way the measurement could never have
// caught. Those are the coordinates of the GAME'S material texture, not of the
// CEF surface this module composites. Turning them into an orientation needs one
// more fact -- whether that texture is stored top-down or bottom-up -- and that
// fact was assumed rather than verified. Assuming it wrongly produces a perfect
// half-turn, which is precisely what was reported after the change shipped
// ("url link still not playing, its showing ... inverted text ... looks like its
// being rendered backwards"). The decode was correct and the conclusion drawn
// from it was not.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES INSTEAD: ASK THE FRAME
// ---------------------------------------------------------------------------
//
// The client already projects all four corners with the engine's own camera. That
// projection answers the question directly and needs no convention at all:
//
//   * the picture's LEFT edge belongs on whichever side of the panel lands on
//     the left of the frame;
//   * the picture's TOP row belongs on whichever side of the panel lands at the
//     top of the frame.
//
// Nothing about handedness, mesh UVs, texture storage, or which local axis the
// catalogue happens to call `right` takes part. The rule is stated in the space
// the player actually looks at, so it cannot be a half-turn out, and it stays
// correct when the panel is tilted, when the player's view is rolled, and when
// the player walks round to the back of the set -- where the panel's own left and
// right are swapped as seen, and the picture follows the frame rather than
// turning into gibberish.
//
// The one place the rule needs help is edge-on: with the panel nearly parallel to
// the view direction both edges project to nearly the same place, the comparison
// is noise, and the answer can flap. So an answer inside `kOrientationDeadband`
// is discarded and the previous one kept -- the media module holds each screen's
// decision across frames for exactly that. Near edge-on the picture is a few
// pixels wide and unreadable either way; what matters is that it does not
// flicker, and that the state it settles into is the one it came from.

#include <cmath>

namespace op77::Api::ScreenQuad
{
struct Vec3
{
    float x{}, y{}, z{};
};

/// A rotation quaternion in the engine's own field order.
struct Rotation
{
    float i{}, j{}, k{}, r{1.0F};
};

/// The screen rectangle in the prop's local frame, in metres: its centre, its
/// two axes, and its full width and height. The axes are not required to be unit
/// length or exactly perpendicular; they are squared up below.
struct Quad
{
    float offset[3]{0.0F, 0.0F, 1.0F};
    float right[3]{1.0F, 0.0F, 0.0F};
    float up[3]{0.0F, 0.0F, 1.0F};
    float width{1.2F};
    float height{0.68F};
    /// Which way the panel's picture looks, as a direction in the prop's own
    /// local frame -- `{0, 1, 0}` for a television whose display is on its +Y
    /// face. A zero vector, the default, means the record does not say, and a
    /// screen that does not say is not gated: it draws from both sides exactly as
    /// it did before this existed.
    ///
    /// It is data and not a derived convention on purpose. Which local axis a
    /// prop's display faces along is a property of the ASSET, and this file has
    /// already been wrong once by assuming such a thing instead of measuring it
    /// (see the header above): a mesh's own UVs said which way the game's material
    /// texture ran and nothing said which way up it was stored, so the assumption
    /// produced exactly the half-turn it was supposed to prevent. A direction
    /// written in the record is the same kind of number as the rest of the
    /// rectangle: taken off the asset, checkable against it, and fixable in one
    /// place when the asset disagrees.
    float faces[3]{0.0F, 0.0F, 0.0F};
};

/// Where the picture's own axes run, relative to the panel's.
///
/// Named for what the picture does, not for the panel: `flipU` is set when the
/// picture's left-to-right runs along MINUS the record's `right` axis, `flipV`
/// when its top-to-bottom runs along MINUS the record's `up` axis. Both false is
/// the identity mapping against the record's axes.
struct Orientation
{
    bool flipU{};
    bool flipV{};
};

/// How far apart, in normalised viewport units, the two candidate edges must
/// project before their order is believed.
///
/// Roughly ten pixels of a 1080-line frame. A screen that is 10 px wide is not
/// showing anybody a picture, so the comparison it would decide is not worth
/// changing an answer for -- and the alternative, deciding on noise, is a picture
/// that flips back and forth while the player walks past a set.
inline constexpr float kOrientationDeadband = 0.01F;

/// The index into `Corners`' output for a (side, row) pair, where `side` is +1 for
/// the `+right` edge and `row` is +1 for the `+up` edge.
[[nodiscard]] constexpr int CornerIndex(const int aSide, const int aRow)
{
    return (aSide < 0 ? (aRow < 0 ? 0 : 3) : (aRow < 0 ? 1 : 2));
}

/// The same rotation product the engine produces, spelled once.
///
/// `v + 2 * (q.ijk x (q.ijk x v + q.r * v))`. Reproducing the engine's axis
/// convention by hand instead would be a second place to be wrong about
/// handedness -- and handedness is exactly what this file does not depend on.
[[nodiscard]] inline Vec3 Rotate(const Rotation& aOrientation, const Vec3& aValue)
{
    const Vec3 axis{aOrientation.i, aOrientation.j, aOrientation.k};
    const auto cross = [](const Vec3& a, const Vec3& b) {
        return Vec3{
            (a.y * b.z) - (a.z * b.y),
            (a.z * b.x) - (a.x * b.z),
            (a.x * b.y) - (a.y * b.x)};
    };
    const auto add = [](const Vec3& a, const Vec3& b) {
        return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
    };
    const auto scale = [](const Vec3& a, const float f) {
        return Vec3{a.x * f, a.y * f, a.z * f};
    };
    const auto first = cross(axis, aValue);
    const auto second = cross(axis, add(first, scale(aValue, aOrientation.r)));
    return add(aValue, scale(second, 2.0F));
}

[[nodiscard]] inline float Length(const Vec3& aValue)
{
    return std::sqrt((aValue.x * aValue.x) + (aValue.y * aValue.y) + (aValue.z * aValue.z));
}

/// Normalises in place, and reports whether the vector was a direction at all.
[[nodiscard]] inline bool Normalise(Vec3& aValue)
{
    const float magnitude = Length(aValue);
    if (!(magnitude > 1.0e-4F) || !std::isfinite(magnitude))
    {
        return false;
    }
    aValue = Vec3{aValue.x / magnitude, aValue.y / magnitude, aValue.z / magnitude};
    return true;
}

[[nodiscard]] inline float Dot(const Vec3& a, const Vec3& b)
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

/// The four corners of the quad in world space. The order is fixed and each
/// index names its own tag, where a corner is
/// `centre + side * (width / 2) * right + row * (height / 2) * up`:
///
///     out[CornerIndex(-1, -1)] = the -right, -up corner
///     out[CornerIndex(+1, -1)] = the +right, -up corner
///     out[CornerIndex(+1, +1)] = the +right, +up corner
///     out[CornerIndex(-1, +1)] = the -right, +up corner
///
/// False when the quad is not a rectangle this module can draw: a non-positive
/// side, a zero-length axis, or an up axis parallel to right. The caller reports
/// `quad_invalid` for that, which is the same answer it gave before this was
/// extracted.
[[nodiscard]] inline bool Corners(
    const Quad& aQuad,
    const Vec3& aPosition,
    const Rotation& aOrientation,
    Vec3 (&aOut)[4])
{
    if (!(aQuad.width > 0.01F) || !(aQuad.height > 0.01F))
    {
        return false;
    }

    auto right = Rotate(aOrientation, Vec3{aQuad.right[0], aQuad.right[1], aQuad.right[2]});
    if (!Normalise(right))
    {
        return false;
    }
    auto up = Rotate(aOrientation, Vec3{aQuad.up[0], aQuad.up[1], aQuad.up[2]});
    // Remove the component of `up` along `right`. A perpendicular pair passes
    // through unchanged; a skewed one is squared up rather than refused, because
    // the alternative is a record that silently draws nothing.
    up = Vec3{
        up.x - (right.x * Dot(up, right)),
        up.y - (right.y * Dot(up, right)),
        up.z - (right.z * Dot(up, right))};
    if (!Normalise(up))
    {
        return false;
    }

    const auto offset = Rotate(aOrientation, Vec3{aQuad.offset[0], aQuad.offset[1], aQuad.offset[2]});
    const Vec3 centre{
        aPosition.x + offset.x, aPosition.y + offset.y, aPosition.z + offset.z};

    const auto halfRight = Vec3{right.x * aQuad.width * 0.5F, right.y * aQuad.width * 0.5F,
                                right.z * aQuad.width * 0.5F};
    const auto halfUp = Vec3{up.x * aQuad.height * 0.5F, up.y * aQuad.height * 0.5F,
                             up.z * aQuad.height * 0.5F};

    const auto corner = [&](const int aSide, const int aRow) {
        return Vec3{
            centre.x + (halfRight.x * static_cast<float>(aSide)) +
                (halfUp.x * static_cast<float>(aRow)),
            centre.y + (halfRight.y * static_cast<float>(aSide)) +
                (halfUp.y * static_cast<float>(aRow)),
            centre.z + (halfRight.z * static_cast<float>(aSide)) +
                (halfUp.z * static_cast<float>(aRow))};
    };

    for (int side = -1; side <= 1; side += 2)
    {
        for (int row = -1; row <= 1; row += 2)
        {
            aOut[CornerIndex(side, row)] = corner(side, row);
        }
    }
    return true;
}

/// Which side of the panel the eye is on, when the record says which side carries
/// the picture.
///
/// ---------------------------------------------------------------------------
/// WHY THIS EXISTS: A RECTANGLE HAS TWO SIDES
/// ---------------------------------------------------------------------------
/// Nothing in the projection says which side of a quad is its front, so a screen
/// stood in front of was equally a screen seen from behind -- with its video
/// playing on the back of the cabinet, which is what a player reported ("tvs are
/// playing videos on both sides"). The orientation rule below deliberately keeps
/// the picture readable from either side, so it cannot answer this: by the time
/// the picture is the right way up, the fact that it is on the wrong side has
/// already been erased.
///
/// Only the record can answer it, and the answer is not derived here. `Quad::faces`
/// is the direction the display looks along in the prop's own frame; this applies
/// the prop's rotation to it and compares it with where the eye is. A record that
/// declares no direction is reported `known = false` and is drawn as it was,
/// because a screen that vanishes on a guess is worse than a screen seen through
/// its own cabinet.
struct Facing
{
    /// False when the record does not say which side is the front. There is then
    /// nothing to answer, and the caller must draw the screen.
    bool known{};
    /// True when the eye is behind the panel's picture. Only meaningful when
    /// `known`.
    bool away{};
};

/// The eye's side of the panel. `aEye` is a world-space point (the camera).
[[nodiscard]] inline Facing Faces(
    const Quad& aQuad,
    const Vec3& aPosition,
    const Rotation& aOrientation,
    const Vec3& aEye)
{
    Facing out{};
    Vec3 declared{aQuad.faces[0], aQuad.faces[1], aQuad.faces[2]};
    if (!Normalise(declared))
    {
        return out;
    }

    const auto offset = Rotate(aOrientation, Vec3{aQuad.offset[0], aQuad.offset[1], aQuad.offset[2]});
    const Vec3 centre{
        aPosition.x + offset.x, aPosition.y + offset.y, aPosition.z + offset.z};
    const Vec3 outward = Rotate(aOrientation, declared);

    out.known = true;
    // `<= 0` and not `< 0`: a camera exactly in the panel's own plane is a screen
    // seen edge-on, a few pixels wide, and the side it is on is not a question
    // anybody can answer from the picture. Refusing it there keeps the answer
    // stable instead of flapping through the plane.
    out.away = Dot(outward, Vec3{aEye.x - centre.x, aEye.y - centre.y, aEye.z - centre.z}) <= 0.0F;
    return out;
}

/// Which way the picture runs, from where the panel's own corners landed.
///
/// `aScreen` is the four corners `Corners` just produced (eight floats, x and y
/// interleaved), in that same order, as
/// normalised viewport coordinates -- 0..1 with the origin top-left, the space
/// every other overlay producer publishes in. `aPrevious` is this screen's last
/// decision, and passing it back in is what makes the answer stable near edge-on
/// (see `kOrientationDeadband`); pass a default-constructed value the first time.
[[nodiscard]] inline Orientation Orient(
    const float* const aScreen,
    const Orientation& aPrevious = Orientation{})
{
    // The panel's own edges, as the frame sees them. With the corner order above,
    // the +right pair is {1, 2} and the +up pair is {2, 3}.
    const float plusRightX = (aScreen[2] + aScreen[4]) * 0.5F;
    const float minusRightX = (aScreen[0] + aScreen[6]) * 0.5F;
    const float plusUpY = (aScreen[5] + aScreen[7]) * 0.5F;
    const float minusUpY = (aScreen[1] + aScreen[3]) * 0.5F;

    Orientation out = aPrevious;
    // Screen y grows downwards, so the `+up` edge is at the top of the frame --
    // and the picture is the right way up -- when it has the SMALLER y.
    if (std::abs(plusRightX - minusRightX) > kOrientationDeadband)
    {
        out.flipU = plusRightX < minusRightX;
    }
    if (std::abs(plusUpY - minusUpY) > kOrientationDeadband)
    {
        out.flipV = plusUpY > minusUpY;
    }
    return out;
}

/// The index into `aScreen` (and into `Corners`' output) of the corner that
/// carries the texture coordinate `(u, v)`, for a given orientation. `u`/`v` are
/// 0 or 1 and the texture's origin is top-left, the way a browser page's is.
[[nodiscard]] constexpr int CornerForTexture(const Orientation aOrientation, const int aU,
                                             const int aV)
{
    // (u = 0) is the picture's left edge, which sits on the -right edge unless the
    // picture is mirrored against the record's axes. (v = 0) is its top row, on
    // the +up edge unless the picture is turned over.
    const int side = (aU == 0) == !aOrientation.flipU ? -1 : 1;
    const int row = (aV == 0) == !aOrientation.flipV ? 1 : -1;
    return CornerIndex(side, row);
}
} // namespace op77::Api::ScreenQuad
