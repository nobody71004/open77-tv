#pragma once

// A world screen with part of it behind the camera: the part in front, exactly.
//
// ---------------------------------------------------------------------------
// THE PROBLEM THIS SOLVES
// ---------------------------------------------------------------------------
//
// Everything else that draws a television starts from its four projected corners.
// A corner behind the eye has no projection: the engine refuses the point and
// `Camera::ProjectPoints` writes it as zero, the centre of the view. On a set seen
// from in front that never happens, and `ScreenTessellation` refuses such a quad
// anyway (`CrossesCameraPlane`) -- but the refusal fell back to the old single
// image quad over those four corners, one of them now at the centre of the view.
// On the 150 ft cinema screen, which a player walks right up to and along, that is
// what was reported on 2026-10-01: rays of colour bars shooting across the sky from
// the middle of the view, and half the picture missing where the quad was folded
// back on itself, whenever the near end of the screen was behind the camera.
//
// ---------------------------------------------------------------------------
// WHAT THIS DOES
// ---------------------------------------------------------------------------
//
// The screen is a flat rectangle, so its view depth is AFFINE in texture space:
// depth(u, v) = d00 + u (d10 - d00) + v (d01 - d00), from three corners' depths.
// The part in front of a near plane is the unit square cut by one straight line in
// (u, v): a convex polygon of three, four or five corners (`VisibleRegion`).
// Points inside it project correctly, so a few of them, with their texture
// coordinates and view depths, fix the plane's own map from texture space to the
// view (`SolveMap`): a homography whose third row is the view depth itself, so it
// carries the depth the depth test needs as well. The renderers then draw that
// polygon: the depth pass computes every pixel's texture coordinate and depth from
// the map's inverse and needs only the polygon's corners to know which pixels to
// shade; the fallback without it cuts the polygon to the view and draws it as a
// grid fine enough that the picture lies in its plane (`PerspectiveImage`).
//
// Pure and free of the engine, of ImGui and of D3D, so `tests/ScreenClipTests.cpp`
// can check it against a ray-traced camera.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace op77::WorldOverlay::ScreenClip
{
/// Where the picture stops: this many metres in front of the camera. Anything
/// nearer projects hundreds of screens away, and the engine's own near plane is
/// about this close too, so nothing that could be on screen is lost.
inline constexpr double kNearDepth = 0.2;

/// At most five corners: a square cut by one line.
inline constexpr int kMaximumCorners = 5;

/// Room for the square cut by the near plane and the four sides of the view, and
/// then by a grid cell's four sides: every cut adds at most one corner.
inline constexpr int kMaximumPolygon = 16;

/// How far, in the units of the samples' positions, a sample may lie from the map
/// solved through it before the samples are refused as not one flat screen seen
/// through one camera. In the overlay's normalised viewport that is one percent of
/// the view: float rounding is thousands of times smaller, and a point the engine
/// refused to project (written as the centre of the view) is far larger.
inline constexpr double kDefaultPositionTolerance = 0.01;

/// Texture space -> view: (u, v, 1) -> (x w, y w, w). `x` and `y` are in whatever
/// space the samples' positions were (the overlay's normalised viewport, or its
/// pixels), and `w` is the view depth in metres: positive in front of the camera.
struct Map
{
    double m[3][3]{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};

    [[nodiscard]] double Depth(const double aU, const double aV) const
    {
        return (m[2][0] * aU) + (m[2][1] * aV) + m[2][2];
    }

    /// The point at (u, v). Only meaningful where `Depth` is positive.
    [[nodiscard]] std::array<double, 2> Apply(const double aU, const double aV) const
    {
        const double w = Depth(aU, aV);
        return {((m[0][0] * aU) + (m[0][1] * aV) + m[0][2]) / w, ((m[1][0] * aU) + (m[1][1] * aV) + m[1][2]) / w};
    }

    /// The same map with its positions scaled: normalised viewport to pixels.
    [[nodiscard]] Map Scaled(const double aX, const double aY) const
    {
        Map out = *this;
        for (int column = 0; column < 3; ++column)
        {
            out.m[0][column] *= aX;
            out.m[1][column] *= aY;
        }
        return out;
    }

    /// Row by row, as nine floats: how an overlay item carries it.
    void Store(float* const aOut) const
    {
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                aOut[(row * 3) + column] = static_cast<float>(m[row][column]);
            }
        }
    }

    [[nodiscard]] static Map Load(const float* const aValues)
    {
        Map out;
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                out.m[row][column] = static_cast<double>(aValues[(row * 3) + column]);
            }
        }
        return out;
    }

    /// Every entry finite.
    [[nodiscard]] bool Finite() const
    {
        for (const auto& row : m)
        {
            for (const double value : row)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }
        }
        return true;
    }
};

/// A convex polygon in texture space, corners in order round its edge. Empty
/// (`count` 0) when there is nothing of it: fewer than three corners, or no area.
struct Polygon
{
    std::array<std::array<double, 2>, kMaximumPolygon> at{};
    int count{};

    [[nodiscard]] std::array<double, 2> Centroid() const
    {
        std::array<double, 2> sum{0.0, 0.0};
        for (int i = 0; i < count; ++i)
        {
            sum[0] += at[static_cast<std::size_t>(i)][0];
            sum[1] += at[static_cast<std::size_t>(i)][1];
        }
        const double n = static_cast<double>(std::max(1, count));
        return {sum[0] / n, sum[1] / n};
    }

    /// Twice the signed area (positive counter-clockwise in u right, v down... the
    /// sign is only ever compared with zero).
    [[nodiscard]] double TwiceArea() const
    {
        double sum = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const auto& p = at[static_cast<std::size_t>(i)];
            const auto& q = at[static_cast<std::size_t>((i + 1) % count)];
            sum += (p[0] * q[1]) - (q[0] * p[1]);
        }
        return sum;
    }
};

/// The whole of the picture: texture coordinates (0,0), (1,0), (1,1), (0,1).
[[nodiscard]] inline Polygon Square()
{
    Polygon out;
    out.at[0] = {0.0, 0.0};
    out.at[1] = {1.0, 0.0};
    out.at[2] = {1.0, 1.0};
    out.at[3] = {0.0, 1.0};
    out.count = 4;
    return out;
}

/// A rectangle of texture space, corners in the same order as `Square`.
[[nodiscard]] inline Polygon Rectangle(const double aU0, const double aV0, const double aU1, const double aV1)
{
    Polygon out;
    out.at[0] = {aU0, aV0};
    out.at[1] = {aU1, aV0};
    out.at[2] = {aU1, aV1};
    out.at[3] = {aU0, aV1};
    out.count = 4;
    return out;
}

/// The part of `aPolygon` where a u + b v + c >= 0 (Sutherland-Hodgman, one line).
///
/// A crossing point is computed from its edge's two corners taken in a fixed order,
/// not in the order the polygon walks them, so two polygons that share an edge --
/// neighbouring cells of a grid, walked in opposite directions -- get the SAME
/// point, to the bit, and the picture drawn from them has no seam.
[[nodiscard]] inline Polygon Cut(const Polygon& aPolygon, const double aA, const double aB, const double aC)
{
    Polygon out;
    if (aPolygon.count < 3)
    {
        return out;
    }
    const auto value = [&](const std::array<double, 2>& aPoint) {
        return (aA * aPoint[0]) + (aB * aPoint[1]) + aC;
    };
    const auto add = [&out](const std::array<double, 2>& aPoint) {
        if (out.count > 0 && out.at[static_cast<std::size_t>(out.count - 1)] == aPoint)
        {
            return;
        }
        if (out.count < kMaximumPolygon)
        {
            out.at[static_cast<std::size_t>(out.count)] = aPoint;
            ++out.count;
        }
    };
    for (int i = 0; i < aPolygon.count; ++i)
    {
        const auto& p = aPolygon.at[static_cast<std::size_t>(i)];
        const auto& q = aPolygon.at[static_cast<std::size_t>((i + 1) % aPolygon.count)];
        const double fp = value(p);
        const double fq = value(q);
        if (fp >= 0.0)
        {
            add(p);
        }
        if ((fp >= 0.0) != (fq >= 0.0))
        {
            const bool flip = (q[0] < p[0]) || (q[0] == p[0] && q[1] < p[1]);
            const auto& from = flip ? q : p;
            const auto& to = flip ? p : q;
            const double fFrom = flip ? fq : fp;
            const double fTo = flip ? fp : fq;
            const double t = fFrom / (fFrom - fTo);
            add({from[0] + ((to[0] - from[0]) * t), from[1] + ((to[1] - from[1]) * t)});
        }
    }
    if (out.count > 1 && out.at[0] == out.at[static_cast<std::size_t>(out.count - 1)])
    {
        --out.count;
    }
    if (out.count < 3 || !(std::abs(out.TwiceArea()) > 1.0e-15))
    {
        out.count = 0;
    }
    return out;
}

/// The unit square cut by depth(u, v) >= `aNear`, depth being affine with the given
/// values at (0,0), (1,0) and (0,1). Three, four or five corners, or empty when all
/// of it is nearer than `aNear`.
[[nodiscard]] inline Polygon VisibleRegion(const double aDepth00, const double aDepth10, const double aDepth01,
                                           const double aNear = kNearDepth)
{
    if (!std::isfinite(aDepth00) || !std::isfinite(aDepth10) || !std::isfinite(aDepth01))
    {
        return Polygon{};
    }
    return Cut(Square(), aDepth10 - aDepth00, aDepth01 - aDepth00, aDepth00 - aNear);
}

/// The same, from a map: its third row is the depth.
[[nodiscard]] inline Polygon VisibleRegion(const Map& aMap, const double aNear = kNearDepth)
{
    return Cut(Square(), aMap.m[2][0], aMap.m[2][1], aMap.m[2][2] - aNear);
}

/// `aRegion` (in front of the camera: the map's depth positive on it) cut to the
/// rectangle of positions `aMinX..aMaxX`, `aMinY..aMaxY` the map's positions are
/// in -- the part of the screen that is in view. Each side of the view is a straight
/// line in texture space: x >= aMinX is x w - aMinX w >= 0 where w > 0, and both
/// are affine in (u, v).
[[nodiscard]] inline Polygon InView(const Polygon& aRegion, const Map& aMap, const double aMinX, const double aMinY,
                                    const double aMaxX, const double aMaxY)
{
    const auto& m = aMap.m;
    Polygon out = Cut(aRegion, m[0][0] - (aMinX * m[2][0]), m[0][1] - (aMinX * m[2][1]), m[0][2] - (aMinX * m[2][2]));
    out = Cut(out, (aMaxX * m[2][0]) - m[0][0], (aMaxX * m[2][1]) - m[0][1], (aMaxX * m[2][2]) - m[0][2]);
    out = Cut(out, m[1][0] - (aMinY * m[2][0]), m[1][1] - (aMinY * m[2][1]), m[1][2] - (aMinY * m[2][2]));
    return Cut(out, (aMaxY * m[2][0]) - m[1][0], (aMaxY * m[2][1]) - m[1][1], (aMaxY * m[2][2]) - m[1][2]);
}

/// Four points inside the polygon to solve the map from, well spread so the solve
/// is well conditioned: three corners pulled a quarter of the way to the centroid
/// and the centroid itself (a triangle), or four of its corners pulled in (more
/// corners: the ones dropped are those whose neighbours are closest together, which
/// loses the least of the spread). Pulled in so each lies strictly inside, in front
/// of the near plane and off the edges. False for an empty polygon.
[[nodiscard]] inline bool SamplePoints(const Polygon& aPolygon, double (&aOut)[4][2])
{
    if (aPolygon.count < 3)
    {
        return false;
    }
    const std::array<double, 2> centroid = aPolygon.Centroid();
    const auto pulled = [&centroid](const std::array<double, 2>& aCorner, double (&aTo)[2]) {
        aTo[0] = aCorner[0] + ((centroid[0] - aCorner[0]) * 0.25);
        aTo[1] = aCorner[1] + ((centroid[1] - aCorner[1]) * 0.25);
    };
    if (aPolygon.count == 3)
    {
        pulled(aPolygon.at[0], aOut[0]);
        pulled(aPolygon.at[1], aOut[1]);
        pulled(aPolygon.at[2], aOut[2]);
        aOut[3][0] = centroid[0];
        aOut[3][1] = centroid[1];
        return true;
    }
    Polygon kept = aPolygon;
    while (kept.count > 4)
    {
        int drop = 0;
        double shortest = 1.0e300;
        for (int i = 0; i < kept.count; ++i)
        {
            const auto& before = kept.at[static_cast<std::size_t>((i + kept.count - 1) % kept.count)];
            const auto& after = kept.at[static_cast<std::size_t>((i + 1) % kept.count)];
            const double gap = std::hypot(after[0] - before[0], after[1] - before[1]);
            if (gap < shortest)
            {
                shortest = gap;
                drop = i;
            }
        }
        for (int i = drop; i + 1 < kept.count; ++i)
        {
            kept.at[static_cast<std::size_t>(i)] = kept.at[static_cast<std::size_t>(i + 1)];
        }
        --kept.count;
    }
    for (int i = 0; i < 4; ++i)
    {
        pulled(kept.at[static_cast<std::size_t>(i)], aOut[i]);
    }
    return true;
}

/// The map from four samples: their texture coordinates, where they landed (`aAt`,
/// in any one 2-D space: the overlay's normalised viewport, or pixels), and their
/// view depths in metres.
///
/// Every row of the map is affine in (u, v): the third is the view depth, a plane
/// in texture space because the screen is flat, and the first two are the position
/// times that depth, because a perspective camera divides by it (any offset of the
/// camera's centre, a jitter included, is the depth times a constant, and affine
/// too). So each row is the least-squares plane through four values, solved from
/// the samples' spread in TEXTURE space -- well conditioned however small, distant
/// or oblique the screen looks, which a solve from four positions on screen is not.
///
/// False, `aOut` untouched, when a value is not finite, a depth is not in front of
/// the camera, the samples are (nearly) in a line in texture space, or a sample
/// lies more than `aTolerance` from where the solved map puts it, or its depth
/// more than a part in a thousand: positions that are not one flat screen seen
/// through one perspective camera.
[[nodiscard]] inline bool SolveMap(const double (&aUv)[4][2], const double (&aAt)[4][2], const double (&aDepth)[4],
                                   const double aTolerance, Map& aOut)
{
    double deepest = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        if (!std::isfinite(aUv[i][0]) || !std::isfinite(aUv[i][1]) || !std::isfinite(aAt[i][0]) ||
            !std::isfinite(aAt[i][1]) || !std::isfinite(aDepth[i]) || !(aDepth[i] > 0.0))
        {
            return false;
        }
        deepest = std::max(deepest, aDepth[i]);
    }
    // The normal equations of the fit: N = sum r r^T with r = (u, v, 1).
    double n[3][3]{};
    for (int i = 0; i < 4; ++i)
    {
        const double r[3] = {aUv[i][0], aUv[i][1], 1.0};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                n[row][column] += r[row] * r[column];
            }
        }
    }
    const double det = (n[0][0] * ((n[1][1] * n[2][2]) - (n[1][2] * n[2][1]))) -
                       (n[0][1] * ((n[1][0] * n[2][2]) - (n[1][2] * n[2][0]))) +
                       (n[0][2] * ((n[1][0] * n[2][1]) - (n[1][1] * n[2][0])));
    // Texture coordinates are 0..1; four samples spread over a polygon of any useful
    // size give a determinant of 1e-6 or more. Below this they are in a line.
    if (!std::isfinite(det) || !(std::abs(det) > 1.0e-12))
    {
        return false;
    }
    double inverse[3][3];
    inverse[0][0] = ((n[1][1] * n[2][2]) - (n[1][2] * n[2][1])) / det;
    inverse[0][1] = ((n[0][2] * n[2][1]) - (n[0][1] * n[2][2])) / det;
    inverse[0][2] = ((n[0][1] * n[1][2]) - (n[0][2] * n[1][1])) / det;
    inverse[1][0] = ((n[1][2] * n[2][0]) - (n[1][0] * n[2][2])) / det;
    inverse[1][1] = ((n[0][0] * n[2][2]) - (n[0][2] * n[2][0])) / det;
    inverse[1][2] = ((n[0][2] * n[1][0]) - (n[0][0] * n[1][2])) / det;
    inverse[2][0] = ((n[1][0] * n[2][1]) - (n[1][1] * n[2][0])) / det;
    inverse[2][1] = ((n[0][1] * n[2][0]) - (n[0][0] * n[2][1])) / det;
    inverse[2][2] = ((n[0][0] * n[1][1]) - (n[0][1] * n[1][0])) / det;

    Map map;
    for (int row = 0; row < 3; ++row)
    {
        // The values this row of the map must take at the samples.
        double rhs[3]{};
        for (int i = 0; i < 4; ++i)
        {
            const double value = row == 2 ? aDepth[i] : aAt[i][row] * aDepth[i];
            rhs[0] += aUv[i][0] * value;
            rhs[1] += aUv[i][1] * value;
            rhs[2] += value;
        }
        for (int column = 0; column < 3; ++column)
        {
            map.m[row][column] =
                (inverse[column][0] * rhs[0]) + (inverse[column][1] * rhs[1]) + (inverse[column][2] * rhs[2]);
        }
    }
    if (!map.Finite())
    {
        return false;
    }
    const double tolerance = std::isfinite(aTolerance) && aTolerance > 0.0 ? aTolerance : kDefaultPositionTolerance;
    for (int i = 0; i < 4; ++i)
    {
        const double depth = map.Depth(aUv[i][0], aUv[i][1]);
        if (!(depth > 0.0) || std::abs(depth - aDepth[i]) > 1.0e-3 * deepest)
        {
            return false;
        }
        const std::array<double, 2> at = map.Apply(aUv[i][0], aUv[i][1]);
        if (!(std::hypot(at[0] - aAt[i][0], at[1] - aAt[i][1]) <= tolerance))
        {
            return false;
        }
    }
    aOut = map;
    return true;
}

/// The inverse of a map: (x, y, 1) -> (u, v, 1) / depth. Its third row is
/// 1 / view depth at a position, an affine function of it, which is what the depth
/// pass's `plane` register is. False for a singular map.
[[nodiscard]] inline bool Invert(const Map& aMap, double (&aOut)[3][3])
{
    const auto& h = aMap.m;
    const double det = (h[0][0] * ((h[1][1] * h[2][2]) - (h[1][2] * h[2][1]))) -
                       (h[0][1] * ((h[1][0] * h[2][2]) - (h[1][2] * h[2][0]))) +
                       (h[0][2] * ((h[1][0] * h[2][1]) - (h[1][1] * h[2][0])));
    if (!std::isfinite(det) || std::abs(det) < 1.0e-300)
    {
        return false;
    }
    aOut[0][0] = ((h[1][1] * h[2][2]) - (h[1][2] * h[2][1])) / det;
    aOut[0][1] = ((h[0][2] * h[2][1]) - (h[0][1] * h[2][2])) / det;
    aOut[0][2] = ((h[0][1] * h[1][2]) - (h[0][2] * h[1][1])) / det;
    aOut[1][0] = ((h[1][2] * h[2][0]) - (h[1][0] * h[2][2])) / det;
    aOut[1][1] = ((h[0][0] * h[2][2]) - (h[0][2] * h[2][0])) / det;
    aOut[1][2] = ((h[0][2] * h[1][0]) - (h[0][0] * h[1][2])) / det;
    aOut[2][0] = ((h[1][0] * h[2][1]) - (h[1][1] * h[2][0])) / det;
    aOut[2][1] = ((h[0][1] * h[2][0]) - (h[0][0] * h[2][1])) / det;
    aOut[2][2] = ((h[0][0] * h[1][1]) - (h[0][1] * h[1][0])) / det;
    for (const auto& row : aOut)
    {
        for (const double value : row)
        {
            if (!std::isfinite(value))
            {
                return false;
            }
        }
    }
    return true;
}

/// The same map with texture space re-expressed: u -> aScaleU u + aOffsetU and
/// v -> aScaleV v + aOffsetV. How a map solved over the panel's own axes becomes one
/// over the picture's, once it is known which way round the picture goes.
[[nodiscard]] inline Map Substituted(const Map& aMap, const double aScaleU, const double aOffsetU,
                                     const double aScaleV, const double aOffsetV)
{
    Map out;
    for (int row = 0; row < 3; ++row)
    {
        const double* const r = aMap.m[row];
        out.m[row][0] = r[0] * aScaleU;
        out.m[row][1] = r[1] * aScaleV;
        out.m[row][2] = r[2] + (r[0] * aOffsetU) + (r[1] * aOffsetV);
    }
    return out;
}

/// The direction the picture's axes run in on screen at (u, v): d x / d u and
/// d y / d v of the map's positions. What `ScreenQuad::Orient` asks of the four
/// projected corners, asked where the screen is actually in front of the camera.
[[nodiscard]] inline std::array<double, 2> Slopes(const Map& aMap, const double aU, const double aV)
{
    const auto& m = aMap.m;
    const double w = aMap.Depth(aU, aV);
    const double x = (m[0][0] * aU) + (m[0][1] * aV) + m[0][2];
    const double y = (m[1][0] * aU) + (m[1][1] * aV) + m[1][2];
    return {((m[0][0] * w) - (x * m[2][0])) / (w * w), ((m[1][1] * w) - (y * m[2][1])) / (w * w)};
}

/// Bounds on the second derivatives of the map's positions over a polygon in front
/// of the camera, as `ScreenTessellation::CurvatureBound` bounds them over the
/// whole square: every numerator is affine in (u, v) and the depth too, so the
/// largest numerator and the smallest depth are both at corners.
///
///     x_uu = -2 g Nu / w^3     x_vv = -2 h Nv / w^3     x_uv = (D w - 2 h Nu) / w^3
///     Nu = D v + (a i - c g),  Nv = -D u + (b i - c h),  D = a h - b g
///
/// for a row (a b c) of positions over the depth row (g h i), and the same for y.
struct Curvature
{
    double uu{};
    double uv{};
    double vv{};
};

[[nodiscard]] inline Curvature CurvatureOver(const Map& aMap, const Polygon& aPolygon)
{
    const auto& m = aMap.m;
    const double g = m[2][0];
    const double h = m[2][1];
    const double i = m[2][2];
    double nearest = 1.0e300;
    double xuu = 0.0, xuv = 0.0, xvv = 0.0, yuu = 0.0, yuv = 0.0, yvv = 0.0;
    for (int k = 0; k < aPolygon.count; ++k)
    {
        const double u = aPolygon.at[static_cast<std::size_t>(k)][0];
        const double v = aPolygon.at[static_cast<std::size_t>(k)][1];
        const double w = aMap.Depth(u, v);
        nearest = std::min(nearest, w);
        const auto numerators = [&](const double* const aRow, double& aUU, double& aUV, double& aVV) {
            const double a = aRow[0];
            const double b = aRow[1];
            const double c = aRow[2];
            const double d = (a * h) - (b * g);
            const double nu = (d * v) + ((a * i) - (c * g));
            const double nv = (-d * u) + ((b * i) - (c * h));
            aUU = std::max(aUU, std::abs(2.0 * g * nu));
            aVV = std::max(aVV, std::abs(2.0 * h * nv));
            aUV = std::max(aUV, std::abs((d * w) - (2.0 * h * nu)));
        };
        numerators(m[0], xuu, xuv, xvv);
        numerators(m[1], yuu, yuv, yvv);
    }
    if (!(nearest > 0.0))
    {
        return Curvature{1.0e300, 1.0e300, 1.0e300};
    }
    const double cube = nearest * nearest * nearest;
    return Curvature{std::hypot(xuu, yuu) / cube, std::hypot(xuv, yuv) / cube, std::hypot(xvv, yvv) / cube};
}
} // namespace op77::WorldOverlay::ScreenClip
