#pragma once

// Drawing a world screen so the picture lies IN the plane it is a picture of.
//
// ---------------------------------------------------------------------------
// THE PROBLEM THIS SOLVES
// ---------------------------------------------------------------------------
//
// A television is a rectangle in the world and the overlay draws it as ONE image
// quad: four projected corners, two triangles, and texture coordinates that ImGui
// interpolates LINEARLY IN SCREEN SPACE, because an ImGui vertex carries a
// position, a uv and a colour and no `w`. A plane seen in perspective needs the
// interpolation done in homogeneous space. The two agree at the four corners and
// nowhere else, so every texel between them lands somewhere other than where the
// plane puts it. That is the affine texture warp, and it is large:
// `tools/quad-affine-error.py` in the open77-tv repository measures it for the
// television catalogue, at 1920x1080 and a 60 degree vertical field of view --
//
//     1.16 m television at 1.5 m, turned 30 degrees      up to  76 px, mean  45 px
//     1.16 m television at 1.5 m, turned 50 degrees      up to 103 px, mean  57 px
//     cinema.100ft at 20.4 m, turned 35 degrees          up to 384 px, mean 224 px
//
// Head-on it is exactly zero, which is why nobody sees it standing square to a set
// and everybody sees it walking past one. What it looks like is a picture with no
// depth: a flat sticker whose grid lines do not converge, kinked along one
// diagonal, sliding over the cabinet it is meant to sit in.
//
// It has a second symptom that is easier to misread. The pointer is placed by
// `Api::MediaScreens` by interpolating the WORLD corners and projecting the result
// (perspective-exact), while the picture under it is drawn affinely. The cursor
// therefore sits where the plane says page pixel (u, v) is, and the page element
// that will actually receive the click is drawn somewhere else -- by the error
// above, tens of pixels on an oblique set. You aim at one button and click another.
// Drawing the picture correctly is what makes the two agree again.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES
// ---------------------------------------------------------------------------
//
// It needs nothing the producer does not already publish. Four projected corners
// of a planar rectangle determine the whole plane-to-screen map: it is a
// homography, and the unit-square-to-quad solution is closed form (Heckbert,
// "Fundamentals of Texture Mapping and Image Warping", 1989). So the renderer can
// subdivide the quad in TEXTURE space, place every grid vertex with the
// homography, and hand ImGui small cells whose vertices are exact. Inside a cell
// the linear interpolation is wrong by a bounded amount, and that amount falls
// with the square of the cell count.
//
// The cell count is not a constant. `ChooseCells` derives it from a closed-form,
// rigorous bound on the homography's second derivatives, so a set seen head-on
// costs one quad -- exactly what is drawn today, bit for bit -- and a set seen
// edge-on costs as many as the tolerance demands, up to a cap. The bound is an
// UPPER bound (it cannot under-subdivide) and it is not vacuous: against brute
// force it asks for 1.4x the cells strictly needed at the median, 2x at the 90th
// percentile and 2.5x at worst over random scenes, and never fewer. Both claims are
// pinned in `tests/ScreenTessellationTests.cpp`. (Cost goes with the square of the
// cell count, so the median set is drawn with about twice the cells it strictly
// needs: a 1.16 m television seen from 1.5 m and turned 30 degrees is 17 cells a
// side where 14 suffice, and its one-quad error of 76 px falls under 0.75.)
//
// What is NOT here, and what it would take, is written down rather than implied:
//
//   * DEPTH. This makes the picture geometrically right. It does not make it
//     disappear behind a wall, a hand or a person; the overlay pass still has no
//     depth buffer. See `docs/depth-and-occlusion.md` in open77-tv.
//   * A CORNER BEHIND THE CAMERA. Any quad with a corner behind the eye is refused
//     (`CrossesCameraPlane`): the four corners fix the homography, its denominator
//     is each corner's own view depth, and a corner behind the eye makes that
//     denominator change sign. More generally `Solve` accepts a quad only if it
//     is convex -- a projective map that is finite on a convex set keeps it
//     convex -- and the tests throw random quads at it to check that nothing
//     non-convex gets through. What four 2-D points cannot reveal is a quad wholly
//     BEHIND the eye: it projects to the point-reflection of one in front and looks
//     like any other. The producer rules both out before it projects: it measures
//     every corner's view depth (MediaScreens.cpp), and only a screen with all
//     four at least `ScreenClip::kNearDepth` in front of the eye reaches this as
//     four corners. One with some of them nearer -- a very large screen seen from
//     very close, the 150 ft cinema walked along -- is cut at that plane and drawn
//     from its map instead (`ScreenClip.hpp`, `PerspectiveImage::AddClipped`):
//     what `Camera::ProjectPoints` returns for a corner behind the eye is the
//     engine's refusal written as zero (`api/Camera.cpp`), the centre of the view,
//     and a quad over that was drawn on 2026-10-01 as rays of colour across the
//     sky. A quad this refuses for any other reason is drawn exactly as before
//     (`PerspectiveImage::Add`).
//   * A CAPPED GRID. `kMaximumCells` bounds the work. Past it the error is
//     reduced, not removed, and `Choice::errorBoundPixels` reports what is left.
//
// Pure and free of the engine and of ImGui, for the same reason
// `ScreenMotion.hpp` and `api/ScreenQuad.hpp` are: it is arithmetic that a
// session cannot verify and a test can.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace op77::WorldOverlay::ScreenTessellation
{
/// Hard ceiling on cells per side. 48 x 48 is 2304 image quads for one screen,
/// which is well inside what a frame can afford for the one or two screens that
/// are ever this oblique, and is the most anything here will ask the draw list for.
inline constexpr int kMaximumCells = 48;

/// How far, in pixels, a texel may land from where perspective puts it.
/// Three quarters of a pixel is below what a bilinearly filtered picture can show.
inline constexpr float kDefaultTolerancePixels = 0.75F;

/// The smallest ratio of the nearest corner's depth to the farthest that is still
/// treated as a quad in front of the camera. Past it the near corner is ten
/// thousand times closer than the far one and its projection is not a number a
/// float can place a texel with.
inline constexpr double kMinimumDepthRatio = 1.0e-4;

/// Corner coordinates beyond this are refused as not finite. A corner a million
/// pixels away is a perspective divide that has already gone wrong upstream.
inline constexpr double kMaximumCoordinate = 1.0e6;

struct Point
{
    float x{};
    float y{};
};

enum class Status : std::uint8_t
{
    Ok,
    /// A corner is NaN, infinite or absurdly far away.
    NotFinite,
    /// The four corners do not span an area: collinear, coincident, or an edge-on
    /// panel collapsed to a line.
    Degenerate,
    /// The homography's denominator is zero or negative somewhere on the quad, i.e.
    /// the plane crosses the camera's own plane inside the rectangle. The corners
    /// are not the image of a rectangle seen through a camera, and no grid built
    /// from them would be.
    CrossesCameraPlane,
};

[[nodiscard]] inline const char* Describe(const Status aStatus)
{
    switch (aStatus)
    {
    case Status::Ok: return "ok";
    case Status::NotFinite: return "not_finite";
    case Status::Degenerate: return "degenerate";
    case Status::CrossesCameraPlane: return "crosses_camera_plane";
    }
    return "unknown";
}

/// The map from texture space (u right, v DOWN, both 0..1) to pixels:
///
///     x = (a u + b v + c) / (g u + h v + 1)
///     y = (d u + e v + f) / (g u + h v + 1)
///
/// The denominator is the corner's view depth relative to corner 0's, so it is
/// linear in (u, v) and positive everywhere on a quad that is in front of the eye.
struct Homography
{
    double a{1.0}, b{0.0}, c{0.0};
    double d{0.0}, e{1.0}, f{0.0};
    double g{0.0}, h{0.0};

    [[nodiscard]] double Denominator(const double aU, const double aV) const
    {
        return (g * aU) + (h * aV) + 1.0;
    }

    [[nodiscard]] Point Apply(const double aU, const double aV) const
    {
        const double w = Denominator(aU, aV);
        return Point{static_cast<float>(((a * aU) + (b * aV) + c) / w),
                     static_cast<float>(((d * aU) + (e * aV) + f) / w)};
    }
};

/// Solves the unit square -> quad homography. `aCorners` is eight floats in TEXTURE
/// order -- top-left, top-right, bottom-right, bottom-left, i.e. the corners of
/// uv (0,0), (1,0), (1,1), (0,1) -- in pixels, which is exactly what
/// `WebUiService::DrawSurfaceQuad` receives. A mirrored quad (the player is behind
/// the panel, or the picture was turned over) is a legitimate input.
///
/// Every refusal leaves `aOut` untouched, so a caller that ignores the status
/// draws whatever it drew before.
[[nodiscard]] inline Status Solve(const float (&aCorners)[8], Homography& aOut)
{
    double p[4][2]{};
    for (int corner = 0; corner < 4; ++corner)
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            const double value = static_cast<double>(aCorners[(corner * 2) + axis]);
            if (!std::isfinite(value) || std::abs(value) > kMaximumCoordinate)
            {
                return Status::NotFinite;
            }
            p[corner][axis] = value;
        }
    }

    double extent = 0.0;
    for (int corner = 1; corner < 4; ++corner)
    {
        extent = std::max(extent, std::abs(p[corner][0] - p[0][0]));
        extent = std::max(extent, std::abs(p[corner][1] - p[0][1]));
    }
    if (!(extent > 1.0e-3))
    {
        return Status::Degenerate;
    }

    const double x0 = p[0][0], y0 = p[0][1];
    const double x1 = p[1][0], y1 = p[1][1];
    const double x2 = p[2][0], y2 = p[2][1];
    const double x3 = p[3][0], y3 = p[3][1];

    // How far the quad is from a parallelogram. Zero for an affine image, and
    // then the solution below has g = h = 0 and this draws the one quad it always
    // did.
    const double sx = x0 - x1 + x2 - x3;
    const double sy = y0 - y1 + y2 - y3;
    const double dx1 = x1 - x2, dx2 = x3 - x2;
    const double dy1 = y1 - y2, dy2 = y3 - y2;
    const double det = (dx1 * dy2) - (dx2 * dy1);
    if (!(std::abs(det) > 1.0e-7 * extent * extent))
    {
        return Status::Degenerate;
    }

    Homography out;
    out.g = ((sx * dy2) - (dx2 * sy)) / det;
    out.h = ((dx1 * sy) - (sx * dy1)) / det;
    out.a = x1 - x0 + (out.g * x1);
    out.b = x3 - x0 + (out.h * x3);
    out.c = x0;
    out.d = y1 - y0 + (out.g * y1);
    out.e = y3 - y0 + (out.h * y3);
    out.f = y0;

    // The denominator is linear, so its extremes on the square are at the corners.
    const double w[4] = {1.0, 1.0 + out.g, 1.0 + out.g + out.h, 1.0 + out.h};
    const double nearest = std::min(std::min(w[0], w[1]), std::min(w[2], w[3]));
    const double farthest = std::max(std::max(w[0], w[1]), std::max(w[2], w[3]));
    if (!std::isfinite(nearest) || !std::isfinite(farthest))
    {
        return Status::NotFinite;
    }
    if (!(nearest > 0.0) || !(nearest / farthest >= kMinimumDepthRatio))
    {
        return Status::CrossesCameraPlane;
    }

    aOut = out;
    return Status::Ok;
}

/// The six second partial derivatives of the map at (u, v).
///
/// Closed form. With X, Y, W the numerators and denominator above, every second
/// derivative is a linear function of (u, v) over W cubed:
///
///     x_uu = -2 g Nu / W^3            x_vv = -2 h Nv / W^3
///     x_uv = (D W - 2 h Nu) / W^3
///
///     D = a h - b g,   Nu = D v + (a - c g),   Nv = -D u + (b - c h)
///
/// and the same with (d, e, f) for y. Exposed so a test can check it against finite
/// differences: the bound below is only as good as this, and a transcription slip
/// here would be an error bound that is quietly wrong.
struct SecondDerivatives
{
    double xuu{}, xuv{}, xvv{};
    double yuu{}, yuv{}, yvv{};
};

[[nodiscard]] inline SecondDerivatives Derivatives(const Homography& aMap, const double aU,
                                                   const double aV)
{
    const double w = aMap.Denominator(aU, aV);
    const double w3 = w * w * w;
    const auto component = [&](const double aP, const double aQ, const double aR, double& aUU,
                               double& aUV, double& aVV) {
        const double ratio = (aP * aMap.h) - (aQ * aMap.g);
        const double nu = (ratio * aV) + (aP - (aR * aMap.g));
        const double nv = (-ratio * aU) + (aQ - (aR * aMap.h));
        aUU = (-2.0 * aMap.g * nu) / w3;
        aVV = (-2.0 * aMap.h * nv) / w3;
        aUV = ((ratio * w) - (2.0 * aMap.h * nu)) / w3;
    };
    SecondDerivatives out;
    component(aMap.a, aMap.b, aMap.c, out.xuu, out.xuv, out.xvv);
    component(aMap.d, aMap.e, aMap.f, out.yuu, out.yuv, out.yvv);
    return out;
}

/// An upper bound, over the whole unit square, on the magnitude of each second
/// derivative of the map (as a pixel vector).
///
/// Each numerator is linear in (u, v), so its largest magnitude is at a corner, and
/// the denominator is smallest at the nearest corner -- so taking the corner
/// numerators over the NEAREST corner's W cubed bounds every interior point. That is
/// where the error lives anyway: the bound is tight because the curvature, in texel
/// terms, is concentrated at the corner closest to the eye.
struct Curvature
{
    double uu{}; ///< bounds |(x_uu, y_uu)|
    double uv{}; ///< bounds |(x_uv, y_uv)|
    double vv{}; ///< bounds |(x_vv, y_vv)|
};

[[nodiscard]] inline Curvature CurvatureBound(const Homography& aMap)
{
    constexpr double kCorners[4][2] = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
    double nearest = aMap.Denominator(0.0, 0.0);
    for (const auto& corner : kCorners)
    {
        nearest = std::min(nearest, aMap.Denominator(corner[0], corner[1]));
    }
    double xuu = 0.0, xuv = 0.0, xvv = 0.0, yuu = 0.0, yuv = 0.0, yvv = 0.0;
    for (const auto& corner : kCorners)
    {
        const double w = aMap.Denominator(corner[0], corner[1]);
        // Derivatives() divides by this corner's W^3; scale back to the nearest's.
        const double rescale = std::pow(w / nearest, 3.0);
        const SecondDerivatives here = Derivatives(aMap, corner[0], corner[1]);
        xuu = std::max(xuu, std::abs(here.xuu) * rescale);
        xuv = std::max(xuv, std::abs(here.xuv) * rescale);
        xvv = std::max(xvv, std::abs(here.xvv) * rescale);
        yuu = std::max(yuu, std::abs(here.yuu) * rescale);
        yuv = std::max(yuv, std::abs(here.yuv) * rescale);
        yvv = std::max(yvv, std::abs(here.yvv) * rescale);
    }
    return Curvature{std::hypot(xuu, yuu), std::hypot(xuv, yuv), std::hypot(xvv, yvv)};
}

/// A rigorous upper bound, in pixels, on how far any texel lands from where the
/// homography puts it when the quad is drawn as `aCellsU` x `aCellsV` cells with
/// exact vertices, each cell split into two triangles the way `AddImageQuad` does.
///
/// Linear interpolation over a triangle errs by the barycentric mean of the Taylor
/// remainders at its vertices, and for the right triangle of a cell with legs
/// p = 1/cellsU and q = 1/cellsV that mean is at most
///
///     (A p^2 + 2 B p q + C q^2) / 8
///
/// with A, B, C the `Curvature` bounds. The mixed term is real, not slack: a
/// function like u*v is not reproduced by two triangles however the cell is split.
[[nodiscard]] inline double ErrorBound(const Homography& aMap, const int aCellsU,
                                       const int aCellsV)
{
    const Curvature k = CurvatureBound(aMap);
    const double p = 1.0 / static_cast<double>(std::max(1, aCellsU));
    const double q = 1.0 / static_cast<double>(std::max(1, aCellsV));
    return ((k.uu * p * p) + (2.0 * k.uv * p * q) + (k.vv * q * q)) / 8.0;
}

struct Choice
{
    /// Cells per side. 1 means "draw the one quad", and is what a head-on screen
    /// and every affine image gets.
    int cells{1};
    /// The bound at `cells`, in pixels. At most the tolerance unless `capped`.
    double errorBoundPixels{0.0};
    /// The tolerance asked for more than the cap allows. Worth logging once per
    /// screen: it names the set and the angle where the picture is still not exact.
    bool capped{false};
};

/// The smallest square grid whose error bound is within `aTolerancePixels`.
[[nodiscard]] inline Choice ChooseCells(const Homography& aMap,
                                        const float aTolerancePixels = kDefaultTolerancePixels,
                                        const int aMaximumCells = kMaximumCells)
{
    const double tolerance = std::max(0.05, static_cast<double>(aTolerancePixels));
    const int ceiling = std::clamp(aMaximumCells, 1, kMaximumCells);
    const Curvature k = CurvatureBound(aMap);
    const double sum = k.uu + (2.0 * k.uv) + k.vv;

    Choice out;
    out.cells = 1;
    if (std::isfinite(sum) && sum > 8.0 * tolerance)
    {
        const double wanted = std::ceil(std::sqrt(sum / (8.0 * tolerance)));
        out.capped = wanted > static_cast<double>(ceiling);
        out.cells = static_cast<int>(std::min(wanted, static_cast<double>(ceiling)));
    }
    else if (!std::isfinite(sum))
    {
        out.cells = ceiling;
        out.capped = true;
    }
    out.cells = std::clamp(out.cells, 1, ceiling);
    out.errorBoundPixels = ErrorBound(aMap, out.cells, out.cells);
    return out;
}

/// A pixel-space rectangle. Used to skip cells that cannot reach the screen, which
/// matters when a large set fills the view and most of its grid is behind you.
struct Rect
{
    float minX{};
    float minY{};
    float maxX{};
    float maxY{};
};

/// Calls `aVisit(positions, uvs)` once per cell, each an array of four `Point`s in
/// texture order -- top-left, top-right, bottom-right, bottom-left -- which is the
/// argument order of `ImDrawList::AddImageQuad`. A cell whose four corners all lie
/// on the far side of one edge of `aCull` is skipped; pass nullptr to visit all.
///
/// Every vertex is computed once per grid node, so two neighbouring cells hold the
/// SAME float pair for their shared corners and the picture is watertight.
///
/// The grid is cells x cells of the same homography `Solve` returned; with
/// `aCellsU = aCellsV = 1` the single cell's corners are the four inputs.
template <class Visit>
inline void ForEachCell(const Homography& aMap, const int aCellsU, const int aCellsV,
                        const Rect* const aCull, Visit&& aVisit)
{
    const int cellsU = std::clamp(aCellsU, 1, kMaximumCells);
    const int cellsV = std::clamp(aCellsV, 1, kMaximumCells);
    const double stepU = 1.0 / static_cast<double>(cellsU);
    const double stepV = 1.0 / static_cast<double>(cellsV);

    std::array<Point, kMaximumCells + 1> above{};
    std::array<Point, kMaximumCells + 1> below{};
    for (int column = 0; column <= cellsU; ++column)
    {
        above[static_cast<std::size_t>(column)] =
            aMap.Apply(column == cellsU ? 1.0 : column * stepU, 0.0);
    }
    for (int row = 1; row <= cellsV; ++row)
    {
        const double v = row == cellsV ? 1.0 : row * stepV;
        for (int column = 0; column <= cellsU; ++column)
        {
            below[static_cast<std::size_t>(column)] =
                aMap.Apply(column == cellsU ? 1.0 : column * stepU, v);
        }
        const float v0 = static_cast<float>((row - 1) * stepV);
        const float v1 = static_cast<float>(v);
        for (int column = 0; column < cellsU; ++column)
        {
            const auto left = static_cast<std::size_t>(column);
            const Point positions[4] = {above[left], above[left + 1], below[left + 1],
                                        below[left]};
            if (aCull != nullptr)
            {
                const bool outside =
                    (std::max(std::max(positions[0].x, positions[1].x),
                              std::max(positions[2].x, positions[3].x)) < aCull->minX) ||
                    (std::min(std::min(positions[0].x, positions[1].x),
                              std::min(positions[2].x, positions[3].x)) > aCull->maxX) ||
                    (std::max(std::max(positions[0].y, positions[1].y),
                              std::max(positions[2].y, positions[3].y)) < aCull->minY) ||
                    (std::min(std::min(positions[0].y, positions[1].y),
                              std::min(positions[2].y, positions[3].y)) > aCull->maxY);
                if (outside)
                {
                    continue;
                }
            }
            const float u0 = static_cast<float>(column * stepU);
            const float u1 =
                static_cast<float>(column + 1 == cellsU ? 1.0 : (column + 1) * stepU);
            const Point uvs[4] = {Point{u0, v0}, Point{u1, v0}, Point{u1, v1}, Point{u0, v1}};
            aVisit(positions, uvs);
        }
        std::swap(above, below);
    }
}
} // namespace op77::WorldOverlay::ScreenTessellation
