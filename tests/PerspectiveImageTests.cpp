// Pins what `WebUiService::DrawSurfaceQuad` hands the GPU for a television, by
// making the same calls into a real ImGui draw list and reading the list back.
//
// `ScreenTessellationTests` proves the arithmetic. This proves the drawing: that
// the vertices ImGui will upload put every texel where the plane puts it, through
// ImGui's own `AddImageQuad`, its own vertex layout and its own merging of draw
// commands -- the part a test of the arithmetic cannot see and a session cannot
// measure, because a bent picture logs exactly like a straight one.
//
// The ground truth shares nothing with the code under test: a pinhole camera, a
// flat rectangle, and a small rasteriser that interpolates each recorded
// triangle's uvs LINEARLY in screen space -- which is what the GPU does with
// ImGui's vertices, since they carry no w. For every sample of every triangle,
// the texel the GPU would fetch there is sent through the pinhole to where the
// plane really puts it, and the distance between the two is the error.
//
// `AddClipped`, for a screen with part of it behind the camera, is held to the
// same: within the tolerance everywhere it draws, nothing drawn of the part behind
// the near plane or outside the view, and every pixel of the part in front that is
// in view covered.

#include "webui/PerspectiveImage.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <utility>
#include <vector>

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
namespace PI = op77::WorldOverlay::PerspectiveImage;
namespace Tess = op77::WorldOverlay::ScreenTessellation;
namespace Clip = op77::WorldOverlay::ScreenClip;

// M_PI is POSIX, not C++; MSVC only has it behind _USE_MATH_DEFINES.
constexpr double kPi = 3.14159265358979323846;
constexpr float kWidth = 1920.0F;
constexpr float kHeight = 1080.0F;
// Any id: what matters is that the draw comes back with the one it was given.
constexpr ImTextureID kPicture = 0x5A17;
constexpr ImU32 kTint = IM_COL32(255, 255, 255, 200);

struct Vec3
{
    double x{}, y{}, z{};
};

Vec3 Plus(const Vec3& aLeft, const Vec3& aRight)
{
    return {aLeft.x + aRight.x, aLeft.y + aRight.y, aLeft.z + aRight.z};
}

Vec3 Times(const Vec3& aVector, const double aScale)
{
    return {aVector.x * aScale, aVector.y * aScale, aVector.z * aScale};
}

struct Pixel
{
    double x{}, y{};
};

/// A pinhole at the origin looking down +z, y up, a 60 degree vertical field, onto
/// a 1920 x 1080 view: the scene the figures in `ScreenTessellation.hpp` use.
Pixel Project(const Vec3& aPoint)
{
    const double focal = (static_cast<double>(kHeight) * 0.5) / std::tan(30.0 * kPi / 180.0);
    return {(static_cast<double>(kWidth) * 0.5) + (focal * aPoint.x / aPoint.z),
            (static_cast<double>(kHeight) * 0.5) - (focal * aPoint.y / aPoint.z)};
}

/// A flat rectangle: its centre and its half extents along its own right and up.
struct Rectangle
{
    Vec3 centre;
    Vec3 right;
    Vec3 up;

    /// The point of the plane that texel (u, v) belongs to: u right, v DOWN, 0..1.
    [[nodiscard]] Vec3 At(const double aU, const double aV) const
    {
        return Plus(centre, Plus(Times(right, (2.0 * aU) - 1.0), Times(up, 1.0 - (2.0 * aV))));
    }
};

/// A `aWidth` x `aHeight` m rectangle `aDistance` m ahead, turned `aYawDegrees` about
/// the vertical (0 faces the camera; past 90 it is seen from behind), its centre
/// moved `aAside` m to the right.
Rectangle Turned(const double aWidth, const double aHeight, const double aDistance,
                 const double aYawDegrees, const double aAside = 0.0)
{
    const double yaw = aYawDegrees * kPi / 180.0;
    return Rectangle{{aAside, 0.0, aDistance},
                     {std::cos(yaw) * aWidth * 0.5, 0.0, std::sin(yaw) * aWidth * 0.5},
                     {0.0, aHeight * 0.5, 0.0}};
}

/// The eight floats `DrawSurfaceQuad` receives: the texture's corners, projected.
void CornersOf(const Rectangle& aRectangle, float (&aOut)[8])
{
    constexpr double kTexture[4][2] = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
    for (int corner = 0; corner < 4; ++corner)
    {
        const Pixel at = Project(aRectangle.At(kTexture[corner][0], kTexture[corner][1]));
        aOut[corner * 2] = static_cast<float>(at.x);
        aOut[(corner * 2) + 1] = static_cast<float>(at.y);
    }
}

/// A draw list as the background list is during a frame: empty, clipped to the view.
struct Frame
{
    ImDrawList list{ImGui::GetDrawListSharedData()};

    Frame()
    {
        list._ResetForNewFrame();
        list.PushClipRect(ImVec2(0.0F, 0.0F), ImVec2(kWidth, kHeight), false);
    }
};

/// The vertices of every triangle that draws the picture, in index order.
template <class Visit>
void ForEachPictureTriangle(const ImDrawList& aList, Visit&& aVisit)
{
    for (const ImDrawCmd& command : aList.CmdBuffer)
    {
        if (command.UserCallback != nullptr || command.TextureId != kPicture)
        {
            continue;
        }
        for (unsigned int element = 0; element + 2 < command.ElemCount; element += 3)
        {
            const auto vertex = [&](const unsigned int aOffset) -> const ImDrawVert& {
                const unsigned int index =
                    static_cast<unsigned int>(aList.IdxBuffer[static_cast<int>(
                        command.IdxOffset + element + aOffset)]) +
                    command.VtxOffset;
                return aList.VtxBuffer[static_cast<int>(index)];
            };
            aVisit(vertex(0), vertex(1), vertex(2));
        }
    }
}

/// How far, in pixels, the worst-placed texel the GPU would draw lands from where
/// the plane puts it: each triangle sampled on a barycentric grid, its uv
/// interpolated linearly in screen space, and the texel fetched there sent through
/// the pinhole.
double WorstTexelError(const ImDrawList& aList, const Rectangle& aRectangle)
{
    constexpr int kSteps = 8;
    double worst = 0.0;
    ForEachPictureTriangle(aList, [&](const ImDrawVert& aA, const ImDrawVert& aB,
                                      const ImDrawVert& aC) {
        for (int i = 0; i <= kSteps; ++i)
        {
            for (int j = 0; i + j <= kSteps; ++j)
            {
                const double b = static_cast<double>(i) / kSteps;
                const double c = static_cast<double>(j) / kSteps;
                const double a = 1.0 - b - c;
                const double x = (a * aA.pos.x) + (b * aB.pos.x) + (c * aC.pos.x);
                const double y = (a * aA.pos.y) + (b * aB.pos.y) + (c * aC.pos.y);
                const double u = (a * aA.uv.x) + (b * aB.uv.x) + (c * aC.uv.x);
                const double v = (a * aA.uv.y) + (b * aB.uv.y) + (c * aC.uv.y);
                const Pixel truth = Project(aRectangle.At(u, v));
                worst = std::max(worst, std::hypot(truth.x - x, truth.y - y));
            }
        }
    });
    return worst;
}

int PictureVertices(const ImDrawList& aList)
{
    int count = 0;
    ForEachPictureTriangle(aList, [&count](const ImDrawVert&, const ImDrawVert&,
                                          const ImDrawVert&) { count += 3; });
    return count;
}

/// What the overlay drew before: one image quad over the four corners.
double OneQuadError(const Rectangle& aRectangle)
{
    float corners[8]{};
    CornersOf(aRectangle, corners);
    Frame frame;
    frame.list.AddImageQuad(kPicture, ImVec2(corners[0], corners[1]),
                            ImVec2(corners[2], corners[3]), ImVec2(corners[4], corners[5]),
                            ImVec2(corners[6], corners[7]), ImVec2(0.0F, 0.0F),
                            ImVec2(1.0F, 0.0F), ImVec2(1.0F, 1.0F), ImVec2(0.0F, 1.0F),
                            kTint);
    return WorstTexelError(frame.list, aRectangle);
}

double Focal()
{
    return (static_cast<double>(kHeight) * 0.5) / std::tan(30.0 * kPi / 180.0);
}

/// The map `AddClipped` is handed for a rectangle, exact: texture space to pixels
/// with the view depth (z) as its third row. Each row is affine in (u, v) because
/// the pinhole divides by z.
Clip::Map MapOf(const Rectangle& aRectangle)
{
    // At(u, v) = (centre - right + up) + u (2 right) + v (-2 up).
    const Vec3 origin = Plus(aRectangle.centre, Plus(Times(aRectangle.right, -1.0), aRectangle.up));
    const Vec3 columns[3] = {Times(aRectangle.right, 2.0), Times(aRectangle.up, -2.0), origin};
    Clip::Map map;
    for (int column = 0; column < 3; ++column)
    {
        const Vec3& c = columns[column];
        map.m[0][column] = (static_cast<double>(kWidth) * 0.5 * c.z) + (Focal() * c.x);
        map.m[1][column] = (static_cast<double>(kHeight) * 0.5 * c.z) - (Focal() * c.y);
        map.m[2][column] = c.z;
    }
    return map;
}

/// The view depth of texel (u, v).
double DepthAt(const Rectangle& aRectangle, const double aU, const double aV)
{
    return aRectangle.At(aU, aV).z;
}

/// Which pixels the picture's triangles cover, every one of them rasterised at its
/// pixel centres.
std::vector<char> Coverage(const ImDrawList& aList)
{
    const int width = static_cast<int>(kWidth);
    const int height = static_cast<int>(kHeight);
    std::vector<char> covered(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    ForEachPictureTriangle(aList, [&](const ImDrawVert& aA, const ImDrawVert& aB, const ImDrawVert& aC) {
        const double area = ((static_cast<double>(aB.pos.x) - aA.pos.x) * (static_cast<double>(aC.pos.y) - aA.pos.y)) -
                            ((static_cast<double>(aC.pos.x) - aA.pos.x) * (static_cast<double>(aB.pos.y) - aA.pos.y));
        if (area == 0.0)
        {
            return;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({aA.pos.x, aB.pos.x, aC.pos.x}))));
        const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({aA.pos.x, aB.pos.x, aC.pos.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({aA.pos.y, aB.pos.y, aC.pos.y}))));
        const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({aA.pos.y, aB.pos.y, aC.pos.y}))));
        const auto edge = [](const ImDrawVert& aP, const ImDrawVert& aQ, const double aX, const double aY) {
            return ((static_cast<double>(aQ.pos.x) - aP.pos.x) * (aY - aP.pos.y)) -
                   ((aX - aP.pos.x) * (static_cast<double>(aQ.pos.y) - aP.pos.y));
        };
        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                const double px = x + 0.5;
                const double py = y + 0.5;
                const double a = edge(aB, aC, px, py) / area;
                const double b = edge(aC, aA, px, py) / area;
                const double c = edge(aA, aB, px, py) / area;
                if (a >= -1.0e-9 && b >= -1.0e-9 && c >= -1.0e-9)
                {
                    covered[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x)] = 1;
                }
            }
        }
    });
    return covered;
}

/// Whether the ray through a pixel meets the rectangle past the near plane.
bool SeenAt(const Rectangle& aRectangle, const double aX, const double aY)
{
    // The ray (dx, dy, 1) has depth t at parameter t; solve centre + right s + up r
    // = ray t for s, r in -1..1.
    const double dx = (aX - (static_cast<double>(kWidth) * 0.5)) / Focal();
    const double dy = -(aY - (static_cast<double>(kHeight) * 0.5)) / Focal();
    const Vec3& c = aRectangle.centre;
    const Vec3& r = aRectangle.right;
    const Vec3& u = aRectangle.up;
    // [r u -d] [s q t]^T = -c, d = (dx, dy, 1).
    const double m[3][3] = {{r.x, u.x, -dx}, {r.y, u.y, -dy}, {r.z, u.z, -1.0}};
    const double rhs[3] = {-c.x, -c.y, -c.z};
    const double det = (m[0][0] * ((m[1][1] * m[2][2]) - (m[1][2] * m[2][1]))) -
                       (m[0][1] * ((m[1][0] * m[2][2]) - (m[1][2] * m[2][0]))) +
                       (m[0][2] * ((m[1][0] * m[2][1]) - (m[1][1] * m[2][0])));
    if (std::abs(det) < 1.0e-12)
    {
        return false;
    }
    const auto solve = [&](const int aColumn) {
        double n[3][3];
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                n[i][j] = j == aColumn ? rhs[i] : m[i][j];
            }
        }
        return ((n[0][0] * ((n[1][1] * n[2][2]) - (n[1][2] * n[2][1]))) -
                (n[0][1] * ((n[1][0] * n[2][2]) - (n[1][2] * n[2][0]))) +
                (n[0][2] * ((n[1][0] * n[2][1]) - (n[1][1] * n[2][0])))) /
               det;
    };
    const double s = solve(0);
    const double q = solve(1);
    const double t = solve(2);
    return t > Clip::kNearDepth && s >= -1.0 && s <= 1.0 && q >= -1.0 && q <= 1.0;
}

/// Everything `AddClipped` must hold to for one rectangle: what it reports, where
/// its vertices are, how far any texel is from where the plane puts it, and that
/// the part in front of the near plane and in view is covered.
double CheckClipped(const Rectangle& aRectangle, const char* const aName)
{
    const Clip::Map map = MapOf(aRectangle);
    Frame frame;
    const int pieces = PI::AddClipped(frame.list, kPicture, map, kTint);
    CHECK(pieces >= 1 && pieces <= PI::kMaximumClippedPieces);
    CHECK(frame.list.VtxBuffer.Size > 0);
    const double margin = PI::kViewMarginPixels + 0.01;
    for (const ImDrawVert& vertex : frame.list.VtxBuffer)
    {
        CHECK(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y));
        CHECK(vertex.pos.x >= -margin && vertex.pos.x <= kWidth + margin);
        CHECK(vertex.pos.y >= -margin && vertex.pos.y <= kHeight + margin);
        CHECK(vertex.uv.x >= 0.0F && vertex.uv.x <= 1.0F && vertex.uv.y >= 0.0F && vertex.uv.y <= 1.0F);
        CHECK(DepthAt(aRectangle, vertex.uv.x, vertex.uv.y) >= Clip::kNearDepth - 1.0e-4);
        CHECK(vertex.col == kTint);
    }
    // One draw command, however many pieces.
    int commands = 0;
    for (const ImDrawCmd& command : frame.list.CmdBuffer)
    {
        if (command.ElemCount > 0)
        {
            ++commands;
            CHECK(command.TextureId == kPicture);
        }
    }
    CHECK(commands == 1);
    // Watertight: a texel drawn by two cells is at the same pixel in both.
    std::map<std::pair<float, float>, std::pair<float, float>> placed;
    for (const ImDrawVert& vertex : frame.list.VtxBuffer)
    {
        const auto [found, inserted] =
            placed.emplace(std::make_pair(vertex.uv.x, vertex.uv.y), std::make_pair(vertex.pos.x, vertex.pos.y));
        CHECK(inserted || found->second == std::make_pair(vertex.pos.x, vertex.pos.y));
    }
    const double error = WorstTexelError(frame.list, aRectangle);
    // Covered: every pixel whose ray meets the picture past the near plane, but for
    // the pixels along the picture's own edge, where the rasteriser decides.
    const std::vector<char> covered = Coverage(frame.list);
    int seen = 0;
    for (int y = 1; y + 1 < static_cast<int>(kHeight); y += 2)
    {
        for (int x = 1; x + 1 < static_cast<int>(kWidth); x += 2)
        {
            const double px = x + 0.5;
            const double py = y + 0.5;
            const bool here = SeenAt(aRectangle, px, py);
            if (!here)
            {
                continue;
            }
            if (!SeenAt(aRectangle, px - 1.5, py) || !SeenAt(aRectangle, px + 1.5, py) ||
                !SeenAt(aRectangle, px, py - 1.5) || !SeenAt(aRectangle, px, py + 1.5))
            {
                continue;
            }
            ++seen;
            CHECK(covered[(static_cast<std::size_t>(y) * static_cast<std::size_t>(kWidth)) + static_cast<std::size_t>(x)] == 1);
        }
    }
    CHECK(seen > 1000);
    std::cout << aName << ": " << pieces << " pieces, " << frame.list.VtxBuffer.Size
              << " vertices, worst texel " << error << " px\n";
    return error;
}

/// The bound `ScreenTessellation` reports for these corners.
Tess::Choice ChoiceFor(const float (&aCorners)[8])
{
    Tess::Homography map;
    CHECK(Tess::Solve(aCorners, map) == Tess::Status::Ok);
    return Tess::ChooseCells(map);
}
} // namespace

int main()
{
    ImGui::CreateContext();

    // Head-on, a television is the one quad it always was: the same four vertices
    // in the same order with the same uvs, so nothing changes for a set seen square.
    {
        const Rectangle tv = Turned(1.16, 0.66, 1.5, 0.0);
        float corners[8]{};
        CornersOf(tv, corners);
        Frame frame;
        CHECK(PI::Add(frame.list, kPicture, corners, kTint) == 1);
        CHECK(frame.list.VtxBuffer.Size == 4);
        CHECK(frame.list.IdxBuffer.Size == 6);
        CHECK(PictureVertices(frame.list) == 6);
        const ImVec2 uvs[4] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}};
        for (int corner = 0; corner < 4; ++corner)
        {
            const ImDrawVert& vertex = frame.list.VtxBuffer[corner];
            CHECK(vertex.pos.x == corners[corner * 2]);
            CHECK(vertex.pos.y == corners[(corner * 2) + 1]);
            CHECK(vertex.uv.x == uvs[corner].x && vertex.uv.y == uvs[corner].y);
            CHECK(vertex.col == kTint);
        }
        CHECK(WorstTexelError(frame.list, tv) < 0.01);
    }

    // Turned 50 degrees at 1.5 m: one quad bends the picture by about a hundred
    // pixels; the grid puts every texel within the tolerance of where the plane does.
    {
        const Rectangle tv = Turned(1.16, 0.66, 1.5, 50.0);
        float corners[8]{};
        CornersOf(tv, corners);
        const double before = OneQuadError(tv);
        CHECK(before > 60.0);

        Frame frame;
        const int cells = PI::Add(frame.list, kPicture, corners, kTint);
        const Tess::Choice choice = ChoiceFor(corners);
        CHECK(cells == choice.cells);
        CHECK(cells > 1 && !choice.capped);
        // Wholly on screen, so every cell is queued: four vertices and six indices
        // a cell, every one carrying the picture and the tint.
        CHECK(frame.list.VtxBuffer.Size == 4 * cells * cells);
        CHECK(PictureVertices(frame.list) == 6 * cells * cells);
        for (const ImDrawVert& vertex : frame.list.VtxBuffer)
        {
            CHECK(vertex.col == kTint);
            CHECK(vertex.uv.x >= 0.0F && vertex.uv.x <= 1.0F);
            CHECK(vertex.uv.y >= 0.0F && vertex.uv.y <= 1.0F);
        }
        const double after = WorstTexelError(frame.list, tv);
        std::cout << "television at 50 degrees: one quad " << before << " px, " << cells
                  << " x " << cells << " cells " << after << " px\n";
        CHECK(after <= static_cast<double>(Tess::kDefaultTolerancePixels) + 0.01);
        CHECK(after <= choice.errorBoundPixels + 0.01);

        // One draw: the cells merge into a single command, so the grid costs one
        // draw call however many cells it has.
        int commands = 0;
        for (const ImDrawCmd& command : frame.list.CmdBuffer)
        {
            if (command.ElemCount > 0)
            {
                ++commands;
                CHECK(command.TextureId == kPicture);
            }
        }
        CHECK(commands == 1);

        // Watertight: every vertex standing for the same texel stands at the same
        // pixel, bit for bit, so two cells can never open a seam between them.
        std::map<std::pair<float, float>, std::pair<float, float>> placed;
        for (const ImDrawVert& vertex : frame.list.VtxBuffer)
        {
            const auto key = std::make_pair(vertex.uv.x, vertex.uv.y);
            const auto at = std::make_pair(vertex.pos.x, vertex.pos.y);
            const auto [found, inserted] = placed.emplace(key, at);
            CHECK(inserted || found->second == at);
        }
        // ... and the grid covers the whole picture, corner to corner.
        CHECK(placed.count({0.0F, 0.0F}) == 1 && placed.count({1.0F, 1.0F}) == 1);
        CHECK(placed.count({1.0F, 0.0F}) == 1 && placed.count({0.0F, 1.0F}) == 1);
        CHECK(static_cast<int>(placed.size()) == (cells + 1) * (cells + 1));
    }

    // Seen from behind -- a set whose record declares no front is drawn from both
    // sides -- the quad arrives mirrored, and is drawn in perspective all the same.
    {
        const Rectangle tv = Turned(1.16, 0.66, 1.5, 210.0);
        float corners[8]{};
        CornersOf(tv, corners);
        Frame frame;
        const int cells = PI::Add(frame.list, kPicture, corners, kTint);
        CHECK(cells > 1);
        CHECK(OneQuadError(tv) > 20.0);
        CHECK(WorstTexelError(frame.list, tv) <= ChoiceFor(corners).errorBoundPixels + 0.01);
    }

    // The 150 ft cinema seen from the side and partly out of view: the cells that
    // cannot reach the view are not queued, and the ones that are drawn are within
    // the bound the grid reports for itself (the cap holds the count; what is left
    // over is reported, not hidden).
    {
        const Rectangle cinema = Turned(45.72, 26.0131, 30.6, 35.0, 14.0);
        float corners[8]{};
        CornersOf(cinema, corners);
        const Tess::Choice choice = ChoiceFor(corners);
        Frame frame;
        const int cells = PI::Add(frame.list, kPicture, corners, kTint);
        CHECK(cells == choice.cells);
        const int queued = frame.list.VtxBuffer.Size / 4;
        CHECK(queued > 0 && queued < cells * cells);
        const double before = OneQuadError(cinema);
        const double after = WorstTexelError(frame.list, cinema);
        std::cout << "cinema at 35 degrees: one quad " << before << " px, " << cells << " x "
                  << cells << " cells (" << queued << " in view) " << after << " px, bound "
                  << choice.errorBoundPixels << " px\n";
        CHECK(before > 100.0);
        CHECK(after <= choice.errorBoundPixels + 0.01);
        CHECK(after < before / 20.0);
    }

    // A quad the map refuses -- here crossed over itself, the shape a corner behind
    // the eye takes after the divide -- is drawn exactly as before: one image quad
    // over the corners as given.
    {
        const float crossed[8] = {100.0F, 100.0F, 900.0F, 800.0F, 900.0F, 100.0F, 100.0F, 800.0F};
        Frame frame;
        CHECK(PI::Add(frame.list, kPicture, crossed, kTint) == 1);
        CHECK(frame.list.VtxBuffer.Size == 4);
        for (int corner = 0; corner < 4; ++corner)
        {
            CHECK(frame.list.VtxBuffer[corner].pos.x == crossed[corner * 2]);
            CHECK(frame.list.VtxBuffer[corner].pos.y == crossed[(corner * 2) + 1]);
        }
    }

    // The 150 ft cinema walked along: a wall 3 m to the left, from 5 m behind the
    // player to 40 m ahead. Its near end is behind the camera, so it has no four
    // corners to draw from; the part in front and in view is drawn from its map,
    // every texel where the plane puts it.
    {
        const Rectangle cinema{{-3.0, 4.0, 17.86}, {0.0, 0.0, 22.86}, {0.0, 12.86, 0.0}};
        CHECK(DepthAt(cinema, 0.0, 0.0) < 0.0 && DepthAt(cinema, 1.0, 0.0) > 0.0);
        const double error = CheckClipped(cinema, "cinema walked along, near end behind");
        CHECK(error <= static_cast<double>(Tess::kDefaultTolerancePixels) + 0.05);
    }

    // A television right beside the camera, turned so that one corner is behind it.
    {
        const Rectangle beside = Turned(1.16, 0.66, 0.35, 70.0, 0.2);
        CHECK(DepthAt(beside, 0.0, 0.0) < Clip::kNearDepth || DepthAt(beside, 1.0, 0.0) < Clip::kNearDepth);
        const double error = CheckClipped(beside, "television turned beside the camera");
        CHECK(error <= static_cast<double>(Tess::kDefaultTolerancePixels) + 0.05);
    }

    // Wholly in front: the same quality as `Add`.
    {
        const Rectangle tv = Turned(1.16, 0.66, 1.5, 50.0);
        const double error = CheckClipped(tv, "television at 50 degrees, through AddClipped");
        CHECK(error <= static_cast<double>(Tess::kDefaultTolerancePixels) + 0.01);
    }

    // Wholly behind the camera, or wholly out of view: nothing is queued.
    {
        const Rectangle behind{{0.0, 0.0, -3.0}, {0.6, 0.0, 0.0}, {0.0, 0.3, 0.0}};
        Frame frame;
        CHECK(PI::AddClipped(frame.list, kPicture, MapOf(behind), kTint) == 0);
        CHECK(frame.list.VtxBuffer.Size == 0);
        const Rectangle aside{{40.0, 0.0, 3.0}, {0.6, 0.0, 0.0}, {0.0, 0.3, 0.0}};
        CHECK(PI::AddClipped(frame.list, kPicture, MapOf(aside), kTint) == 0);
        CHECK(frame.list.VtxBuffer.Size == 0);
        Clip::Map broken = MapOf(aside);
        broken.m[0][1] = std::nan("");
        CHECK(PI::AddClipped(frame.list, kPicture, broken, kTint) == 0);
        CHECK(frame.list.VtxBuffer.Size == 0);
    }

    // Not a number is refused the same way, rather than spread over a grid.
    {
        const float broken[8] = {std::nanf(""), 100.0F, 900.0F, 100.0F,
                                 900.0F,        800.0F, 100.0F, 800.0F};
        Frame frame;
        CHECK(PI::Add(frame.list, kPicture, broken, kTint) == 1);
        CHECK(frame.list.VtxBuffer.Size == 4);
    }

    ImGui::DestroyContext();
    std::cout << "PerspectiveImageTests: all checks passed\n";
    return 0;
}
