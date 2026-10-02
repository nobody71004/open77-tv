// Pins the arithmetic that makes a television's picture lie in its own plane.
//
// The failure this exists for is the affine texture warp, and it is invisible in
// every way a log can report: the screen is in the right place, the right size,
// on the right prop, drawing every frame, and the picture on it is bent. Head-on
// it is exact, which is why it survives a session of standing in front of a set.
//
// So this file does not test the subdivision against itself. It builds an
// independent ground truth -- a pinhole camera and a planar rectangle, nothing
// shared with the code under test -- projects the four corners the way the
// producer does, hands ONLY those eight floats to `Solve`, and then asks, over a
// dense sample of the quad, how far the picture `AddImageQuad` would draw is from
// the pinhole's own answer. Three claims are pinned that way:
//
//   * four projected corners are enough: `Solve` reproduces the interior;
//   * the cell count `ChooseCells` picks is enough: the measured error never
//     exceeds the bound it reports, and within the cap never exceeds the tolerance;
//   * the bound is not vacuous: it is within a small factor of what is measured, so
//     it cannot be satisfied by subdividing everything forty-eight ways.
//
// The same scenes reproduce the numbers quoted in the header and in open77-tv's
// `tools/quad-affine-error.py`, so a documented figure cannot drift from the code.

#include "webui/ScreenTessellation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace op77::WorldOverlay::ScreenTessellation;

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
// M_PI is POSIX, not C++; MSVC only has it behind _USE_MATH_DEFINES.
constexpr double kPi = 3.14159265358979323846;

using Vec2 = std::array<double, 2>;

struct Vec3
{
    double x{}, y{}, z{};
};

Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec3 operator*(const Vec3& a, const double s)
{
    return {a.x * s, a.y * s, a.z * s};
}

/// Pinhole camera at the origin looking down +Z, Y up, X right.
struct Camera
{
    double width;
    double height;
    double focal;

    Camera(const double aWidth, const double aHeight, const double aVerticalFovDegrees)
        : width(aWidth), height(aHeight),
          focal((aHeight / 2.0) / std::tan(aVerticalFovDegrees * kPi / 360.0))
    {
    }

    [[nodiscard]] Vec2 Project(const Vec3& aPoint) const
    {
        return {(width / 2.0) + (focal * aPoint.x / aPoint.z),
                (height / 2.0) - (focal * aPoint.y / aPoint.z)};
    }
};

/// A rectangle in view space. (u, v) is texture space: u to the right, v DOWN.
struct Rectangle
{
    Vec3 centre;
    Vec3 right;
    Vec3 up;
    double width;
    double height;

    [[nodiscard]] Vec3 At(const double aU, const double aV) const
    {
        return centre + (right * ((aU - 0.5) * width)) + (up * ((0.5 - aV) * height));
    }

    [[nodiscard]] double NearestDepth() const
    {
        double nearest = 1.0e30;
        for (const auto& corner : {At(0, 0), At(1, 0), At(1, 1), At(0, 1)})
        {
            nearest = std::min(nearest, corner.z);
        }
        return nearest;
    }
};

/// The rectangle of open77-tv's `tools/quad-affine-error.py`: centred on the optical
/// axis and turned `aYawDegrees` about the vertical. yaw 0 faces the camera.
Rectangle Turned(const double aWidth, const double aHeight, const double aDistance,
                 const double aYawDegrees)
{
    const double yaw = aYawDegrees * kPi / 180.0;
    return Rectangle{{0.0, 0.0, aDistance},
                     {std::cos(yaw), 0.0, std::sin(yaw)},
                     {0.0, 1.0, 0.0},
                     aWidth,
                     aHeight};
}

Vec3 RotateY(const Vec3& v, const double a)
{
    return {(v.x * std::cos(a)) + (v.z * std::sin(a)), v.y,
            (-v.x * std::sin(a)) + (v.z * std::cos(a))};
}
Vec3 RotateX(const Vec3& v, const double a)
{
    return {v.x, (v.y * std::cos(a)) - (v.z * std::sin(a)),
            (v.y * std::sin(a)) + (v.z * std::cos(a))};
}
Vec3 RotateZ(const Vec3& v, const double a)
{
    return {(v.x * std::cos(a)) - (v.y * std::sin(a)),
            (v.x * std::sin(a)) + (v.y * std::cos(a)), v.z};
}

/// A rectangle turned about all three axes and placed off the optical axis: the
/// general case, which is what a player walking round a set produces.
Rectangle Placed(const double aWidth, const double aHeight, const Vec3& aCentre,
                 const double aYawDegrees, const double aPitchDegrees, const double aRollDegrees)
{
    const double yaw = aYawDegrees * kPi / 180.0;
    const double pitch = aPitchDegrees * kPi / 180.0;
    const double roll = aRollDegrees * kPi / 180.0;
    const auto turn = [&](const Vec3& axis) {
        return RotateZ(RotateX(RotateY(axis, yaw), pitch), roll);
    };
    return Rectangle{aCentre, turn({1.0, 0.0, 0.0}), turn({0.0, 1.0, 0.0}), aWidth, aHeight};
}

/// The four corners in texture order, as the producer publishes them: pixels, eight
/// floats, top-left, top-right, bottom-right, bottom-left.
void CornersOf(const Camera& aCamera, const Rectangle& aRectangle, float (&aOut)[8])
{
    constexpr double kUV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int corner = 0; corner < 4; ++corner)
    {
        const Vec2 pixel = aCamera.Project(aRectangle.At(kUV[corner][0], kUV[corner][1]));
        aOut[corner * 2] = static_cast<float>(pixel[0]);
        aOut[corner * 2 + 1] = static_cast<float>(pixel[1]);
    }
}

struct Lcg
{
    std::uint64_t state;
    double Next()
    {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / 9007199254740992.0;
    }
    double Range(const double aLow, const double aHigh)
    {
        return aLow + ((aHigh - aLow) * Next());
    }
};

/// A random scene in front of a 1920x1080, 60 degree camera. Every corner is at
/// least 0.3 m deep, so the rectangle is one a camera could actually see.
struct Scene
{
    Camera camera{1920.0, 1080.0, 60.0};
    Rectangle rectangle{};
    float corners[8]{};
};

Scene RandomScene(Lcg& aRandom)
{
    for (;;)
    {
        Scene scene;
        const double width = std::exp(aRandom.Range(std::log(0.3), std::log(30.0)));
        const double height = width * aRandom.Range(0.3, 1.2);
        const double distance = std::exp(aRandom.Range(std::log(0.8), std::log(40.0)));
        const Vec3 centre{aRandom.Range(-0.6, 0.6) * distance, aRandom.Range(-0.3, 0.3) * distance,
                          distance};
        scene.rectangle = Placed(width, height, centre, aRandom.Range(-80.0, 80.0),
                                 aRandom.Range(-35.0, 35.0), aRandom.Range(-25.0, 25.0));
        if (scene.rectangle.NearestDepth() < 0.3)
        {
            continue;
        }
        CornersOf(scene.camera, scene.rectangle, scene.corners);
        return scene;
    }
}

/// Where texel (s, t) lands when a quad is drawn as cellsU x cellsV cells, each
/// cell split the way `ImDrawList::AddImageQuad` splits it -- (TL, TR, BR) and
/// (TL, BR, BL) -- with vertices from `aVertex` and texture coordinates
/// interpolated linearly in screen space.
template <class Vertex>
Vec2 Drawn(const Vertex& aVertex, const int aCellsU, const int aCellsV, const double aS,
           const double aT)
{
    const int i = std::min(static_cast<int>(aS * aCellsU), aCellsU - 1);
    const int j = std::min(static_cast<int>(aT * aCellsV), aCellsV - 1);
    const double s0 = static_cast<double>(i) / aCellsU;
    const double s1 = static_cast<double>(i + 1) / aCellsU;
    const double t0 = static_cast<double>(j) / aCellsV;
    const double t1 = static_cast<double>(j + 1) / aCellsV;
    const double ls = (aS - s0) / (s1 - s0);
    const double lt = (aT - t0) / (t1 - t0);
    const Point tl = aVertex(s0, t0), tr = aVertex(s1, t0), br = aVertex(s1, t1),
                bl = aVertex(s0, t1);
    const auto mix = [](const double w0, const Point& p0, const double w1, const Point& p1,
                        const double w2, const Point& p2) {
        return Vec2{(w0 * p0.x) + (w1 * p1.x) + (w2 * p2.x),
                    (w0 * p0.y) + (w1 * p1.y) + (w2 * p2.y)};
    };
    if (ls >= lt)
    {
        return mix(1.0 - ls, tl, ls - lt, tr, lt, br);
    }
    return mix(1.0 - lt, tl, ls, br, lt - ls, bl);
}

/// The largest distance, over a dense sample of the quad, between the picture the
/// grid draws and the pinhole's own answer.
double MeasuredError(const Scene& aScene, const Homography& aMap, const int aCells,
                     const int aSamplesPerCell = 4)
{
    const auto vertex = [&](const double u, const double v) { return aMap.Apply(u, v); };
    const int samples = aCells * aSamplesPerCell;
    double worst = 0.0;
    for (int row = 0; row <= samples; ++row)
    {
        for (int column = 0; column <= samples; ++column)
        {
            const double s = static_cast<double>(column) / samples;
            const double t = static_cast<double>(row) / samples;
            const Vec2 drawn = Drawn(vertex, aCells, aCells, s, t);
            const Vec2 truth = aScene.camera.Project(aScene.rectangle.At(s, t));
            worst = std::max(worst, std::hypot(drawn[0] - truth[0], drawn[1] - truth[1]));
        }
    }
    return worst;
}

/// `Apply` in double, for finite differences: the float `Point` is too coarse to
/// difference twice.
Vec2 ApplyDouble(const Homography& aMap, const double aU, const double aV)
{
    const double w = aMap.Denominator(aU, aV);
    return {((aMap.a * aU) + (aMap.b * aV) + aMap.c) / w,
            ((aMap.d * aU) + (aMap.e * aV) + aMap.f) / w};
}

bool SameBits(const Point& aLeft, const Point& aRight)
{
    return std::memcmp(&aLeft, &aRight, sizeof(Point)) == 0;
}

int CellCount(const Homography& aMap, const int aCells, const Rect* const aCull)
{
    int visited = 0;
    ForEachCell(aMap, aCells, aCells, aCull, [&](const Point (&)[4], const Point (&)[4]) {
        ++visited;
    });
    return visited;
}

/// The cross product at each vertex of the quad taken in order, i.e. twice the signed
/// area of the triangle a corner makes with its two neighbours.
void VertexCrosses(const float (&aCorners)[8], double (&aOut)[4])
{
    for (int vertex = 0; vertex < 4; ++vertex)
    {
        const int a = vertex;
        const int b = (vertex + 1) % 4;
        const int c = (vertex + 2) % 4;
        const double abx = static_cast<double>(aCorners[b * 2]) - aCorners[a * 2];
        const double aby = static_cast<double>(aCorners[b * 2 + 1]) - aCorners[a * 2 + 1];
        const double bcx = static_cast<double>(aCorners[c * 2]) - aCorners[b * 2];
        const double bcy = static_cast<double>(aCorners[c * 2 + 1]) - aCorners[b * 2 + 1];
        aOut[vertex] = (abx * bcy) - (aby * bcx);
    }
}

/// Every vertex turns the same way, and none is straight.
bool IsStrictlyConvex(const float (&aCorners)[8])
{
    double cross[4];
    VertexCrosses(aCorners, cross);
    const bool positive = cross[0] > 0.0 && cross[1] > 0.0 && cross[2] > 0.0 && cross[3] > 0.0;
    const bool negative = cross[0] < 0.0 && cross[1] < 0.0 && cross[2] < 0.0 && cross[3] < 0.0;
    return positive || negative;
}

/// How close a convex quad is to having a straight vertex: the smallest vertex cross
/// product over the largest. Near zero is a sliver.
double ThinnessRatio(const float (&aCorners)[8])
{
    double cross[4];
    VertexCrosses(aCorners, cross);
    double smallest = 1.0e300;
    double largest = 0.0;
    for (const double value : cross)
    {
        smallest = std::min(smallest, std::abs(value));
        largest = std::max(largest, std::abs(value));
    }
    return largest > 0.0 ? smallest / largest : 0.0;
}
} // namespace

int main()
{
    // ------------------------------------------------ the documented numbers
    // The figures in the header of ScreenTessellation.hpp and in the output of
    // tools/quad-affine-error.py are the error of ONE quad (cells = 1). They are
    // reproduced here from the pinhole, so neither can drift from the other.
    {
        const Camera camera{1920.0, 1080.0, 60.0};
        struct Case
        {
            double width, height, distance, yaw, expected, slack;
        };
        const Case cases[] = {
            {1.16, 0.66, 1.5, 0.0, 0.0, 0.05},
            {1.16, 0.66, 1.5, 30.0, 76.0, 2.0},
            {1.16, 0.66, 1.5, 50.0, 103.0, 2.0},
            {30.48, 17.34, 20.4, 35.0, 384.0, 6.0},
        };
        for (const auto& item : cases)
        {
            Scene scene;
            scene.rectangle = Turned(item.width, item.height, item.distance, item.yaw);
            CornersOf(camera, scene.rectangle, scene.corners);
            Homography map;
            CHECK(Solve(scene.corners, map) == Status::Ok);
            // One cell is the quad as drawn today: corners exact, interior affine.
            const double error = MeasuredError(scene, map, 1, 128);
            CHECK(std::abs(error - item.expected) <= item.slack);
        }
    }

    // ------------------------------------------ four corners determine the plane
    {
        Lcg random{0x5EEDULL};
        double worst = 0.0;
        for (int index = 0; index < 400; ++index)
        {
            const Scene scene = RandomScene(random);
            Homography map;
            CHECK(Solve(scene.corners, map) == Status::Ok);
            // The corners themselves, to float rounding.
            constexpr int kUV[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int corner = 0; corner < 4; ++corner)
            {
                const Point at = map.Apply(kUV[corner][0], kUV[corner][1]);
                const double scale =
                    std::max({1.0, std::abs(static_cast<double>(scene.corners[corner * 2])),
                              std::abs(static_cast<double>(scene.corners[corner * 2 + 1]))});
                CHECK(std::abs(static_cast<double>(at.x) - scene.corners[corner * 2]) <=
                      (1.0e-6 * scale) + 1.0e-3);
                CHECK(std::abs(static_cast<double>(at.y) - scene.corners[corner * 2 + 1]) <=
                      (1.0e-6 * scale) + 1.0e-3);
            }
            // The interior, which no corner told it about. Judged where a picture
            // can be seen: both coordinates within a generous ten screens, so float
            // quantisation of a corner a million pixels away cannot decide it.
            double extent = 0.0;
            for (const float value : scene.corners)
            {
                extent = std::max(extent, std::abs(static_cast<double>(value)));
            }
            if (extent > 1.0e4)
            {
                continue;
            }
            for (int probe = 0; probe < 25; ++probe)
            {
                const double s = random.Next();
                const double t = random.Next();
                const Point at = map.Apply(s, t);
                const Vec2 truth = scene.camera.Project(scene.rectangle.At(s, t));
                const double error = std::hypot(static_cast<double>(at.x) - truth[0],
                                                static_cast<double>(at.y) - truth[1]);
                worst = std::max(worst, error);
                CHECK(error <= 0.25);
            }
        }
        // Not a bound anyone relies on -- a record of how exact it actually is.
        CHECK(worst < 0.25);
    }

    // ------------------------------------------ an affine image is one plain quad
    {
        // A parallelogram is what an orthographic or head-on-and-distant view
        // produces. It must come out as the one quad that is drawn today.
        const float parallelogram[8] = {100.0F, 100.0F, 500.0F, 140.0F,
                                        520.0F, 360.0F, 120.0F, 320.0F};
        Homography map;
        CHECK(Solve(parallelogram, map) == Status::Ok);
        CHECK(std::abs(map.g) < 1.0e-12 && std::abs(map.h) < 1.0e-12);
        const Choice choice = ChooseCells(map);
        CHECK(choice.cells == 1 && !choice.capped && choice.errorBoundPixels < 1.0e-6);

        int visited = 0;
        ForEachCell(map, choice.cells, choice.cells, nullptr,
                    [&](const Point (&positions)[4], const Point (&uvs)[4]) {
                        ++visited;
                        for (int corner = 0; corner < 4; ++corner)
                        {
                            CHECK(positions[corner].x == parallelogram[corner * 2]);
                            CHECK(positions[corner].y == parallelogram[corner * 2 + 1]);
                        }
                        CHECK(uvs[0].x == 0.0F && uvs[0].y == 0.0F);
                        CHECK(uvs[1].x == 1.0F && uvs[1].y == 0.0F);
                        CHECK(uvs[2].x == 1.0F && uvs[2].y == 1.0F);
                        CHECK(uvs[3].x == 0.0F && uvs[3].y == 1.0F);
                    });
        CHECK(visited == 1);
    }

    // ------------------------------------------ head-on costs nothing, turned costs
    {
        const Camera camera{1920.0, 1080.0, 60.0};
        int previous = 0;
        for (const double yaw : {0.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0})
        {
            Scene scene;
            scene.rectangle = Turned(1.16, 0.66, 1.5, yaw);
            CornersOf(camera, scene.rectangle, scene.corners);
            Homography map;
            CHECK(Solve(scene.corners, map) == Status::Ok);
            const Choice choice = ChooseCells(map);
            CHECK(choice.cells >= previous);
            CHECK(!choice.capped);
            previous = choice.cells;
            if (yaw == 0.0)
            {
                CHECK(choice.cells == 1);
            }
            if (yaw == 30.0)
            {
                // tools/quad-affine-error.py: 12 cells strictly needed for 1 px and
                // 17 for 0.5 px. The bound asks for a little more, never less.
                CHECK(choice.cells >= 14 && choice.cells <= 22);
            }
        }
        CHECK(previous > 14);
    }

    // ------------------------------------------ the bound is rigorous, and useful
    {
        Lcg random{0xB0075ULL};
        int scenes = 0;
        int wide = 0;
        std::vector<double> ratios;
        for (int index = 0; index < 500; ++index)
        {
            const Scene scene = RandomScene(random);
            Homography map;
            CHECK(Solve(scene.corners, map) == Status::Ok);
            const Choice choice = ChooseCells(map);
            const double measured = MeasuredError(scene, map, choice.cells);
            ++scenes;
            // Rigorous: the error is never more than the bound says. The 0.02 px is
            // float rounding of eight corners, not slack in the maths.
            CHECK(measured <= choice.errorBoundPixels + 0.02);
            if (!choice.capped)
            {
                CHECK(choice.errorBoundPixels <= static_cast<double>(kDefaultTolerancePixels) + 1e-9);
                CHECK(measured <= static_cast<double>(kDefaultTolerancePixels) + 0.02);
            }
            else
            {
                ++wide;
                CHECK(choice.cells == kMaximumCells);
                CHECK(choice.errorBoundPixels > static_cast<double>(kDefaultTolerancePixels));
            }
            if (choice.cells >= 4 && !choice.capped)
            {
                ratios.push_back(measured / choice.errorBoundPixels);
            }
        }
        CHECK(scenes == 500);
        CHECK(ratios.size() > 50);
        // Useful: measured error is a healthy fraction of the bound, so the grid is
        // not being bought at several times what the tolerance needs. The median of
        // measured/bound is about 0.5 for this model; it must not fall below a
        // quarter, which would be a bound requiring twice the cells.
        std::sort(ratios.begin(), ratios.end());
        CHECK(ratios[ratios.size() / 2] >= 0.25);
        // And it is not absurd the other way: it is an upper bound, to float noise.
        CHECK(ratios.back() <= 1.05);
        (void)wide;
    }

    // ------------------------------------------ the cell count against brute force
    // The claim in the header, measured directly: for each scene find by brute force the
    // smallest grid whose measured error is within the tolerance, and compare it with
    // what `ChooseCells` asks for. Never fewer (that would be the bound lying), and not
    // wildly more (that would be a bound that can be satisfied by doing everything).
    // Measured over 1500 scenes while writing this: median 1.4x, 90th percentile 2.0x,
    // worst 2.5x, never below 1x.
    {
        Lcg random{0xFEEDULL};
        std::vector<double> asked;
        for (int index = 0; index < 400; ++index)
        {
            const Scene scene = RandomScene(random);
            Homography map;
            CHECK(Solve(scene.corners, map) == Status::Ok);
            const Choice choice = ChooseCells(map);
            if (choice.capped)
            {
                continue;
            }
            int needed = 0;
            for (int cells = 1; cells <= kMaximumCells; ++cells)
            {
                if (MeasuredError(scene, map, cells, 6) <= static_cast<double>(kDefaultTolerancePixels))
                {
                    needed = cells;
                    break;
                }
            }
            CHECK(needed >= 1);
            CHECK(choice.cells >= needed);
            if (choice.cells > 1 || needed > 1)
            {
                asked.push_back(static_cast<double>(choice.cells) / needed);
            }
        }
        CHECK(asked.size() > 150);
        std::sort(asked.begin(), asked.end());
        CHECK(asked.front() >= 1.0);
        CHECK(asked[asked.size() / 2] <= 1.7);
        CHECK(asked.back() <= 3.0);
    }

    // ------------------------------------------ the cap is honoured and reported
    {
        const Camera camera{1920.0, 1080.0, 60.0};
        Scene scene;
        scene.rectangle = Turned(30.48, 17.34, 20.4, 60.0); // cinema.100ft, 60 degrees
        CornersOf(camera, scene.rectangle, scene.corners);
        Homography map;
        CHECK(Solve(scene.corners, map) == Status::Ok);
        const Choice choice = ChooseCells(map);
        CHECK(choice.capped && choice.cells == kMaximumCells);
        CHECK(choice.errorBoundPixels > static_cast<double>(kDefaultTolerancePixels));
        // Still an improvement of two orders of magnitude over the one quad.
        const double oneQuad = MeasuredError(scene, map, 1, 64);
        const double capped = MeasuredError(scene, map, choice.cells, 2);
        CHECK(oneQuad > 600.0);
        CHECK(capped < 12.0);
        CHECK(capped <= choice.errorBoundPixels + 0.05);
        // A lower cap is honoured exactly.
        const Choice low = ChooseCells(map, kDefaultTolerancePixels, 10);
        CHECK(low.cells == 10 && low.capped);
        // A cap above the ceiling is clamped to it, never obeyed.
        const Choice high = ChooseCells(map, kDefaultTolerancePixels, 10000);
        CHECK(high.cells == kMaximumCells);
    }

    // ------------------------------------------ the grid is exact and watertight
    {
        Lcg random{0xC311ULL};
        for (int index = 0; index < 40; ++index)
        {
            const Scene scene = RandomScene(random);
            Homography map;
            CHECK(Solve(scene.corners, map) == Status::Ok);
            const int cellsU = 1 + static_cast<int>(random.Range(0.0, 9.0));
            const int cellsV = 1 + static_cast<int>(random.Range(0.0, 9.0));

            struct Cell
            {
                Point positions[4];
                Point uvs[4];
            };
            std::vector<Cell> cells;
            ForEachCell(map, cellsU, cellsV, nullptr,
                        [&](const Point (&positions)[4], const Point (&uvs)[4]) {
                            Cell cell;
                            std::memcpy(cell.positions, positions, sizeof(cell.positions));
                            std::memcpy(cell.uvs, uvs, sizeof(cell.uvs));
                            cells.push_back(cell);
                        });
            CHECK(static_cast<int>(cells.size()) == cellsU * cellsV);

            for (int row = 0; row < cellsV; ++row)
            {
                for (int column = 0; column < cellsU; ++column)
                {
                    const Cell& cell = cells[static_cast<std::size_t>((row * cellsU) + column)];
                    // Every vertex is the homography at its own uv.
                    for (int corner = 0; corner < 4; ++corner)
                    {
                        const Point expected =
                            map.Apply(static_cast<double>(cell.uvs[corner].x),
                                      static_cast<double>(cell.uvs[corner].y));
                        CHECK(std::abs(expected.x - cell.positions[corner].x) <= 1.0e-3F *
                              std::max(1.0F, std::abs(expected.x)));
                        CHECK(std::abs(expected.y - cell.positions[corner].y) <= 1.0e-3F *
                              std::max(1.0F, std::abs(expected.y)));
                    }
                    // The right neighbour shares its left edge, bit for bit.
                    if (column + 1 < cellsU)
                    {
                        const Cell& next = cells[static_cast<std::size_t>((row * cellsU) + column + 1)];
                        CHECK(SameBits(cell.positions[1], next.positions[0]));
                        CHECK(SameBits(cell.positions[2], next.positions[3]));
                        CHECK(cell.uvs[1].x == next.uvs[0].x && cell.uvs[2].x == next.uvs[3].x);
                    }
                    // The cell below shares its top edge.
                    if (row + 1 < cellsV)
                    {
                        const Cell& below = cells[static_cast<std::size_t>(((row + 1) * cellsU) + column)];
                        CHECK(SameBits(cell.positions[3], below.positions[0]));
                        CHECK(SameBits(cell.positions[2], below.positions[1]));
                        CHECK(cell.uvs[3].y == below.uvs[0].y && cell.uvs[2].y == below.uvs[1].y);
                    }
                }
            }
            // The uvs tile the unit square exactly, ending on 1.0 and not on 0.99999.
            CHECK(cells.front().uvs[0].x == 0.0F && cells.front().uvs[0].y == 0.0F);
            CHECK(cells.back().uvs[2].x == 1.0F && cells.back().uvs[2].y == 1.0F);
            // The outer corners are the four inputs.
            for (int corner = 0; corner < 4; ++corner)
            {
                const Cell& outer = corner == 0 ? cells.front()
                    : corner == 1 ? cells[static_cast<std::size_t>(cellsU - 1)]
                    : corner == 2 ? cells.back()
                                  : cells[static_cast<std::size_t>((cellsV - 1) * cellsU)];
                const Point& got = outer.positions[corner];
                const double scale =
                    std::max({1.0, std::abs(static_cast<double>(scene.corners[corner * 2])),
                              std::abs(static_cast<double>(scene.corners[corner * 2 + 1]))});
                CHECK(std::abs(static_cast<double>(got.x) - scene.corners[corner * 2]) <=
                      (1.0e-6 * scale) + 1.0e-3);
                CHECK(std::abs(static_cast<double>(got.y) - scene.corners[corner * 2 + 1]) <=
                      (1.0e-6 * scale) + 1.0e-3);
            }
        }
    }

    // ------------------------------------------ culling skips only the invisible
    {
        const Camera camera{1920.0, 1080.0, 60.0};
        Scene scene;
        // A big set close enough that much of it is outside the frame.
        scene.rectangle = Turned(12.0, 6.8, 6.0, 35.0);
        CornersOf(camera, scene.rectangle, scene.corners);
        Homography map;
        CHECK(Solve(scene.corners, map) == Status::Ok);
        const int cells = 16;
        const Rect frame{0.0F, 0.0F, 1920.0F, 1080.0F};

        int kept = 0;
        ForEachCell(map, cells, cells, &frame, [&](const Point (&positions)[4], const Point (&)[4]) {
            ++kept;
            // A kept cell may not be wholly outside on any one side.
            float lowX = positions[0].x, highX = positions[0].x;
            float lowY = positions[0].y, highY = positions[0].y;
            for (const auto& point : positions)
            {
                lowX = std::min(lowX, point.x);
                highX = std::max(highX, point.x);
                lowY = std::min(lowY, point.y);
                highY = std::max(highY, point.y);
            }
            CHECK(!(highX < frame.minX) && !(lowX > frame.maxX));
            CHECK(!(highY < frame.minY) && !(lowY > frame.maxY));
        });
        CHECK(kept > 0 && kept < cells * cells);

        // And every cell that does touch the frame was kept: compare to a full walk.
        int touching = 0;
        ForEachCell(map, cells, cells, nullptr, [&](const Point (&positions)[4], const Point (&)[4]) {
            float lowX = positions[0].x, highX = positions[0].x;
            float lowY = positions[0].y, highY = positions[0].y;
            for (const auto& point : positions)
            {
                lowX = std::min(lowX, point.x);
                highX = std::max(highX, point.x);
                lowY = std::min(lowY, point.y);
                highY = std::max(highY, point.y);
            }
            if (!(highX < frame.minX) && !(lowX > frame.maxX) && !(highY < frame.minY) &&
                !(lowY > frame.maxY))
            {
                ++touching;
            }
        });
        CHECK(touching == kept);

        const Rect everything{-1.0e9F, -1.0e9F, 1.0e9F, 1.0e9F};
        CHECK(CellCount(map, cells, &everything) == cells * cells);
        CHECK(CellCount(map, cells, nullptr) == cells * cells);

        // A rectangle the quad never reaches. Coordinates are bounded by
        // kMaximumCoordinate (1e6), so 2e6..3e6 is empty by construction. (An earlier
        // draft used a small off-screen box, but this quad is bigger than the frame and
        // does extend under it - the culler was right and the test was wrong.)
        const Rect nothing{2.0e6F, 2.0e6F, 3.0e6F, 3.0e6F};
        CHECK(CellCount(map, cells, &nothing) == 0);

        // For any other rectangle the culled walk must agree with a full walk that
        // tests each cell against it: off-screen boxes the quad does reach, slivers,
        // and a box strictly inside the frame.
        const Rect boxes[] = {
            {-100.0F, -100.0F, -50.0F, -50.0F},
            {-5000.0F, 400.0F, -10.0F, 500.0F},
            {900.0F, 500.0F, 1000.0F, 600.0F},
            {1919.0F, 0.0F, 1921.0F, 1080.0F},
            {0.0F, 1079.0F, 1920.0F, 1081.0F},
            {-50.0F, -50.0F, 5.0F, 5.0F},
        };
        for (const Rect& box : boxes)
        {
            int expected = 0;
            ForEachCell(map, cells, cells, nullptr,
                        [&](const Point (&positions)[4], const Point (&)[4]) {
                float lowX = positions[0].x, highX = positions[0].x;
                float lowY = positions[0].y, highY = positions[0].y;
                for (const auto& point : positions)
                {
                    lowX = std::min(lowX, point.x);
                    highX = std::max(highX, point.x);
                    lowY = std::min(lowY, point.y);
                    highY = std::max(highY, point.y);
                }
                if (!(highX < box.minX) && !(lowX > box.maxX) && !(highY < box.minY) &&
                    !(lowY > box.maxY))
                {
                    ++expected;
                }
            });
            CHECK(CellCount(map, cells, &box) == expected);
        }
    }

    // ------------------------------------------ what is not a screen is refused
    {
        const Camera camera{1920.0, 1080.0, 60.0};
        Scene good;
        good.rectangle = Turned(1.16, 0.66, 1.5, 30.0);
        CornersOf(camera, good.rectangle, good.corners);

        // A refusal leaves the output alone: a caller that ignores the status draws
        // what it drew before.
        const auto untouched = [](const Homography& map) {
            return map.a == 7.0 && map.b == 8.0 && map.c == 9.0 && map.d == 1.0 &&
                   map.e == 2.0 && map.f == 3.0 && map.g == 0.25 && map.h == 0.5;
        };
        const auto sentinel = [] {
            Homography map;
            map.a = 7.0, map.b = 8.0, map.c = 9.0, map.d = 1.0;
            map.e = 2.0, map.f = 3.0, map.g = 0.25, map.h = 0.5;
            return map;
        };

        float broken[8];
        std::memcpy(broken, good.corners, sizeof(broken));
        broken[3] = std::nanf("");
        Homography map = sentinel();
        CHECK(Solve(broken, map) == Status::NotFinite && untouched(map));

        std::memcpy(broken, good.corners, sizeof(broken));
        broken[0] = INFINITY;
        CHECK(Solve(broken, map) == Status::NotFinite && untouched(map));

        std::memcpy(broken, good.corners, sizeof(broken));
        broken[5] = 2.0e7F;
        CHECK(Solve(broken, map) == Status::NotFinite && untouched(map));

        const float coincident[8] = {10, 10, 10, 10, 10, 10, 10, 10};
        CHECK(Solve(coincident, map) == Status::Degenerate && untouched(map));

        const float collinear[8] = {0, 0, 100, 100, 200, 200, 300, 300};
        CHECK(Solve(collinear, map) == Status::Degenerate && untouched(map));

        // A bow-tie: two adjacent corners swapped. This is what a corner behind the
        // camera very often looks like after the perspective divide, and it must not
        // be drawn as a picture.
        float bowtie[8];
        std::memcpy(bowtie, good.corners, sizeof(bowtie));
        std::swap(bowtie[2], bowtie[4]);
        std::swap(bowtie[3], bowtie[5]);
        CHECK(Solve(bowtie, map) == Status::CrossesCameraPlane && untouched(map));

        // A concave "dart": the bottom-right corner pulled inside the triangle the
        // other three make. (An earlier draft of this test used a shape that only
        // looked like an arrowhead and was convex, so it was - correctly - accepted.
        // `IsStrictlyConvex` is here so that the shape's concavity is checked rather
        // than eyeballed.)
        const float dart[8] = {100, 100, 500, 100, 250, 200, 100, 500};
        CHECK(!IsStrictlyConvex(dart));
        Homography dartMap = sentinel();
        CHECK(Solve(dart, dartMap) == Status::CrossesCameraPlane && untouched(dartMap));

        // And a convex shape that is merely lopsided is a screen.
        const float lopsided[8] = {100, 100, 500, 100, 450, 450, 100, 500};
        CHECK(IsStrictlyConvex(lopsided));
        Homography lopsidedMap = sentinel();
        CHECK(Solve(lopsided, lopsidedMap) == Status::Ok);

        CHECK(std::string(Describe(Status::Ok)) == "ok");
        CHECK(std::string(Describe(Status::NotFinite)) == "not_finite");
        CHECK(std::string(Describe(Status::Degenerate)) == "degenerate");
        CHECK(std::string(Describe(Status::CrossesCameraPlane)) == "crosses_camera_plane");
    }

    // ------------------------------------------ a screen is accepted exactly when it is convex
    // Not a property of the implementation but of the geometry: a projective map that
    // is finite on a convex set keeps it convex, and a convex quadrilateral is always
    // the image of a square under such a map. So `Solve` must never accept a quad whose
    // corners, taken in texture order, are not convex, and must accept every convex one
    // except the near-degenerate ones whose depth ratio is below kMinimumDepthRatio.
    // Random quads in a box make the check independent of any camera model.
    {
        Lcg random{0xC0DEULL};
        long convexAccepted = 0;
        long convexRefused = 0;
        long concaveRefused = 0;
        for (int index = 0; index < 300000; ++index)
        {
            float corners[8];
            for (float& value : corners)
            {
                value = static_cast<float>(random.Range(0.0, 2000.0));
            }
            Homography map;
            const Status status = Solve(corners, map);
            if (IsStrictlyConvex(corners))
            {
                if (status == Status::Ok)
                {
                    ++convexAccepted;
                }
                else
                {
                    ++convexRefused;
                    // Only a sliver may be refused: one whose thinnest corner is under
                    // a thousandth of its fattest.
                    CHECK(ThinnessRatio(corners) < 1.0e-3);
                }
            }
            else
            {
                CHECK(status != Status::Ok);
                ++concaveRefused;
            }
        }
        // The fuzz must actually have visited both populations (about 23% of random
        // quads are convex), and almost every convex quad must have been accepted.
        CHECK(convexAccepted > 50000);
        CHECK(concaveRefused > 150000);
        CHECK(convexRefused * 1000 < convexAccepted);
    }

    // ------------------------------------------ a corner behind the camera
    // The producer drops a screen whose CENTRE is behind the eye (depth <= 0.05 in
    // MediaScreens.cpp). A centre in front does not keep the corners in front - a wall
    // sized screen seen from close has one edge beside and behind the viewer - and what
    // reaches `Solve` then is the plain perspective divide of a corner with negative
    // depth. It must be refused, not drawn as a picture.
    {
        const Camera camera{1920.0, 1080.0, 60.0};
        Lcg random{0xBE41DULL};
        int visited = 0;
        int crossing = 0;
        for (int index = 0; index < 20000 && visited < 2000; ++index)
        {
            const double width = random.Range(3.0, 40.0);
            const double height = width * random.Range(0.3, 1.0);
            const double distance = random.Range(0.3, 4.0);
            const Vec3 centre{random.Range(-1.0, 1.0) * distance,
                              random.Range(-0.5, 0.5) * distance, distance};
            const Rectangle rectangle =
                Placed(width, height, centre, random.Range(-89.0, 89.0),
                       random.Range(-30.0, 30.0), random.Range(-20.0, 20.0));
            // The producer's gate passes (centre in front) and a corner is clearly behind.
            if (rectangle.centre.z <= 0.05 || rectangle.NearestDepth() > -0.05)
            {
                continue;
            }
            float corners[8];
            CornersOf(camera, rectangle, corners);
            Homography map;
            const Status status = Solve(corners, map);
            // NotFinite is a refusal too (a corner a hair behind the eye projects to
            // an absurd coordinate); what matters is that it is never Ok.
            CHECK(status == Status::CrossesCameraPlane || status == Status::NotFinite);
            crossing += status == Status::CrossesCameraPlane ? 1 : 0;
            ++visited;
        }
        CHECK(visited >= 500);
        CHECK(crossing * 10 >= visited * 9);

        // What the corners cannot reveal, pinned so nobody mistakes it for a bug in
        // `Solve`: a screen wholly behind the eye projects to the point-reflection of
        // one in front, and is accepted. The centre-depth gate upstream is what keeps
        // that from ever being drawn.
        Scene reflected;
        reflected.rectangle = Turned(1.16, 0.66, -1.5, 30.0);
        CornersOf(camera, reflected.rectangle, reflected.corners);
        Homography map;
        CHECK(Solve(reflected.corners, map) == Status::Ok);
    }

    // ------------------------------------------ a mirrored quad is a legitimate quad
    {
        const Camera camera{1920.0, 1080.0, 60.0};
        Scene scene;
        scene.rectangle = Turned(1.16, 0.66, 1.5, 40.0);
        CornersOf(camera, scene.rectangle, scene.corners);
        // The picture turned over: texture left on the panel's right. This is what
        // `ScreenQuad::Orient` produces when the player walks round to the back.
        const float mirrored[8] = {scene.corners[2], scene.corners[3], scene.corners[0],
                                   scene.corners[1], scene.corners[6], scene.corners[7],
                                   scene.corners[4], scene.corners[5]};
        Homography original, flipped;
        CHECK(Solve(scene.corners, original) == Status::Ok);
        CHECK(Solve(mirrored, flipped) == Status::Ok);
        CHECK(ChooseCells(original).cells == ChooseCells(flipped).cells);
        const Point at = flipped.Apply(0.0, 0.0);
        CHECK(std::abs(at.x - scene.corners[2]) < 1.0e-3F && std::abs(at.y - scene.corners[3]) < 1.0e-3F);
    }

    // ------------------------------------------ the closed-form curvature is right
    {
        Lcg random{0xD1FFULL};
        for (int index = 0; index < 60; ++index)
        {
            const Scene scene = RandomScene(random);
            double extent = 0.0;
            for (const float value : scene.corners)
            {
                extent = std::max(extent, std::abs(static_cast<double>(value)));
            }
            if (extent > 1.0e4)
            {
                continue;
            }
            Homography map;
            CHECK(Solve(scene.corners, map) == Status::Ok);
            const Curvature bound = CurvatureBound(map);
            for (int probe = 0; probe < 40; ++probe)
            {
                const double u = random.Range(0.05, 0.95);
                const double v = random.Range(0.05, 0.95);
                const SecondDerivatives analytic = Derivatives(map, u, v);

                // Central differences, in double.
                const double step = 1.0e-3;
                const Vec2 centre = ApplyDouble(map, u, v);
                const Vec2 right = ApplyDouble(map, u + step, v), left = ApplyDouble(map, u - step, v);
                const Vec2 down = ApplyDouble(map, u, v + step), up = ApplyDouble(map, u, v - step);
                const Vec2 rd = ApplyDouble(map, u + step, v + step), ru = ApplyDouble(map, u + step, v - step);
                const Vec2 ld = ApplyDouble(map, u - step, v + step), lu = ApplyDouble(map, u - step, v - step);
                const double xuu = (right[0] - (2.0 * centre[0]) + left[0]) / (step * step);
                const double yuu = (right[1] - (2.0 * centre[1]) + left[1]) / (step * step);
                const double xvv = (down[0] - (2.0 * centre[0]) + up[0]) / (step * step);
                const double yvv = (down[1] - (2.0 * centre[1]) + up[1]) / (step * step);
                const double xuv = (rd[0] - ru[0] - ld[0] + lu[0]) / (4.0 * step * step);
                const double yuv = (rd[1] - ru[1] - ld[1] + lu[1]) / (4.0 * step * step);
                const auto close = [](const double a, const double b) {
                    return std::abs(a - b) <= 2.0e-3 * (1.0 + std::abs(b));
                };
                CHECK(close(analytic.xuu, xuu) && close(analytic.yuu, yuu));
                CHECK(close(analytic.xvv, xvv) && close(analytic.yvv, yvv));
                CHECK(close(analytic.xuv, xuv) && close(analytic.yuv, yuv));

                // And the corner-based bound really bounds every interior point.
                CHECK(std::hypot(analytic.xuu, analytic.yuu) <= (bound.uu * (1.0 + 1.0e-9)) + 1.0e-9);
                CHECK(std::hypot(analytic.xuv, analytic.yuv) <= (bound.uv * (1.0 + 1.0e-9)) + 1.0e-9);
                CHECK(std::hypot(analytic.xvv, analytic.yvv) <= (bound.vv * (1.0 + 1.0e-9)) + 1.0e-9);
            }
        }
    }

    // ------------------------------------------ the bound covers the whole square, not just the corners
    // The curvature bound is built from the four corners, scaled to the NEAREST corner's
    // depth. The scaling is what makes it a bound: a function N(u,v)/W(u,v)^3 with N and W
    // both linear can peak in the interior of the square, and in a minority of scenes
    // (about one in a hundred here) it peaks above every corner value taken at the
    // corner's own depth - by up to three times. Sample a dense grid, corners and edges
    // included, in many scenes, require the bound to hold everywhere, and require that the
    // generator really does produce scenes where the naive corner maximum would have been
    // exceeded, so this test cannot quietly lose its teeth.
    {
        Lcg random{0xB0DDULL};
        int scenes = 0;
        int naiveWouldFail = 0;
        constexpr int kGrid = 16;
        for (int index = 0; index < 30000; ++index)
        {
            const Scene scene = RandomScene(random);
            Homography map;
            if (Solve(scene.corners, map) != Status::Ok)
            {
                continue;
            }
            ++scenes;
            const Curvature bound = CurvatureBound(map);

            double interiorUU = 0.0, interiorUV = 0.0, interiorVV = 0.0;
            for (int row = 0; row <= kGrid; ++row)
            {
                for (int column = 0; column <= kGrid; ++column)
                {
                    const SecondDerivatives d =
                        Derivatives(map, static_cast<double>(column) / kGrid,
                                    static_cast<double>(row) / kGrid);
                    interiorUU = std::max(interiorUU, std::hypot(d.xuu, d.yuu));
                    interiorUV = std::max(interiorUV, std::hypot(d.xuv, d.yuv));
                    interiorVV = std::max(interiorVV, std::hypot(d.xvv, d.yvv));
                }
            }
            CHECK(interiorUU <= (bound.uu * (1.0 + 1.0e-9)) + 1.0e-9);
            CHECK(interiorUV <= (bound.uv * (1.0 + 1.0e-9)) + 1.0e-9);
            CHECK(interiorVV <= (bound.vv * (1.0 + 1.0e-9)) + 1.0e-9);

            // The same bound built without the depth scaling: each corner's own value.
            double cornerXUU = 0.0, cornerXUV = 0.0, cornerXVV = 0.0;
            double cornerYUU = 0.0, cornerYUV = 0.0, cornerYVV = 0.0;
            for (const auto& corner : {std::array<double, 2>{0, 0}, std::array<double, 2>{1, 0},
                                       std::array<double, 2>{1, 1}, std::array<double, 2>{0, 1}})
            {
                const SecondDerivatives d = Derivatives(map, corner[0], corner[1]);
                cornerXUU = std::max(cornerXUU, std::abs(d.xuu));
                cornerXUV = std::max(cornerXUV, std::abs(d.xuv));
                cornerXVV = std::max(cornerXVV, std::abs(d.xvv));
                cornerYUU = std::max(cornerYUU, std::abs(d.yuu));
                cornerYUV = std::max(cornerYUV, std::abs(d.yuv));
                cornerYVV = std::max(cornerYVV, std::abs(d.yvv));
            }
            const double naiveUU = std::hypot(cornerXUU, cornerYUU);
            const double naiveUV = std::hypot(cornerXUV, cornerYUV);
            const double naiveVV = std::hypot(cornerXVV, cornerYVV);
            if (interiorUU > naiveUU * (1.0 + 1.0e-9) || interiorUV > naiveUV * (1.0 + 1.0e-9) ||
                interiorVV > naiveVV * (1.0 + 1.0e-9))
            {
                ++naiveWouldFail;
            }
        }
        CHECK(scenes > 20000);
        CHECK(naiveWouldFail >= 10);
    }

    // ------------------------------------------ a corner a hair in front of the eye
    // In front of the camera, so the denominator is positive - but the nearest corner is
    // more than ten thousand times closer than the farthest, so its projection is a
    // number no float can place a texel with. Refused, and the threshold is pinned from
    // both sides. (Depth 0.002 and 0.004 against 30 m: ratios of 6.7e-5 and 1.3e-4
    // against the limit of 1e-4.)
    {
        const Camera camera{1920.0, 1080.0, 60.0};
        const auto cornersFor = [&](const double aNearDepth, float (&aOut)[8]) {
            const Vec3 world[4] = {{0.4, 0.3, aNearDepth},
                                   {8.0, 0.3, 30.0},
                                   {8.0, -0.3, 30.0},
                                   {0.4, -0.3, aNearDepth}};
            for (int corner = 0; corner < 4; ++corner)
            {
                const Vec2 pixel = camera.Project(world[corner]);
                aOut[corner * 2] = static_cast<float>(pixel[0]);
                aOut[corner * 2 + 1] = static_cast<float>(pixel[1]);
            }
        };
        float corners[8];
        Homography map;

        cornersFor(0.002, corners);
        CHECK(IsStrictlyConvex(corners));
        CHECK(Solve(corners, map) == Status::CrossesCameraPlane);

        cornersFor(0.004, corners);
        CHECK(IsStrictlyConvex(corners));
        CHECK(Solve(corners, map) == Status::Ok);
        // and what it accepts it can subdivide without overflowing the grid.
        const Choice choice = ChooseCells(map);
        CHECK(choice.cells >= 1 && choice.cells <= kMaximumCells);
    }

    std::cout << "ScreenTessellation: corners, bound, cap, watertight grid, culling and refusals OK\n";
    return 0;
}
