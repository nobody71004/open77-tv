// Pins which pixel of a television's picture goes on which corner of the panel.
//
// This is the one part of the media feature that cannot fail loudly. A screen
// whose texture is mapped a half-turn out is in the right place, the right size,
// on the right prop, drawing at the right frame rate -- and reads backwards to
// the only instrument that can tell: a person looking at it. That happened
// twice. First the mapping was written straight, then it was derived from the
// prop mesh's own UVs, and both times the report was the same: unreadable text
// on a screen every log line said was drawing.
//
// So this file now pins the rule that replaced both: the picture's left edge goes
// on whichever side of the panel lands on the left of the FRAME, and its top row
// on whichever side lands at the top. That is stated in the space the player
// looks at, so no mesh convention, texture storage order or handedness can put it
// a half-turn out. The property test at the end is the real assertion -- for a
// panel seen from the front, from behind, and upside down, the corner that gets
// texture (0,0) is always the top-left of the frame.
//
// The same rule cannot answer the other half of the question -- *which* of the
// panel's two sides the picture belongs on -- because by the time the picture is
// upright that fact has been erased. Only the asset knows, so a record declares it
// and `Faces` is the one dot product that reads the declaration; its cases are
// below, including the one that matters most: a record that declares nothing is
// never gated.

#include "api/ScreenQuad.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace op77::Api::ScreenQuad;

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
constexpr float kTolerance = 1.0e-4F;

bool Near(const float aLeft, const float aRight)
{
    return std::abs(aLeft - aRight) < kTolerance;
}

bool Near(const Vec3& aLeft, const Vec3& aRight)
{
    return Near(aLeft.x, aRight.x) && Near(aLeft.y, aRight.y) && Near(aLeft.z, aRight.z);
}

/// The catalogue's upright 16:9 television, at the prop's own origin.
Quad Television()
{
    Quad quad;
    // `offset` is the screen centre inside the prop: 0.1154 m out along the
    // prop's +Y (the front face) and 0.42 m up.
    quad.offset[0] = 0.0F;
    quad.offset[1] = 0.115394F;
    quad.offset[2] = 0.42F;
    quad.right[0] = 1.0F;
    quad.right[1] = 0.0F;
    quad.right[2] = 0.0F;
    quad.up[0] = 0.0F;
    quad.up[1] = 0.0F;
    quad.up[2] = 1.0F;
    quad.width = 1.16F;
    quad.height = 0.66F;
    return quad;
}

/// The panel's four corners as they might land on a frame, given where each of
/// the panel's own edges was seen. Indexed exactly as `Corners` outputs them.
void Screen(std::array<float, 8>& aOut, const float aMinusRightX, const float aPlusRightX,
            const float aMinusUpY, const float aPlusUpY)
{
    aOut[0] = aMinusRightX; aOut[1] = aMinusUpY; // (-right, -up)
    aOut[2] = aPlusRightX;  aOut[3] = aMinusUpY; // (+right, -up)
    aOut[4] = aPlusRightX;  aOut[5] = aPlusUpY;  // (+right, +up)
    aOut[6] = aMinusRightX; aOut[7] = aPlusUpY;  // (-right, +up)
}

/// The same, but with the edges named the other way round -- what the frame sees
/// when the player has walked round to the back of the set.
void ScreenFromBehind(std::array<float, 8>& aOut)
{
    Screen(aOut, 0.3F, 0.7F, 0.7F, 0.3F);
}

/// The frame the player is standing in front of: the record's `+right` axis is
/// on the left of the screen and its `+up` axis is at the top.
void ScreenFromFront(std::array<float, 8>& aOut)
{
    Screen(aOut, 0.7F, 0.3F, 0.7F, 0.3F);
}

/// The view is rolled: the panel's own top edge is at the BOTTOM of the frame.
void ScreenUpsideDown(std::array<float, 8>& aOut)
{
    Screen(aOut, 0.7F, 0.3F, 0.3F, 0.7F);
}

/// Where a texture coordinate ended up, as a point.
std::array<float, 2> TexturePoint(const std::array<float, 8>& aScreen,
                                  const Orientation aOrientation, const int aU, const int aV)
{
    const int index = CornerForTexture(aOrientation, aU, aV);
    return {aScreen[index * 2], aScreen[index * 2 + 1]};
}
} // namespace

int main()
{
    // -----------------------------------------------------------------------
    // The corners are the panel's own rectangle, each tagged with its edge
    // -----------------------------------------------------------------------
    {
        Vec3 corners[4]{};
        CHECK(Corners(Television(), Vec3{}, Rotation{}, corners));

        // Centre (0, 0.115394, 0.42), 1.16 m along local X, 0.66 m along local Z.
        CHECK(Near(corners[CornerIndex(-1, -1)], Vec3{-0.58F, 0.115394F, 0.09F}));
        CHECK(Near(corners[CornerIndex(+1, -1)], Vec3{+0.58F, 0.115394F, 0.09F}));
        CHECK(Near(corners[CornerIndex(+1, +1)], Vec3{+0.58F, 0.115394F, 0.75F}));
        CHECK(Near(corners[CornerIndex(-1, +1)], Vec3{-0.58F, 0.115394F, 0.75F}));

        // The four indices are distinct and cover the array: a tag that aliased
        // another would silently drop a corner from the quad.
        const std::array<int, 4> indices{
            CornerIndex(-1, -1), CornerIndex(+1, -1), CornerIndex(+1, +1), CornerIndex(-1, +1)};
        for (size_t left = 0; left < indices.size(); ++left)
        {
            for (size_t right = left + 1; right < indices.size(); ++right)
            {
                CHECK(indices[left] != indices[right]);
            }
        }
    }

    // -----------------------------------------------------------------------
    // A record may write a skewed or a non-unit axis
    // -----------------------------------------------------------------------
    {
        Quad tilted = Television();
        tilted.up[2] = 0.94F;
        tilted.up[1] = -0.34F; // tilted back twenty degrees, not a unit vector

        Vec3 corners[4]{};
        CHECK(Corners(tilted, Vec3{}, Rotation{}, corners));

        // Squared up rather than refused, and still a rectangle of the right
        // size: the height is measured along the corrected axis.
        const Vec3& bottom = corners[CornerIndex(-1, -1)];
        const Vec3& top = corners[CornerIndex(-1, +1)];
        const Vec3 vStep{top.x - bottom.x, top.y - bottom.y, top.z - bottom.z};
        const float magnitude = std::sqrt(
            (vStep.x * vStep.x) + (vStep.y * vStep.y) + (vStep.z * vStep.z));
        CHECK(Near(magnitude, 0.66F));
        CHECK(vStep.y < 0.0F); // still leaning backwards, not forwards
    }

    // -----------------------------------------------------------------------
    // The prop's own orientation rotates the panel, and the tags go with it
    // -----------------------------------------------------------------------
    {
        // A quarter turn about Z, in the engine's (i, j, k, r) order.
        const float half = 0.70710678F;
        const Rotation yaw{Rotation{0.0F, 0.0F, half, half}};

        Vec3 corners[4]{};
        CHECK(Corners(Television(), Vec3{}, yaw, corners));

        // Local +X becomes world +Y, and the offset rotates with it, so the
        // corner off the panel's +right edge moves from (+0.58, ., 0.09) to
        // (-0.115394, 0.58, 0.09) -- the same physical corner of the panel.
        CHECK(Near(corners[CornerIndex(+1, -1)], Vec3{-0.115394F, 0.58F, 0.09F}));
    }

    // -----------------------------------------------------------------------
    // The orientation comes from the frame, and it is never a half-turn out
    // -----------------------------------------------------------------------
    {
        std::array<float, 8> screen{};

        // In front: the record's `+right` axis is on the left of the frame, so
        // the picture's left-to-right runs along -right and the record's `+up`
        // axis is already at the top.
        ScreenFromFront(screen);
        const Orientation front = Orient(screen.data());
        CHECK(front.flipU);
        CHECK(!front.flipV);

        // From behind: the panel's edges swap sides as seen, and the picture
        // follows the frame rather than turning into gibberish.
        ScreenFromBehind(screen);
        const Orientation behind = Orient(screen.data());
        CHECK(!behind.flipU);
        CHECK(!behind.flipV);

        // Rolled view: the top row has to move to the panel's other edge.
        ScreenUpsideDown(screen);
        const Orientation rolled = Orient(screen.data());
        CHECK(rolled.flipU);
        CHECK(rolled.flipV);
    }

    // -----------------------------------------------------------------------
    // The property that matters: (0,0) is the top-left of the frame, always
    //
    // This is the assertion the two earlier versions of this code would have
    // failed, and the one a person reading a television is actually testing.
    // -----------------------------------------------------------------------
    {
        std::array<std::array<float, 8>, 4> views{};
        ScreenFromFront(views[0]);
        ScreenFromBehind(views[1]);
        ScreenUpsideDown(views[2]);
        // A tilted panel: a trapezoid, not a rectangle, as perspective makes it.
        Screen(views[3], 0.80F, 0.30F, 0.62F, 0.34F);

        for (const auto& screen : views)
        {
            const Orientation orientation = Orient(screen.data());
            const auto topLeft = TexturePoint(screen, orientation, 0, 0);
            const auto topRight = TexturePoint(screen, orientation, 1, 0);
            const auto bottomRight = TexturePoint(screen, orientation, 1, 1);
            const auto bottomLeft = TexturePoint(screen, orientation, 0, 1);

            // The four texture corners are the four panel corners, once each.
            const std::array<int, 4> visited{
                CornerForTexture(orientation, 0, 0), CornerForTexture(orientation, 1, 0),
                CornerForTexture(orientation, 1, 1), CornerForTexture(orientation, 0, 1)};
            for (size_t left = 0; left < visited.size(); ++left)
            {
                for (size_t right = left + 1; right < visited.size(); ++right)
                {
                    CHECK(visited[left] != visited[right]);
                }
            }

            // Left of right, and top above bottom: the picture is readable.
            CHECK(topLeft[0] < topRight[0]);
            CHECK(bottomLeft[0] < bottomRight[0]);
            CHECK(topLeft[1] < bottomLeft[1]);
            CHECK(topRight[1] < bottomRight[1]);
        }
    }

    // -----------------------------------------------------------------------
    // Edge-on: the answer is not re-decided on noise
    // -----------------------------------------------------------------------
    {
        // Both candidate edges project to the same place, well inside the
        // deadband -- the state a panel reaches as the player walks past it.
        std::array<float, 8> edgeOn{};
        Screen(edgeOn, 0.5000F, 0.5005F, 0.5000F, 0.5005F);

        // Whatever the screen had decided, it keeps.
        const Orientation wasFlipped = Orient(edgeOn.data(), Orientation{true, true});
        CHECK(wasFlipped.flipU);
        CHECK(wasFlipped.flipV);
        const Orientation wasStraight = Orient(edgeOn.data(), Orientation{false, false});
        CHECK(!wasStraight.flipU);
        CHECK(!wasStraight.flipV);

        // And past the deadband it decides again, on a real difference.
        std::array<float, 8> clear{};
        Screen(clear, 0.20F, 0.80F, 0.80F, 0.20F);
        const Orientation decided = Orient(clear.data(), Orientation{true, true});
        CHECK(!decided.flipU);
        CHECK(!decided.flipV);
    }

    // -----------------------------------------------------------------------
    // Which side of the panel carries the picture
    // -----------------------------------------------------------------------
    // A rectangle has two sides and a projection does not distinguish them, so
    // without the record's declared front a television seen from behind played
    // its video on the back of the cabinet. The three things that matter for a
    // *gate* -- as opposed to the orientation rule above, which deliberately
    // works from either side -- are that an undeclared record is never gated,
    // that a declared one is gated on exactly one side, and that the prop's own
    // rotation is what decides which side that is in the world.
    {
        const Vec3 behind{0.0F, -2.0F, 0.42F};
        const Vec3 inFront{0.0F, 2.0F, 0.42F};
        // Exactly in the panel's own plane: same Y as the screen centre
        // (0.115394), three metres out to the side. Edge-on, the side is not a
        // question the picture answers.
        const Vec3 atTheSide{3.0F, 0.115394F, 0.42F};

        // An undeclared record. This is every record the catalogue has not
        // measured, and the answer must be "nothing to refuse" from every angle
        // -- a screen that vanishes because a catalogue entry is unfinished is a
        // worse failure than one seen through its own cabinet.
        const Quad undeclared = Television();
        {
            const Facing away = Faces(undeclared, Vec3{}, Rotation{}, behind);
            CHECK(!away.known);
            CHECK(!away.away);
            const Facing front = Faces(undeclared, Vec3{}, Rotation{}, inFront);
            CHECK(!front.known);
            CHECK(!front.away);
        }

        // The catalogue's own television: the screen mesh sits 1 cm inside the
        // body's +Y face, so the picture faces +Y and the cabinet is behind it.
        Quad television = Television();
        television.faces[0] = 0.0F;
        television.faces[1] = 1.0F;
        television.faces[2] = 0.0F;
        {
            const Facing behindPanel = Faces(television, Vec3{}, Rotation{}, behind);
            CHECK(behindPanel.known);
            CHECK(behindPanel.away);
            const Facing inFrontOfPanel = Faces(television, Vec3{}, Rotation{}, inFront);
            CHECK(inFrontOfPanel.known);
            CHECK(!inFrontOfPanel.away);
        }

        // A camera exactly in the panel's own plane is looking at a screen a few
        // pixels wide edge-on, and which side of it the eye is on is not a
        // question the picture answers. Refused, so the answer cannot flap as a
        // player walks through the plane.
        {
            const Facing edgeOn = Faces(television, Vec3{}, Rotation{}, atTheSide);
            CHECK(edgeOn.known);
            CHECK(edgeOn.away);
        }

        // The declaration is in the PROP's frame, so the prop's rotation decides
        // where the front is in the world. A half-turn about Z (`yaw + 180`, the
        // way `facingPlacement` puts a set down facing the player) carries the
        // declared +Y onto -Y: the eye in front of the picture is now the one at
        // negative Y, and the one at positive Y is behind the cabinet.
        Rotation halfTurn{};
        halfTurn.i = 0.0F;
        halfTurn.j = 0.0F;
        halfTurn.k = 1.0F;
        halfTurn.r = 0.0F;
        {
            const Facing afterTheHalfTurn = Faces(television, Vec3{}, halfTurn, behind);
            CHECK(afterTheHalfTurn.known);
            CHECK(!afterTheHalfTurn.away);
            const Facing beforeTheHalfTurn = Faces(television, Vec3{}, halfTurn, inFront);
            CHECK(beforeTheHalfTurn.known);
            CHECK(beforeTheHalfTurn.away);
        }

        // Unnormalised declarations are legal -- a record may write a raw
        // direction -- and a direction that is not a direction at all (zero, or
        // so close to zero that it normalises to noise) is the undeclared case
        // rather than a screen gated on nothing.
        {
            Quad scaled = Television();
            scaled.faces[1] = 5.0F;
            const Facing away = Faces(scaled, Vec3{}, Rotation{}, behind);
            CHECK(away.known);
            CHECK(away.away);

            Quad dust = Television();
            dust.faces[1] = 1.0e-6F;
            const Facing nothing = Faces(dust, Vec3{}, Rotation{}, behind);
            CHECK(!nothing.known);
            CHECK(!nothing.away);
        }
    }

    // -----------------------------------------------------------------------
    // The refusals, which the caller reports as `quad_invalid`
    // -----------------------------------------------------------------------
    {
        Vec3 corners[4]{};

        Quad noWidth = Television();
        noWidth.width = 0.0F;
        CHECK(!Corners(noWidth, Vec3{}, Rotation{}, corners));

        Quad noHeight = Television();
        noHeight.height = 0.0F;
        CHECK(!Corners(noHeight, Vec3{}, Rotation{}, corners));

        Quad noAxis = Television();
        noAxis.right[0] = 0.0F;
        CHECK(!Corners(noAxis, Vec3{}, Rotation{}, corners));

        // Up parallel to right collapses to nothing once squared up.
        Quad parallel = Television();
        parallel.up[0] = 1.0F;
        parallel.up[2] = 0.0F;
        CHECK(!Corners(parallel, Vec3{}, Rotation{}, corners));
    }

    std::cout << "ScreenQuad: corners, frame-derived orientation, facing gate, property and "
                 "refusals OK\n";
    return 0;
}
