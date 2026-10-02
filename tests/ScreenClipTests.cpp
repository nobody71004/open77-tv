// Pins the arithmetic that draws a television with part of it behind the camera.
//
// The failure this exists for was reported on 2026-10-01 from the 150 ft cinema:
// walking along the screen, with its near end behind the camera, the picture
// turned into rays of colour bars shooting across the sky from the middle of the
// view, and half of it went missing. A corner behind the eye has no projection --
// the engine writes it as the centre of the view -- and the quad over those four
// corners was drawn anyway. The fix (webui/ScreenClip.hpp) cuts the screen at a
// plane just in front of the camera and draws the part in front from the plane's
// own map, solved from points the engine CAN project.
//
// The ground truth here shares nothing with the code under test: a pinhole camera
// and a flat rectangle, ray-traced in double precision. Pinned:
//   * the part in front of the near plane is exactly the polygon the cut makes, for
//     one, two and three corners behind the eye;
//   * the map solved from four projected points puts every texel of that part where
//     the pinhole does, and carries its view depth -- with the engine's float32
//     arithmetic a long way from the world's origin too;
//   * four points that are not one flat screen seen through one camera (one of them
//     refused by the engine) are refused, not drawn;
//   * the depth pass's constants built from the map (ScreenDepth.hpp, BuildFromMap)
//     give every pixel the right texture coordinate and depth, through the shader's
//     own arithmetic (ScreenDepth.hlsli);
//   * the depth pass's fan over the polygon (ScreenDepthPass.hpp) shades exactly the
//     part in front, and a whole screen exactly as before;
//   * the picture is the right way round: the orientation and texture space the
//     producer derives from the map agree with `ScreenQuad::Orient` and
//     `CornerForTexture` on every screen they can both see;
//   * the curvature bound the fallback tessellation relies on is a bound;
//   * cut points two neighbouring cells share are the same to the bit.

#include "api/ScreenQuad.hpp"
#include "webui/ScreenClip.hpp"
#include "webui/ScreenDepth.hpp"
#include "webui/ScreenDepthPass.hpp"
#include "webui/ScreenMotion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
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
namespace Clip = op77::WorldOverlay::ScreenClip;
namespace SD = op77::WorldOverlay::ScreenDepth;
namespace Quad = op77::Api::ScreenQuad;

constexpr double kPi = 3.14159265358979323846;

struct Vec3
{
    double x{}, y{}, z{};
};
Vec3 operator+(const Vec3 a, const Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3 a, const Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3 a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(const Vec3 a, const Vec3 b) { return (a.x * b.x) + (a.y * b.y) + (a.z * b.z); }
Vec3 Cross(const Vec3 a, const Vec3 b)
{
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

/// A pinhole at the origin looking down +z, y up: 60 degrees of vertical field
/// onto `width` x `height` pixels, y down. Positions are given either in pixels or
/// in the overlay's normalised viewport (0..1 from the top-left), the space the
/// producer publishes in.
struct Camera
{
    double width = 1280.0;
    double height = 720.0;
    double focal = (720.0 / 2.0) / std::tan(30.0 * kPi / 180.0);

    [[nodiscard]] std::array<double, 2> Pixel(const Vec3 p) const
    {
        return {(width / 2.0) + (focal * p.x / p.z), (height / 2.0) - (focal * p.y / p.z)};
    }
    [[nodiscard]] std::array<double, 2> Normalised(const Vec3 p) const
    {
        const auto at = Pixel(p);
        return {at[0] / width, at[1] / height};
    }
    /// The ray through a pixel, scaled so its parameter is view depth.
    [[nodiscard]] Vec3 Ray(const double aX, const double aY) const
    {
        return {(aX - (width / 2.0)) / focal, -(aY - (height / 2.0)) / focal, 1.0};
    }
    /// The exact map of a parallelogram `aOrigin + u aU + v aV`, in pixels: each row
    /// is affine because the projection divides by the depth, which is z.
    [[nodiscard]] Clip::Map ExactMap(const Vec3 aOrigin, const Vec3 aU, const Vec3 aV) const
    {
        Clip::Map map;
        const Vec3 columns[3] = {aU, aV, aOrigin};
        for (int column = 0; column < 3; ++column)
        {
            const Vec3 c = columns[column];
            map.m[0][column] = ((width / 2.0) * c.z) + (focal * c.x);
            map.m[1][column] = ((height / 2.0) * c.z) - (focal * c.y);
            map.m[2][column] = c.z;
        }
        return map;
    }
};

/// A screen: its corner at texture (0,0) and its two edges, u along `across` and
/// v (DOWN the picture) along `down`.
struct Screen
{
    Vec3 origin;
    Vec3 across;
    Vec3 down;

    [[nodiscard]] Vec3 At(const double aU, const double aV) const { return origin + (across * aU) + (down * aV); }
    [[nodiscard]] double Depth(const double aU, const double aV) const { return At(aU, aV).z; }

    /// Where a ray from the eye meets it: t (= view depth, the ray's z being 1)
    /// and (u, v); t < 0 when it does not, or not inside the picture.
    [[nodiscard]] double Hit(const Vec3 aDir, double& aU, double& aV) const
    {
        const Vec3 n = Cross(across, down);
        const double denom = Dot(aDir, n);
        if (std::abs(denom) < 1.0e-15)
        {
            return -1.0;
        }
        const double t = Dot(origin, n) / denom;
        if (!(t > 0.0))
        {
            return -1.0;
        }
        // Solve p - origin = u across + v down in the plane.
        const Vec3 d = (aDir * t) - origin;
        const double aa = Dot(across, across);
        const double ab = Dot(across, down);
        const double bb = Dot(down, down);
        const double da = Dot(d, across);
        const double db = Dot(d, down);
        const double det = (aa * bb) - (ab * ab);
        aU = ((da * bb) - (db * ab)) / det;
        aV = ((db * aa) - (da * ab)) / det;
        if (aU < 0.0 || aU > 1.0 || aV < 0.0 || aV > 1.0)
        {
            return -1.0;
        }
        return t;
    }
};

/// A `width` x `height` m screen centred at `centre`, its picture's left-to-right
/// along `right` and its top-to-bottom along minus `up`.
Screen Make(const Vec3 aCentre, const Vec3 aRight, const Vec3 aUp, const double aWidth, const double aHeight)
{
    const Vec3 across = aRight * aWidth;
    const Vec3 down = aUp * (-aHeight);
    return Screen{aCentre - (across * 0.5) - (down * 0.5), across, down};
}

Vec3 Turn(const Vec3 aVector, const double aYawDegrees)
{
    const double yaw = aYawDegrees * kPi / 180.0;
    return {(aVector.x * std::cos(yaw)) + (aVector.z * std::sin(yaw)), aVector.y,
            (-aVector.x * std::sin(yaw)) + (aVector.z * std::cos(yaw))};
}

bool InsidePolygon(const Clip::Polygon& aPolygon, const double aU, const double aV, const double aSlack)
{
    // Convex, either winding: inside when on the same side of every edge.
    int positive = 0;
    int negative = 0;
    for (int i = 0; i < aPolygon.count; ++i)
    {
        const auto& p = aPolygon.at[static_cast<std::size_t>(i)];
        const auto& q = aPolygon.at[static_cast<std::size_t>((i + 1) % aPolygon.count)];
        const double ex = q[0] - p[0];
        const double ey = q[1] - p[1];
        const double length = std::hypot(ex, ey);
        const double side = ((ex * (aV - p[1])) - (ey * (aU - p[0]))) / std::max(length, 1.0e-300);
        if (side > aSlack)
        {
            ++positive;
        }
        else if (side < -aSlack)
        {
            ++negative;
        }
    }
    return positive == 0 || negative == 0;
}

/// What the producer does for a screen partly behind the eye, in normalised
/// viewport units: the region in texture space, four points in it projected, their
/// depths, and the map solved from them.
bool SolveFor(const Screen& aScreen, const Camera& aCamera, Clip::Polygon& aRegion, Clip::Map& aMap)
{
    aRegion = Clip::VisibleRegion(aScreen.Depth(0.0, 0.0), aScreen.Depth(1.0, 0.0), aScreen.Depth(0.0, 1.0));
    double samples[4][2];
    if (!Clip::SamplePoints(aRegion, samples))
    {
        return false;
    }
    double at[4][2];
    double depths[4];
    for (int i = 0; i < 4; ++i)
    {
        const Vec3 p = aScreen.At(samples[i][0], samples[i][1]);
        const auto n = aCamera.Normalised(p);
        at[i][0] = n[0];
        at[i][1] = n[1];
        depths[i] = p.z;
    }
    return Clip::SolveMap(samples, at, depths, Clip::kDefaultPositionTolerance, aMap);
}

// The scenes. The camera is at the origin looking down +z.

/// The cinema, 150 ft by its 16:9 height, as a wall 3 m to the left that runs from
/// 5 m behind the player to 40 m ahead: walked along, its near end behind the eye.
/// Two corners behind, so the part in front has four corners.
Screen Cinema()
{
    return Make(Vec3{-3.0, 4.0, -5.0 + (45.72 / 2.0)}, Vec3{0.0, 0.0, 1.0}, Vec3{0.0, 1.0, 0.0}, 45.72, 25.72);
}

/// A television stood right next to, turned and tilted so that one corner is behind
/// the eye: five corners in front.
Screen OneCornerBehind()
{
    // Depths at (0,0), (1,0), (0,1): -0.3, 0.9, 0.7, so (1,1) is at 1.9.
    return Screen{Vec3{-0.4, 0.3, -0.3}, Vec3{1.1, 0.05, 1.2}, Vec3{0.1, -0.62, 1.0}};
}

/// Three corners behind: only the far corner's triangle is in front.
Screen ThreeCornersBehind()
{
    // Depths -1.6, -0.3, -0.1 and (1,1) at 1.2: the triangle in front is in view.
    return Screen{Vec3{0.35, 0.25, -1.6}, Vec3{-0.2, -0.35, 1.3}, Vec3{-0.45, -0.15, 1.5}};
}
} // namespace

int main()
{
    const Camera camera;

    // -- the part in front of the near plane ----------------------------------------
    {
        struct Case
        {
            const char* name;
            Screen screen;
            int corners;
        };
        const Case cases[] = {
            {"cinema walked along", Cinema(), 4},
            {"one corner behind", OneCornerBehind(), 5},
            {"three corners behind", ThreeCornersBehind(), 3},
            {"wholly in front", Make(Vec3{0.2, 0.1, 2.5}, Turn(Vec3{1.0, 0.0, 0.0}, 40.0), Vec3{0.0, 1.0, 0.0}, 1.6, 0.9),
             4},
        };
        for (const Case& c : cases)
        {
            const Clip::Polygon region = Clip::VisibleRegion(c.screen.Depth(0.0, 0.0), c.screen.Depth(1.0, 0.0),
                                                             c.screen.Depth(0.0, 1.0));
            CHECK(region.count == c.corners);
            for (int i = 0; i < region.count; ++i)
            {
                const auto& corner = region.at[static_cast<std::size_t>(i)];
                CHECK(corner[0] >= -1.0e-12 && corner[0] <= 1.0 + 1.0e-12);
                CHECK(corner[1] >= -1.0e-12 && corner[1] <= 1.0 + 1.0e-12);
                CHECK(c.screen.Depth(corner[0], corner[1]) >= Clip::kNearDepth - 1.0e-9);
            }
            // Every texel nearer than the plane is outside it and every texel past it
            // inside, a hair either side of the line excepted.
            for (int j = 0; j <= 200; ++j)
            {
                for (int i = 0; i <= 200; ++i)
                {
                    const double u = i / 200.0;
                    const double v = j / 200.0;
                    const double depth = c.screen.Depth(u, v);
                    if (std::abs(depth - Clip::kNearDepth) < 1.0e-6)
                    {
                        continue;
                    }
                    CHECK(InsidePolygon(region, u, v, 1.0e-9) == (depth > Clip::kNearDepth));
                }
            }
            std::cout << c.name << ": " << region.count << " corners in front of the near plane\n";
        }
        // All of it behind: nothing.
        CHECK(Clip::VisibleRegion(-2.0, -1.0, -3.0).count == 0);
        CHECK(Clip::VisibleRegion(0.1, 0.15, 0.05).count == 0);
        CHECK(Clip::VisibleRegion(std::nan(""), 1.0, 1.0).count == 0);
    }

    // -- the map, solved from four projected points ---------------------------------
    {
        const Screen screens[] = {Cinema(), OneCornerBehind(), ThreeCornersBehind()};
        for (const Screen& screen : screens)
        {
            Clip::Polygon region;
            Clip::Map map;
            CHECK(SolveFor(screen, camera, region, map));
            // Every texel of the part in front where the pinhole puts it, to a
            // millionth of a pixel, with its depth.
            double worstPixels = 0.0;
            double worstDepth = 0.0;
            for (int j = 0; j <= 100; ++j)
            {
                for (int i = 0; i <= 100; ++i)
                {
                    const double u = i / 100.0;
                    const double v = j / 100.0;
                    if (!InsidePolygon(region, u, v, -1.0e-12))
                    {
                        continue;
                    }
                    const Vec3 p = screen.At(u, v);
                    const auto truth = camera.Normalised(p);
                    const auto at = map.Apply(u, v);
                    worstPixels = std::max(worstPixels, std::hypot((at[0] - truth[0]) * camera.width,
                                                                   (at[1] - truth[1]) * camera.height));
                    worstDepth = std::max(worstDepth, std::abs(map.Depth(u, v) - p.z) / p.z);
                }
            }
            CHECK(worstPixels < 1.0e-6);
            CHECK(worstDepth < 1.0e-9);
            // The region the map's own depth row cuts is the region the corners cut.
            const Clip::Polygon again = Clip::VisibleRegion(map);
            CHECK(again.count == region.count);
            for (int i = 0; i < region.count; ++i)
            {
                const auto& corner = region.at[static_cast<std::size_t>(i)];
                bool found = false;
                for (int k = 0; k < again.count; ++k)
                {
                    const auto& other = again.at[static_cast<std::size_t>(k)];
                    found = found || std::hypot(other[0] - corner[0], other[1] - corner[1]) < 1.0e-9;
                }
                CHECK(found);
            }
        }
    }

    // -- the same through the engine's float32, two kilometres from the origin ------
    {
        // The world as the game has it: float positions far from the origin, a float
        // subtraction to the eye, a float projection. The producer's depths are its
        // own float dot products. Still within a fiftieth of a pixel.
        const Vec3 offset{-2134.37, 1517.82, 31.5};
        const Screen screen = Cinema();
        const auto toFloat = [](const Vec3 p) { return std::array<float, 3>{static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)}; };
        const std::array<float, 3> eye = toFloat(offset);
        const Clip::Polygon region =
            Clip::VisibleRegion(screen.Depth(0.0, 0.0), screen.Depth(1.0, 0.0), screen.Depth(0.0, 1.0));
        double samples[4][2];
        CHECK(Clip::SamplePoints(region, samples));
        double at[4][2];
        double depths[4];
        for (int i = 0; i < 4; ++i)
        {
            const std::array<float, 3> world = toFloat(screen.At(samples[i][0], samples[i][1]) + offset);
            const float x = world[0] - eye[0];
            const float y = world[1] - eye[1];
            const float z = world[2] - eye[2];
            const float focal = static_cast<float>(camera.focal);
            at[i][0] = static_cast<double>((0.5F * static_cast<float>(camera.width) + (focal * x / z)) /
                                           static_cast<float>(camera.width));
            at[i][1] = static_cast<double>((0.5F * static_cast<float>(camera.height) - (focal * y / z)) /
                                           static_cast<float>(camera.height));
            depths[i] = static_cast<double>(z);
        }
        Clip::Map map;
        CHECK(Clip::SolveMap(samples, at, depths, Clip::kDefaultPositionTolerance, map));
        double worst = 0.0;
        for (int j = 0; j <= 50; ++j)
        {
            for (int i = 0; i <= 50; ++i)
            {
                const double u = i / 50.0;
                const double v = j / 50.0;
                if (!InsidePolygon(region, u, v, -1.0e-12))
                {
                    continue;
                }
                const auto truth = camera.Normalised(screen.At(u, v));
                const auto placed = map.Apply(u, v);
                const auto px = camera.Pixel(screen.At(u, v));
                // Only what can be on screen: a texel a hundred screens off the view
                // is placed as well as float can place it, which is not what matters.
                if (px[0] < 0.0 || px[0] > camera.width || px[1] < 0.0 || px[1] > camera.height)
                {
                    continue;
                }
                worst = std::max(worst, std::hypot((placed[0] - truth[0]) * camera.width,
                                                   (placed[1] - truth[1]) * camera.height));
            }
        }
        std::cout << "cinema through float32 at 2 km: worst texel " << worst << " px\n";
        CHECK(worst < 0.05);
    }

    // -- refusals ------------------------------------------------------------------
    {
        const Screen screen = Cinema();
        const Clip::Polygon region =
            Clip::VisibleRegion(screen.Depth(0.0, 0.0), screen.Depth(1.0, 0.0), screen.Depth(0.0, 1.0));
        double samples[4][2];
        CHECK(Clip::SamplePoints(region, samples));
        double at[4][2];
        double depths[4];
        for (int i = 0; i < 4; ++i)
        {
            const Vec3 p = screen.At(samples[i][0], samples[i][1]);
            const auto n = camera.Normalised(p);
            at[i][0] = n[0];
            at[i][1] = n[1];
            depths[i] = p.z;
        }
        Clip::Map map;
        Clip::Map untouched;
        untouched.m[0][0] = 42.0;
        // A point the engine refused, written as the centre of the view.
        {
            double refused[4][2];
            std::copy(&at[0][0], &at[0][0] + 8, &refused[0][0]);
            refused[2][0] = 0.5;
            refused[2][1] = 0.5;
            map = untouched;
            CHECK(!Clip::SolveMap(samples, refused, depths, Clip::kDefaultPositionTolerance, map));
            CHECK(map.m[0][0] == 42.0);
        }
        // A depth that is not in front, or not a number; a position not a number.
        {
            double bad[4];
            std::copy(depths, depths + 4, bad);
            bad[1] = -bad[1];
            CHECK(!Clip::SolveMap(samples, at, bad, Clip::kDefaultPositionTolerance, map));
            bad[1] = std::nan("");
            CHECK(!Clip::SolveMap(samples, at, bad, Clip::kDefaultPositionTolerance, map));
            double nan[4][2];
            std::copy(&at[0][0], &at[0][0] + 8, &nan[0][0]);
            nan[3][1] = std::nan("");
            CHECK(!Clip::SolveMap(samples, nan, depths, Clip::kDefaultPositionTolerance, map));
        }
        // Samples in a line in texture space fix no plane.
        {
            const double line[4][2] = {{0.1, 0.1}, {0.3, 0.3}, {0.5, 0.5}, {0.9, 0.9}};
            CHECK(!Clip::SolveMap(line, at, depths, Clip::kDefaultPositionTolerance, map));
        }
        // Depths that are not the plane's the positions describe: two swapped.
        {
            double off[4];
            std::copy(depths, depths + 4, off);
            std::swap(off[0], off[2]);
            CHECK(!Clip::SolveMap(samples, at, off, Clip::kDefaultPositionTolerance, map));
        }
        // A polygon of nothing gives no samples.
        CHECK(!Clip::SamplePoints(Clip::Polygon{}, samples));
    }

    // -- the depth pass's constants, through the shader's own arithmetic -------------
    {
        const Screen screens[] = {Cinema(), OneCornerBehind(), ThreeCornersBehind()};
        for (const Screen& screen : screens)
        {
            Clip::Polygon region;
            Clip::Map normalised;
            CHECK(SolveFor(screen, camera, region, normalised));
            const Clip::Map pixels = normalised.Scaled(camera.width, camera.height);
            const float size[2] = {static_cast<float>(camera.width), static_cast<float>(camera.height)};
            SD::Constants constants;
            CHECK(SD::BuildFromMap(pixels, SD::DepthModel::ReversedInfinite(0.05), size, size, size, SD::Tuning{},
                                   true, constants) == SD::Status::Ok);
            const SD::Shared::SdConstants c = SD::ToShared(constants);
            int shaded = 0;
            double worstUv = 0.0;
            double worstDepth = 0.0;
            for (int py = 0; py < static_cast<int>(camera.height); py += 2)
            {
                for (int px = 0; px < static_cast<int>(camera.width); px += 2)
                {
                    const Vec3 dir = camera.Ray(px + 0.5, py + 0.5);
                    double u = 0.0, v = 0.0;
                    const double t = screen.Hit(dir, u, v);
                    if (!(t > Clip::kNearDepth))
                    {
                        continue;
                    }
                    const SD::Hlsl::float2 pixel(static_cast<float>(px) + 0.5F, static_cast<float>(py) + 0.5F);
                    const SD::Hlsl::float2 uv = SD::Shared::SdScreenUv(c, pixel);
                    // Texels of a 1920 x 1080 picture.
                    worstUv = std::max(worstUv, std::abs(static_cast<double>(uv.x) - u) * 1920.0);
                    worstUv = std::max(worstUv, std::abs(static_cast<double>(uv.y) - v) * 1080.0);
                    const double inverse = static_cast<double>(SD::Shared::SdPlaneInverseDepth(c, pixel));
                    worstDepth = std::max(worstDepth, std::abs((inverse * t) - 1.0));
                    ++shaded;
                }
            }
            std::cout << "depth pass from the map: " << shaded << " px, uv within " << worstUv
                      << " texel, depth within " << worstDepth << "\n";
            CHECK(shaded > 1000);
            CHECK(worstUv < 0.05);
            CHECK(worstDepth < 1.0e-4);
        }
        // A singular map is refused; so are bad sizes, as for the corners.
        {
            Clip::Map singular;
            singular.m[2][0] = 0.0;
            singular.m[2][1] = 0.0;
            singular.m[2][2] = 0.0;
            const float size[2] = {1280.0F, 720.0F};
            SD::Constants constants;
            CHECK(SD::BuildFromMap(singular, SD::DepthModel{}, size, size, size, SD::Tuning{}, false, constants) ==
                  SD::Status::BadQuad);
            const float zero[2] = {0.0F, 720.0F};
            CHECK(SD::BuildFromMap(Clip::Map{}, SD::DepthModel{}, zero, size, size, SD::Tuning{}, false,
                                   constants) == SD::Status::BadSize);
        }
    }

    // -- the fan the depth pass draws over the polygon ------------------------------
    {
        // The vertex shader's own choice of corner for each of its twelve vertices
        // (ScreenDepthComposite.hlsl, VSMain), from the geometry it is given.
        const auto corner = [](const SD::Geometry& aGeometry, const unsigned aVertex) {
            const unsigned fan = (aVertex / 3) % 4;
            const unsigned within = aVertex % 3;
            const unsigned index = within == 0 ? 0 : fan + within;
            const float* const source =
                index < 2 ? aGeometry.corners01 : (index < 4 ? aGeometry.corners23 : aGeometry.corners45);
            const unsigned offset = (index % 2) * 2;
            return std::array<double, 2>{source[offset], source[offset + 1]};
        };
        const auto area = [](const std::array<double, 2>& a, const std::array<double, 2>& b,
                             const std::array<double, 2>& c) {
            return ((b[0] - a[0]) * (c[1] - a[1])) - ((c[0] - a[0]) * (b[1] - a[1]));
        };
        // A whole screen: the two triangles it always was, (0 1 2) and (0 2 3), and
        // two more with no area.
        {
            const float corners[8] = {100.0F, 50.0F, 900.0F, 80.0F, 950.0F, 600.0F, 60.0F, 650.0F};
            const SD::Geometry g = SD::MakeGeometry(corners, 1280.0F, 720.0F);
            constexpr unsigned kOld[6] = {0, 1, 2, 0, 2, 3};
            for (unsigned vertex = 0; vertex < 6; ++vertex)
            {
                const auto at = corner(g, vertex);
                CHECK(at[0] == corners[kOld[vertex] * 2] && at[1] == corners[(kOld[vertex] * 2) + 1]);
            }
            for (unsigned triangle = 2; triangle < 4; ++triangle)
            {
                CHECK(area(corner(g, triangle * 3), corner(g, (triangle * 3) + 1), corner(g, (triangle * 3) + 2)) ==
                      0.0);
            }
            CHECK(SD::kGeometryVertices == 12);
            CHECK(g.viewport[0] == 1280.0F && g.viewport[1] == 720.0F);
        }
        // A cut screen: the fan covers the polygon exactly -- every pixel inside it
        // in exactly one triangle, none outside it in any.
        const Screen screens[] = {Cinema(), OneCornerBehind(), ThreeCornersBehind()};
        for (const Screen& screen : screens)
        {
            Clip::Polygon region;
            Clip::Map normalised;
            CHECK(SolveFor(screen, camera, region, normalised));
            const Clip::Map pixels = normalised.Scaled(camera.width, camera.height);
            const Clip::Polygon visible = Clip::VisibleRegion(pixels);
            CHECK(visible.count >= 3 && visible.count <= Clip::kMaximumCorners);
            float polygon[2 * Clip::kMaximumCorners]{};
            std::vector<std::array<double, 2>> placed;
            for (int i = 0; i < visible.count; ++i)
            {
                const auto& c = visible.at[static_cast<std::size_t>(i)];
                const auto at = pixels.Apply(c[0], c[1]);
                polygon[i * 2] = static_cast<float>(at[0]);
                polygon[(i * 2) + 1] = static_cast<float>(at[1]);
                placed.push_back({static_cast<double>(polygon[i * 2]), static_cast<double>(polygon[(i * 2) + 1])});
            }
            const SD::Geometry g = SD::MakePolygonGeometry(polygon, visible.count, static_cast<float>(camera.width),
                                                           static_cast<float>(camera.height));
            int inside = 0;
            for (int py = 0; py < static_cast<int>(camera.height); py += 3)
            {
                for (int px = 0; px < static_cast<int>(camera.width); px += 3)
                {
                    const std::array<double, 2> p{px + 0.5, py + 0.5};
                    int covered = 0;  // inside a triangle or on its edge
                    int strictly = 0; // inside, off every edge: two of these would be an overlap
                    double nearestEdge = 1.0e300;
                    for (unsigned triangle = 0; triangle < 4; ++triangle)
                    {
                        const auto a = corner(g, triangle * 3);
                        const auto b = corner(g, (triangle * 3) + 1);
                        const auto c = corner(g, (triangle * 3) + 2);
                        const double whole = area(a, b, c);
                        if (whole == 0.0)
                        {
                            continue;
                        }
                        const double wa = area(b, c, p) / whole;
                        const double wb = area(c, a, p) / whole;
                        const double wc = area(a, b, p) / whole;
                        if (wa >= -1.0e-9 && wb >= -1.0e-9 && wc >= -1.0e-9)
                        {
                            ++covered;
                        }
                        if (wa > 1.0e-9 && wb > 1.0e-9 && wc > 1.0e-9)
                        {
                            ++strictly;
                        }
                    }
                    for (std::size_t i = 0; i < placed.size(); ++i)
                    {
                        const auto& s = placed[i];
                        const auto& e = placed[(i + 1) % placed.size()];
                        const double length = std::hypot(e[0] - s[0], e[1] - s[1]);
                        nearestEdge = std::min(nearestEdge, std::abs(area(s, e, p)) / std::max(length, 1.0e-300));
                    }
                    // The truth: does this pixel's ray meet the screen past the plane?
                    const Vec3 dir = camera.Ray(p[0], p[1]);
                    double u = 0.0, v = 0.0;
                    const bool truth = screen.Hit(dir, u, v) > Clip::kNearDepth;
                    if (nearestEdge < 0.5)
                    {
                        continue; // on an edge, where either answer is the rasteriser's
                    }
                    CHECK(strictly <= 1);
                    if (truth)
                    {
                        ++inside;
                        CHECK(covered >= 1);
                    }
                    else
                    {
                        CHECK(covered == 0);
                    }
                }
            }
            CHECK(inside > 1000);
        }
    }

    // -- which way round the picture goes -----------------------------------------
    {
        // A whole screen, seen in poses the corner rule can answer: square on, turned,
        // from behind, turned over, rolled. The producer's map-based answer (slopes at
        // the middle, then the substitution into texture space) must agree with
        // `ScreenQuad::Orient` and put every texture corner where `CornerForTexture`
        // says the frame does.
        struct Pose
        {
            double yaw;
            double roll;
        };
        const Pose poses[] = {{0.0, 0.0}, {35.0, 0.0}, {-60.0, 0.0}, {160.0, 0.0}, {200.0, 0.0},
                              {0.0, 180.0}, {20.0, 170.0}, {180.0, 180.0}, {10.0, 25.0}};
        for (const Pose& pose : poses)
        {
            const double roll = pose.roll * kPi / 180.0;
            const Vec3 right = Turn(Vec3{std::cos(roll), std::sin(roll), 0.0}, pose.yaw);
            const Vec3 up = Turn(Vec3{-std::sin(roll), std::cos(roll), 0.0}, pose.yaw);
            const Vec3 centre{0.3, -0.2, 3.0};
            const double halfWidth = 0.8;
            const double halfHeight = 0.45;
            // The panel's corners in `ScreenQuad::Corners`' tagged order.
            Vec3 tagged[4];
            for (int side = -1; side <= 1; side += 2)
            {
                for (int row = -1; row <= 1; row += 2)
                {
                    tagged[Quad::CornerIndex(side, row)] = centre + (right * (side * halfWidth)) + (up * (row * halfHeight));
                }
            }
            float projected[8];
            for (int i = 0; i < 4; ++i)
            {
                const auto n = camera.Normalised(tagged[i]);
                projected[i * 2] = static_cast<float>(n[0]);
                projected[(i * 2) + 1] = static_cast<float>(n[1]);
            }
            const Quad::Orientation byCorners = Quad::Orient(projected);

            // The producer's way: the panel's own (a, b) space, a along +right from the
            // -right edge, b along +up from the -up edge.
            const Vec3 origin = tagged[Quad::CornerIndex(-1, -1)];
            const Screen panel{origin, tagged[Quad::CornerIndex(+1, -1)] - origin,
                               tagged[Quad::CornerIndex(-1, +1)] - origin};
            Clip::Polygon region;
            Clip::Map panelMap;
            CHECK(SolveFor(panel, camera, region, panelMap));
            CHECK(region.count == 4);
            const auto middle = region.Centroid();
            const auto slopes = Clip::Slopes(panelMap, middle[0], middle[1]);
            Quad::Orientation byMap{};
            CHECK(std::abs(slopes[0]) > Quad::kOrientationDeadband);
            CHECK(std::abs(slopes[1]) > Quad::kOrientationDeadband);
            byMap.flipU = slopes[0] < 0.0;
            byMap.flipV = slopes[1] > 0.0;
            CHECK(byMap.flipU == byCorners.flipU);
            CHECK(byMap.flipV == byCorners.flipV);

            const Clip::Map texture = Clip::Substituted(panelMap, byMap.flipU ? -1.0 : 1.0, byMap.flipU ? 1.0 : 0.0,
                                                        byMap.flipV ? 1.0 : -1.0, byMap.flipV ? 0.0 : 1.0);
            for (int v = 0; v <= 1; ++v)
            {
                for (int u = 0; u <= 1; ++u)
                {
                    const int source = Quad::CornerForTexture(byCorners, u, v);
                    const auto at = texture.Apply(u, v);
                    CHECK(std::abs(at[0] - projected[source * 2]) < 1.0e-6);
                    CHECK(std::abs(at[1] - projected[(source * 2) + 1]) < 1.0e-6);
                }
            }
        }
    }

    // -- the curvature bound is a bound ---------------------------------------------
    {
        const Screen screens[] = {Cinema(), OneCornerBehind(), ThreeCornersBehind()};
        for (const Screen& screen : screens)
        {
            const Clip::Map map = camera.ExactMap(screen.origin, screen.across, screen.down);
            const Clip::Polygon region = Clip::VisibleRegion(map);
            CHECK(region.count >= 3);
            const Clip::Curvature bound = Clip::CurvatureOver(map, region);
            const double h = 1.0e-4;
            for (int j = 1; j < 40; ++j)
            {
                for (int i = 1; i < 40; ++i)
                {
                    const double u = i / 40.0;
                    const double v = j / 40.0;
                    if (!InsidePolygon(region, u, v, 2.0 * h) || !InsidePolygon(region, u + h, v + h, 0.0) ||
                        !InsidePolygon(region, u - h, v - h, 0.0) || !InsidePolygon(region, u + h, v - h, 0.0) ||
                        !InsidePolygon(region, u - h, v + h, 0.0))
                    {
                        continue;
                    }
                    const auto f = [&map](const double aU, const double aV) { return map.Apply(aU, aV); };
                    const auto c = f(u, v);
                    const auto pu = f(u + h, v);
                    const auto mu = f(u - h, v);
                    const auto pv = f(u, v + h);
                    const auto mv = f(u, v - h);
                    const auto pp = f(u + h, v + h);
                    const auto pm = f(u + h, v - h);
                    const auto mp = f(u - h, v + h);
                    const auto mm = f(u - h, v - h);
                    const double uu = std::hypot((pu[0] - (2.0 * c[0]) + mu[0]) / (h * h),
                                                 (pu[1] - (2.0 * c[1]) + mu[1]) / (h * h));
                    const double vv = std::hypot((pv[0] - (2.0 * c[0]) + mv[0]) / (h * h),
                                                 (pv[1] - (2.0 * c[1]) + mv[1]) / (h * h));
                    const double uv = std::hypot((pp[0] - pm[0] - mp[0] + mm[0]) / (4.0 * h * h),
                                                 (pp[1] - pm[1] - mp[1] + mm[1]) / (4.0 * h * h));
                    CHECK(uu <= (bound.uu * 1.001) + 1.0e-3);
                    CHECK(vv <= (bound.vv * 1.001) + 1.0e-3);
                    CHECK(uv <= (bound.uv * 1.001) + 1.0e-3);
                }
            }
        }
    }

    // -- cut points neighbouring cells share are the same to the bit ----------------
    {
        const double line[3] = {0.731, -1.29, 0.3017};
        const Clip::Polygon left = Clip::Cut(Clip::Rectangle(0.1, 0.2, 0.35, 0.45), line[0], line[1], line[2]);
        const Clip::Polygon right = Clip::Cut(Clip::Rectangle(0.35, 0.2, 0.6, 0.45), line[0], line[1], line[2]);
        int shared = 0;
        for (int i = 0; i < left.count; ++i)
        {
            const auto& p = left.at[static_cast<std::size_t>(i)];
            if (p[0] != 0.35)
            {
                continue;
            }
            bool same = false;
            for (int k = 0; k < right.count; ++k)
            {
                same = same || right.at[static_cast<std::size_t>(k)] == p;
            }
            CHECK(same);
            ++shared;
        }
        CHECK(shared == 2); // one corner of the shared edge, and where the line crosses it
        // A cut that leaves nothing, and one that leaves it all.
        CHECK(Clip::Cut(Clip::Square(), 0.0, 0.0, -1.0).count == 0);
        CHECK(Clip::Cut(Clip::Square(), 0.0, 0.0, 1.0).count == 4);
    }

    // -- a map held between two ticks blends as the corners do ----------------------
    {
        std::array<float, 9> from{};
        std::array<float, 9> to{};
        for (std::size_t i = 0; i < 9; ++i)
        {
            from[i] = static_cast<float>(i);
            to[i] = static_cast<float>(i) * 3.0F;
        }
        namespace Motion = op77::WorldOverlay::ScreenMotion;
        CHECK(Motion::Blend(from, to, 1.0F) == to);
        CHECK(Motion::Blend(from, to, 0.0F) == from);
        const auto middle = Motion::Blend(from, to, 0.5F);
        CHECK(middle[4] == 8.0F);
    }

    std::cout << "ScreenClip: near-plane cut, map from projected points, refusals, depth pass, fan, "
                 "orientation, curvature bound and shared cuts OK\n";
    return 0;
}
