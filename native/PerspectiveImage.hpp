#pragma once

// A WebUI surface drawn onto a world quad IN PERSPECTIVE.
//
// ---------------------------------------------------------------------------
// WHY THIS IS NOT ONE `AddImageQuad`
// ---------------------------------------------------------------------------
//
// `WebUiService::DrawSurfaceQuad` used to hand ImGui a television's four
// projected corners as one image quad. ImGui splits that into two triangles and
// interpolates the texture LINEARLY in screen space, which a perspective
// projection does not: the picture was right at the four corners and bent
// everywhere between them, along the diagonal where the two triangles meet, and
// the bend changed as the player walked round the set -- which is how it was
// reported: the screen moves when it is looked at from a different angle. Head-on
// it is exactly right, which is how a television is usually checked. On the
// 150 ft cinema seen from the side it is hundreds of pixels.
//
// `ScreenTessellation` turns the four corners into the plane's own map (a
// homography) and picks the smallest grid of cells inside which linear
// interpolation errs by less than a pixel; this draws that grid, every vertex
// exact. A head-on screen is one cell -- the single quad drawn before, with the
// same four vertices -- and a quad the map refuses (a corner behind the eye) is
// drawn exactly as before too.
//
// Consecutive `AddImageQuad` calls with one texture merge into one draw command,
// so the grid is still a single draw: 9216 vertices for the most oblique set the
// cap allows, which a 16-bit index addresses even before the DX12 backend's
// vertex offsets.
//
// Its own header, with ImGui in it, so a test can drive exactly the calls
// `DrawSurfaceQuad` makes into a real `ImDrawList` and read back what the GPU is
// handed (`tests/PerspectiveImageTests.cpp`); `ScreenTessellation.hpp` stays free
// of ImGui and of the engine.
//
// A screen with part of it behind the camera has no four corners to start from
// (ScreenClip.hpp): `AddClipped` draws it from its map instead, cut to the part in
// front of the camera and in view.

#include "ScreenClip.hpp"
#include "ScreenTessellation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include <imgui.h>

namespace op77::WorldOverlay::PerspectiveImage
{
/// Queues `aTexture` over `aCorners` -- eight floats in pixels, top-left,
/// top-right, bottom-right, bottom-left: the texture's (0,0), (1,0), (1,1) and
/// (0,1) -- with `aTint` on every vertex.
///
/// Returns the cells per side it drew: 1 for a head-on quad or one the map
/// refuses (one `AddImageQuad`, exactly as before), more for an oblique one.
/// Cells wholly outside the draw list's clip rectangle are not queued, which is
/// most of the grid when a large set fills the view.
inline int Add(ImDrawList& aDraw, const ImTextureID aTexture, const float (&aCorners)[8],
               const ImU32 aTint)
{
    namespace Tess = ScreenTessellation;
    Tess::Homography map;
    if (Tess::Solve(aCorners, map) == Tess::Status::Ok)
    {
        const Tess::Choice choice = Tess::ChooseCells(map);
        if (choice.cells > 1)
        {
            const ImVec2 clipMin = aDraw.GetClipRectMin();
            const ImVec2 clipMax = aDraw.GetClipRectMax();
            const Tess::Rect visible{clipMin.x, clipMin.y, clipMax.x, clipMax.y};
            Tess::ForEachCell(map, choice.cells, choice.cells, &visible,
                              [&aDraw, aTexture, aTint](const Tess::Point (&aAt)[4],
                                                        const Tess::Point (&aUv)[4]) {
                                  aDraw.AddImageQuad(aTexture,
                                                     ImVec2(aAt[0].x, aAt[0].y),
                                                     ImVec2(aAt[1].x, aAt[1].y),
                                                     ImVec2(aAt[2].x, aAt[2].y),
                                                     ImVec2(aAt[3].x, aAt[3].y),
                                                     ImVec2(aUv[0].x, aUv[0].y),
                                                     ImVec2(aUv[1].x, aUv[1].y),
                                                     ImVec2(aUv[2].x, aUv[2].y),
                                                     ImVec2(aUv[3].x, aUv[3].y), aTint);
                              });
            return choice.cells;
        }
    }
    aDraw.AddImageQuad(aTexture, ImVec2(aCorners[0], aCorners[1]),
                       ImVec2(aCorners[2], aCorners[3]), ImVec2(aCorners[4], aCorners[5]),
                       ImVec2(aCorners[6], aCorners[7]), ImVec2(0.0F, 0.0F),
                       ImVec2(1.0F, 0.0F), ImVec2(1.0F, 1.0F), ImVec2(0.0F, 1.0F), aTint);
    return 1;
}

/// How far past the draw list's clip rectangle, in pixels, a cut screen is still
/// drawn: the scissor trims it there anyway, and the margin keeps the cut a whole
/// pixel clear of anything that is seen.
inline constexpr double kViewMarginPixels = 2.0;

/// Bounds on the work `AddClipped` may do for one screen: how many times a cell
/// may be halved, and how many pieces may be queued. Past either, what is left is
/// drawn as it stands, a little less exact rather than not at all.
inline constexpr int kMaximumClippedDepth = 10;
inline constexpr int kMaximumClippedPieces = 4096;

/// Queues `aTexture` over a screen that is partly behind the camera, `aMap` its
/// map from texture space to pixels with the view depth as its third row
/// (ScreenClip.hpp), with `aTint` on every vertex.
///
/// Only the part in front of the camera's near plane AND in view is drawn: the
/// picture cut by five straight lines in texture space, the near plane and the four
/// sides of the clip rectangle. A uniform grid cannot cover that part: the closer
/// the screen comes to the eye the more it curves on screen -- as the cube of the
/// nearness -- so the cells a few metres away must be hundreds of times smaller
/// than those at the far end of the cinema. So the cut part's texture-space bounds
/// are halved, quadrant by quadrant, until every cell is within `aTolerancePixels`
/// by the curvature bound of `ScreenTessellation`, taken over that cell's own drawn
/// part. A cell wholly inside is one image quad, as `Add` draws its cells; a cell a
/// line runs through is cut to it and drawn as a fan. Every vertex is exact, and a
/// corner two cells share is the same to the bit in both; where a cell meets two
/// smaller ones, the smaller ones' extra corner lies on its edge, to float rounding.
///
/// Returns the pieces queued: 0 when none of the picture is in front of the camera
/// and in view.
inline int AddClipped(ImDrawList& aDraw, const ImTextureID aTexture, const ScreenClip::Map& aMap,
                      const ImU32 aTint,
                      const float aTolerancePixels = ScreenTessellation::kDefaultTolerancePixels)
{
    namespace Clip = ScreenClip;
    if (!aMap.Finite())
    {
        return 0;
    }
    const ImVec2 clipMin = aDraw.GetClipRectMin();
    const ImVec2 clipMax = aDraw.GetClipRectMax();
    const double minX = static_cast<double>(clipMin.x) - kViewMarginPixels;
    const double minY = static_cast<double>(clipMin.y) - kViewMarginPixels;
    const double maxX = static_cast<double>(clipMax.x) + kViewMarginPixels;
    const double maxY = static_cast<double>(clipMax.y) + kViewMarginPixels;
    const Clip::Polygon region = Clip::InView(Clip::VisibleRegion(aMap), aMap, minX, minY, maxX, maxY);
    if (region.count < 3)
    {
        return 0;
    }

    struct Cell
    {
        double u0{};
        double v0{};
        double u1{};
        double v1{};
    };
    Cell root{1.0, 1.0, 0.0, 0.0};
    for (int i = 0; i < region.count; ++i)
    {
        const auto& corner = region.at[static_cast<std::size_t>(i)];
        root.u0 = std::min(root.u0, corner[0]);
        root.u1 = std::max(root.u1, corner[0]);
        root.v0 = std::min(root.v0, corner[1]);
        root.v1 = std::max(root.v1, corner[1]);
    }
    root.u0 = std::clamp(root.u0, 0.0, 1.0);
    root.v0 = std::clamp(root.v0, 0.0, 1.0);
    root.u1 = std::clamp(root.u1, root.u0, 1.0);
    root.v1 = std::clamp(root.v1, root.v0, 1.0);

    // The five lines, as a u + b v + c >= 0 on the side that is drawn.
    const auto& m = aMap.m;
    const double lines[5][3] = {
        {m[2][0], m[2][1], m[2][2] - Clip::kNearDepth},
        {m[0][0] - (minX * m[2][0]), m[0][1] - (minX * m[2][1]), m[0][2] - (minX * m[2][2])},
        {(maxX * m[2][0]) - m[0][0], (maxX * m[2][1]) - m[0][1], (maxX * m[2][2]) - m[0][2]},
        {m[1][0] - (minY * m[2][0]), m[1][1] - (minY * m[2][1]), m[1][2] - (minY * m[2][2])},
        {(maxY * m[2][0]) - m[1][0], (maxY * m[2][1]) - m[1][1], (maxY * m[2][2]) - m[1][2]},
    };
    constexpr unsigned kAllInside = (1U << 5) - 1U;
    const auto sides = [&lines](const double aU, const double aV) {
        unsigned inside = 0;
        for (unsigned line = 0; line < 5; ++line)
        {
            if ((lines[line][0] * aU) + (lines[line][1] * aV) + lines[line][2] >= 0.0)
            {
                inside |= 1U << line;
            }
        }
        return inside;
    };
    const auto at = [&aMap](const double aU, const double aV) {
        const std::array<double, 2> pixel = aMap.Apply(aU, aV);
        return ImVec2(static_cast<float>(pixel[0]), static_cast<float>(pixel[1]));
    };
    const auto uv = [](const double aU, const double aV) {
        return ImVec2(static_cast<float>(aU), static_cast<float>(aV));
    };
    const auto draw = [&](const Clip::Polygon& aPiece, const bool aWhole) {
        if (aWhole)
        {
            const auto& c = aPiece.at;
            aDraw.AddImageQuad(aTexture, at(c[0][0], c[0][1]), at(c[1][0], c[1][1]), at(c[2][0], c[2][1]),
                               at(c[3][0], c[3][1]), uv(c[0][0], c[0][1]), uv(c[1][0], c[1][1]),
                               uv(c[2][0], c[2][1]), uv(c[3][0], c[3][1]), aTint);
            return;
        }
        // A fan from the first corner, two triangles to an image quad; an odd one out
        // is a quad with its last corner repeated, whose second triangle has no area.
        ImVec2 pixels[Clip::kMaximumPolygon];
        ImVec2 uvs[Clip::kMaximumPolygon];
        for (int i = 0; i < aPiece.count; ++i)
        {
            const auto& corner = aPiece.at[static_cast<std::size_t>(i)];
            pixels[i] = at(corner[0], corner[1]);
            uvs[i] = uv(corner[0], corner[1]);
        }
        for (int i = 1; i + 1 < aPiece.count; i += 2)
        {
            const int third = i + 2 < aPiece.count ? i + 2 : i + 1;
            aDraw.AddImageQuad(aTexture, pixels[0], pixels[i], pixels[i + 1], pixels[third], uvs[0], uvs[i],
                               uvs[i + 1], uvs[third], aTint);
        }
    };

    // Level by level, so that a budget that runs out leaves every part of the
    // picture equally coarse rather than one corner exact and the rest not drawn.
    const double tolerance = std::max(0.05, static_cast<double>(aTolerancePixels));
    struct Pending
    {
        Cell cell;
        Clip::Polygon piece;
        bool whole{};
    };
    std::vector<Cell> level{root};
    std::vector<Pending> split;
    int queued = 0;
    for (int depth = 0; !level.empty(); ++depth)
    {
        split.clear();
        for (const Cell& cell : level)
        {
            const unsigned corners[4] = {sides(cell.u0, cell.v0), sides(cell.u1, cell.v0), sides(cell.u1, cell.v1),
                                         sides(cell.u0, cell.v1)};
            const unsigned all = corners[0] & corners[1] & corners[2] & corners[3];
            const unsigned any = corners[0] | corners[1] | corners[2] | corners[3];
            if (any != kAllInside)
            {
                continue; // wholly on the far side of one of the lines
            }
            Clip::Polygon piece = Clip::Rectangle(cell.u0, cell.v0, cell.u1, cell.v1);
            for (unsigned line = 0; line < 5 && piece.count >= 3; ++line)
            {
                if ((all & (1U << line)) == 0)
                {
                    piece = Clip::Cut(piece, lines[line][0], lines[line][1], lines[line][2]);
                }
            }
            if (piece.count < 3)
            {
                continue;
            }
            // `ScreenTessellation::ErrorBound` for this cell's legs, with the
            // curvature bounded over the part of it that is drawn.
            const Clip::Curvature k = Clip::CurvatureOver(aMap, piece);
            const double p = cell.u1 - cell.u0;
            const double q = cell.v1 - cell.v0;
            const double bound = ((k.uu * p * p) + (2.0 * k.uv * p * q) + (k.vv * q * q)) / 8.0;
            const bool whole = all == kAllInside;
            if (!(bound <= tolerance) && depth < kMaximumClippedDepth)
            {
                split.push_back(Pending{cell, piece, whole});
                continue;
            }
            draw(piece, whole);
            ++queued;
        }
        if (split.empty())
        {
            break;
        }
        // Past the budget, what wanted halving is drawn as it is.
        if (queued + (4 * static_cast<int>(split.size())) > kMaximumClippedPieces)
        {
            for (const Pending& pending : split)
            {
                draw(pending.piece, pending.whole);
                ++queued;
            }
            break;
        }
        level.clear();
        for (const Pending& pending : split)
        {
            const Cell& c = pending.cell;
            const double u = (c.u0 + c.u1) * 0.5;
            const double v = (c.v0 + c.v1) * 0.5;
            level.push_back(Cell{c.u0, c.v0, u, v});
            level.push_back(Cell{u, c.v0, c.u1, v});
            level.push_back(Cell{c.u0, v, u, c.v1});
            level.push_back(Cell{u, v, c.u1, c.v1});
        }
    }
    return queued;
}
} // namespace op77::WorldOverlay::PerspectiveImage
