// Pins the arithmetic that puts a television's picture behind whatever is in
// front of it.
//
// The failure this exists for is the one in the original report: the picture is
// drawn over V's hands and over the room, because the overlay has no depth test.
// The fix is a pixel shader that compares the screen's own depth with the game's
// depth buffer, and every number it compares is computed by webui/ScreenDepth.hlsli
// -- the shader's source, compiled here as C++ -- from constants ScreenDepth.hpp
// builds. So this file does not test the shader against itself. It builds an
// independent ground truth: a pinhole camera, a planar television, and things in
// front of it (a hand, a pillar, a wall), ray-traced exactly in double precision.
// It renders the game's depth buffer from that scene the way a game would (device
// depth in one of three conventions, at full or upscaler resolution, 24-bit or
// float), hands the shader ONLY what the overlay really has -- the four projected
// corners, the centre's view depth, the depth convention, the depth buffer -- and
// then asks, pixel by pixel, whether the picture shows where the ray says it does.
//
// Pinned:
//   * the texture coordinate is exact everywhere on the screen (no affine warp);
//   * the screen's depth is exact everywhere on it, from one centre depth;
//   * a pixel is hidden exactly where something is in front of the screen, up to
//     a band a pixel or two wide along the edge of what is in front;
//   * the prop's own screen mesh, a few millimetres in front of the picture, does
//     NOT hide it -- and would, without the tolerance (so the tolerance is pinned
//     as necessary, not decorative);
//   * all three depth conventions, an upscaler's half-resolution depth, and depth
//     drawn into a corner of a larger texture (dynamic resolution) work;
//   * bad input is refused, not drawn.

#include "webui/ScreenDepth.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace SD = op77::WorldOverlay::ScreenDepth;

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

// A pinhole camera at the origin looking down +Z, Y up, pixels with y down.
struct Camera
{
    int width = 1280;
    int height = 720;
    double focal = 0.0;

    explicit Camera(const double aVerticalFovDegrees)
    {
        focal = (height / 2.0) / std::tan(aVerticalFovDegrees * kPi / 360.0);
    }

    [[nodiscard]] std::array<double, 2> Project(const Vec3 p) const
    {
        return {(width / 2.0) + (focal * p.x / p.z), (height / 2.0) - (focal * p.y / p.z)};
    }

    // A ray through a point in pixels, scaled so that t is view depth (dir.z == 1).
    [[nodiscard]] Vec3 Ray(const double aX, const double aY) const
    {
        return {(aX - (width / 2.0)) / focal, -(aY - (height / 2.0)) / focal, 1.0};
    }
};

// A rectangle: centre, right and up (unit), and its size.
struct Rectangle
{
    Vec3 centre, right, up;
    double width = 1.0, height = 1.0;

    [[nodiscard]] Vec3 Normal() const { return Cross(right, up); }
    [[nodiscard]] Vec3 Corner(const double aU, const double aV) const
    {
        return centre + (right * ((aU - 0.5) * width)) + (up * ((0.5 - aV) * height));
    }

    // t (view depth) and uv where a ray meets it, or t < 0.
    [[nodiscard]] double Hit(const Vec3 aDir, double& aU, double& aV) const
    {
        const Vec3 n = Normal();
        const double denom = Dot(aDir, n);
        if (std::abs(denom) < 1.0e-12)
        {
            return -1.0;
        }
        const double t = Dot(centre, n) / denom;
        if (!(t > 0.0))
        {
            return -1.0;
        }
        const Vec3 p = aDir * t;
        const Vec3 topLeft = Corner(0.0, 0.0);
        aU = Dot(p - topLeft, right) / width;
        aV = Dot(topLeft - p, up) / height;
        if (aU < 0.0 || aU > 1.0 || aV < 0.0 || aV > 1.0)
        {
            return -1.0;
        }
        return t;
    }
};

Rectangle Television(const double aWidth, const double aHeight, const double aDistance,
                     const double aYawDegrees, const double aSide = 0.0, const double aLift = 0.0)
{
    const double yaw = aYawDegrees * kPi / 180.0;
    Rectangle r;
    r.centre = {aSide, aLift, aDistance};
    r.right = {std::cos(yaw), 0.0, std::sin(yaw)};
    r.up = {0.0, 1.0, 0.0};
    r.width = aWidth;
    r.height = aHeight;
    return r;
}

// An axis-aligned box in view space: a hand, a pillar, a wall.
struct Box
{
    Vec3 low, high;

    [[nodiscard]] double Hit(const Vec3 aDir) const
    {
        double enter = 0.0;
        double leave = 1.0e30;
        const double o[3] = {0.0, 0.0, 0.0};
        const double d[3] = {aDir.x, aDir.y, aDir.z};
        const double lo[3] = {low.x, low.y, low.z};
        const double hi[3] = {high.x, high.y, high.z};
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::abs(d[axis]) < 1.0e-15)
            {
                if (o[axis] < lo[axis] || o[axis] > hi[axis])
                {
                    return -1.0;
                }
                continue;
            }
            double t0 = (lo[axis] - o[axis]) / d[axis];
            double t1 = (hi[axis] - o[axis]) / d[axis];
            if (t0 > t1)
            {
                std::swap(t0, t1);
            }
            enter = std::max(enter, t0);
            leave = std::min(leave, t1);
            if (enter > leave)
            {
                return -1.0;
            }
        }
        return enter > 0.0 ? enter : -1.0;
    }
};

enum class Precision
{
    Float32,
    Unorm24,
};

struct Scene
{
    std::string name;
    double fov = 60.0;
    Rectangle tv;
    std::vector<Box> occluders;
    bool propSurface = true;        // the prop's own screen mesh, in the depth buffer
    double propOffset = 0.005;      // how far in front of the picture it sits (m)
    SD::DepthModel model = SD::DepthModel::ReversedInfinite(0.05);
    Precision precision = Precision::Float32;
    double depthScale = 1.0;        // depth texels per overlay pixel (0.5 = upscaler at half)
    int textureScale = 1;           // the depth texture's size over the part drawn this frame
                                    // (2: drawn into its top-left quarter, dynamic resolution)
    SD::Tuning tuning{};
    bool useDepth = true;
};

struct Outcome
{
    int inside = 0;          // overlay pixels on the screen
    int shownTruth = 0;      // of those, where nothing is in front
    int shownShader = 0;     // where the shader shows the picture (visibility > 0.5)
    int mismatches = 0;
    int mismatchesAwayFromEdges = 0;
    int fadedAwayFromEdges = 0;     // shown, but not fully, with nothing in front anywhere near
    double worstUvTexels = 0.0;     // against a 1280 x 720 picture
    double worstInverseDepth = 0.0; // relative
};

// The depth buffer the game would have drawn: nearest of everything but the
// picture itself (which is an overlay, not geometry).
std::vector<float> RenderDepth(const Scene& aScene, const Camera& aCamera, int& aWidth, int& aHeight)
{
    aWidth = static_cast<int>(std::lround(aCamera.width * aScene.depthScale));
    aHeight = static_cast<int>(std::lround(aCamera.height * aScene.depthScale));
    std::vector<float> depth(static_cast<std::size_t>(aWidth) * static_cast<std::size_t>(aHeight));
    Rectangle prop = aScene.tv;
    prop.centre = prop.centre - (prop.Normal() * aScene.propOffset *
                                 (Dot(prop.Normal(), prop.centre) > 0.0 ? 1.0 : -1.0));
    prop.width += 0.06; // a bezel
    prop.height += 0.06;
    for (int j = 0; j < aHeight; ++j)
    {
        for (int i = 0; i < aWidth; ++i)
        {
            // The texel's centre, in overlay pixels.
            const double x = (i + 0.5) / aScene.depthScale;
            const double y = (j + 0.5) / aScene.depthScale;
            const Vec3 dir = aCamera.Ray(x, y);
            double nearest = 1.0e30;
            for (const Box& box : aScene.occluders)
            {
                const double t = box.Hit(dir);
                if (t > 0.0)
                {
                    nearest = std::min(nearest, t);
                }
            }
            if (aScene.propSurface)
            {
                double u = 0.0, v = 0.0;
                const double t = prop.Hit(dir, u, v);
                if (t > 0.0)
                {
                    nearest = std::min(nearest, t);
                }
            }
            double device = nearest >= 1.0e29 ? aScene.model.Device(1.0e30) : aScene.model.Device(nearest);
            device = std::clamp(device, 0.0, 1.0);
            if (aScene.precision == Precision::Unorm24)
            {
                const double scale = 16777215.0;
                device = std::round(device * scale) / scale;
            }
            depth[(static_cast<std::size_t>(j) * static_cast<std::size_t>(aWidth)) + static_cast<std::size_t>(i)] =
                static_cast<float>(device);
        }
    }
    return depth;
}

// HLSL's GatherRed at a point in texels: (0,1), (1,1), (1,0), (0,0) of the 2x2
// block whose top-left texel is floor(t - 0.5), with clamped addressing.
SD::Hlsl::float4 Gather(const std::vector<float>& aDepth, const int aWidth, const int aHeight,
                        const SD::Hlsl::float2 aTexel)
{
    const int i = static_cast<int>(std::floor(aTexel.x - 0.5F));
    const int j = static_cast<int>(std::floor(aTexel.y - 0.5F));
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, aWidth - 1);
        y = std::clamp(y, 0, aHeight - 1);
        return aDepth[(static_cast<std::size_t>(y) * static_cast<std::size_t>(aWidth)) + static_cast<std::size_t>(x)];
    };
    return SD::Hlsl::float4(at(i, j + 1), at(i + 1, j + 1), at(i + 1, j), at(i, j));
}

Outcome Run(const Scene& aScene)
{
    const Camera camera(aScene.fov);
    int depthWidth = 0, depthHeight = 0;
    const std::vector<float> depth = RenderDepth(aScene, camera, depthWidth, depthHeight);

    // What the overlay has: the projected corners, in texture order, and the
    // centre's view depth. Nothing else about the scene.
    float corners[8];
    for (int corner = 0; corner < 4; ++corner)
    {
        const double u = (corner == 1 || corner == 2) ? 1.0 : 0.0;
        const double v = (corner >= 2) ? 1.0 : 0.0;
        const auto p = camera.Project(aScene.tv.Corner(u, v));
        corners[corner * 2] = static_cast<float>(p[0]);
        corners[(corner * 2) + 1] = static_cast<float>(p[1]);
    }
    const float centreDepth = static_cast<float>(aScene.tv.centre.z);
    const float overlaySize[2] = {static_cast<float>(camera.width), static_cast<float>(camera.height)};
    const float depthSize[2] = {static_cast<float>(depthWidth), static_cast<float>(depthHeight)};

    // Dynamic resolution: this frame's depth in the top-left corner of a larger
    // texture, the rest of it left over from a frame at another resolution. Filled
    // here with something right in front of the camera, which would hide the
    // picture wherever it were read.
    int textureWidth = depthWidth, textureHeight = depthHeight;
    std::vector<float> texture = depth;
    if (aScene.textureScale > 1)
    {
        textureWidth = depthWidth * aScene.textureScale;
        textureHeight = depthHeight * aScene.textureScale;
        texture.assign(static_cast<std::size_t>(textureWidth) * static_cast<std::size_t>(textureHeight),
                       static_cast<float>(aScene.model.Device(0.06)));
        for (int j = 0; j < depthHeight; ++j)
        {
            std::copy_n(depth.begin() + (static_cast<std::ptrdiff_t>(j) * depthWidth), depthWidth,
                        texture.begin() + (static_cast<std::ptrdiff_t>(j) * textureWidth));
        }
    }
    const float textureSize[2] = {static_cast<float>(textureWidth), static_cast<float>(textureHeight)};
    SD::Constants constants;
    const SD::Status status = SD::Build(corners, centreDepth, aScene.model, overlaySize, depthSize, textureSize,
                                        aScene.tuning, aScene.useDepth, constants);
    CHECK(status == SD::Status::Ok);
    const SD::Shared::SdConstants c = SD::ToShared(constants);

    // Ground truth: is the picture, at this pixel, the nearest thing?
    const auto truthAt = [&](const int px, const int py, bool& aInside, double& aU, double& aV, double& aT) {
        const Vec3 dir = camera.Ray(px + 0.5, py + 0.5);
        aT = aScene.tv.Hit(dir, aU, aV);
        aInside = aT > 0.0;
        if (!aInside)
        {
            return false;
        }
        for (const Box& box : aScene.occluders)
        {
            const double t = box.Hit(dir);
            if (t > 0.0 && t < aT)
            {
                return false;
            }
        }
        return true;
    };

    Outcome out;
    std::vector<std::int8_t> truthMap(static_cast<std::size_t>(camera.width) * static_cast<std::size_t>(camera.height), -1);
    std::vector<std::array<int, 2>> mismatched;
    std::vector<std::array<int, 2>> faded;
    for (int py = 0; py < camera.height; ++py)
    {
        for (int px = 0; px < camera.width; ++px)
        {
            bool inside = false;
            double u = 0.0, v = 0.0, t = 0.0;
            const bool shown = truthAt(px, py, inside, u, v, t);
            const SD::Hlsl::float2 pixel(static_cast<float>(px) + 0.5F, static_cast<float>(py) + 0.5F);
            const SD::Hlsl::float2 uv = SD::Shared::SdScreenUv(c, pixel);
            const bool shaderInside = uv.x >= 0.0F && uv.x <= 1.0F && uv.y >= 0.0F && uv.y <= 1.0F;
            if (!inside)
            {
                continue;
            }
            // A pixel the ray puts on the screen, a hair from its edge, may round to
            // just outside in float; that is the edge of the triangles, not the test.
            if (!shaderInside)
            {
                CHECK(std::min(std::min(u, 1.0 - u), std::min(v, 1.0 - v)) < 1.0e-3);
                continue;
            }
            truthMap[(static_cast<std::size_t>(py) * static_cast<std::size_t>(camera.width)) + static_cast<std::size_t>(px)] =
                shown ? 1 : 0;
            ++out.inside;
            out.shownTruth += shown ? 1 : 0;
            out.worstUvTexels = std::max(out.worstUvTexels, std::abs(static_cast<double>(uv.x) - u) * 1280.0);
            out.worstUvTexels = std::max(out.worstUvTexels, std::abs(static_cast<double>(uv.y) - v) * 720.0);
            const float planeInverse = SD::Shared::SdPlaneInverseDepth(c, pixel);
            out.worstInverseDepth =
                std::max(out.worstInverseDepth, std::abs((static_cast<double>(planeInverse) * t) - 1.0));

            float visibility = 1.0F;
            if (c.look.z > 0.5F)
            {
                const SD::Hlsl::float2 texel = SD::Shared::SdDepthTexel(c, pixel);
                const SD::Hlsl::float4 devices = Gather(texture, textureWidth, textureHeight, texel);
                visibility = SD::Shared::SdVisibility4(c, planeInverse, devices, texel);
            }
            const bool shaderShown = visibility > 0.5F;
            out.shownShader += shaderShown ? 1 : 0;
            if (shown && visibility < 0.99F)
            {
                faded.push_back({px, py});
            }
            if (shaderShown != shown)
            {
                ++out.mismatches;
                mismatched.push_back({px, py});
            }
        }
    }
    // A disagreement is only allowed where the truth itself changes nearby: along
    // the edge of what is in front, within the depth buffer's own resolution. The
    // same for a pixel the shader fades: only next to something in front of it.
    const int radius = static_cast<int>(std::ceil(1.5 / aScene.depthScale)) + 1;
    const auto around = [&](const std::array<int, 2>& p, bool& aSawShown, bool& aSawHidden) {
        aSawShown = false;
        aSawHidden = false;
        for (int dy = -radius; dy <= radius; ++dy)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                const int x = p[0] + dx, y = p[1] + dy;
                if (x < 0 || y < 0 || x >= camera.width || y >= camera.height)
                {
                    continue;
                }
                const std::int8_t value =
                    truthMap[(static_cast<std::size_t>(y) * static_cast<std::size_t>(camera.width)) + static_cast<std::size_t>(x)];
                aSawShown = aSawShown || value == 1;
                aSawHidden = aSawHidden || value == 0;
            }
        }
    };
    for (const auto& p : mismatched)
    {
        bool sawShown = false, sawHidden = false;
        around(p, sawShown, sawHidden);
        if (!(sawShown && sawHidden))
        {
            ++out.mismatchesAwayFromEdges;
        }
    }
    for (const auto& p : faded)
    {
        bool sawShown = false, sawHidden = false;
        around(p, sawShown, sawHidden);
        if (!sawHidden)
        {
            ++out.fadedAwayFromEdges;
        }
    }
    return out;
}

void Report(const Scene& aScene, const Outcome& aOut)
{
    std::cout << "  " << aScene.name << ": " << aOut.inside << " px on the screen, " << aOut.shownTruth
              << " shown by the truth, " << aOut.shownShader << " by the shader, " << aOut.mismatches
              << " differ (" << aOut.mismatchesAwayFromEdges << " away from an edge), " << aOut.fadedAwayFromEdges
              << " faded with nothing near in front; uv within "
              << aOut.worstUvTexels << " texel, depth within " << aOut.worstInverseDepth << "\n";
}

// A hand at arm's length, low and to the right, over part of the screen.
Box Hand() { return Box{{0.02, -0.40, 0.45}, {0.30, -0.05, 0.60}}; }

// A pillar between the camera and the set, clearly in front of its nearest edge
// (a set turned 50 degrees at 1.5 m comes as near as 1.06 m).
Box Pillar() { return Box{{-0.30, -1.0, 0.80}, {-0.18, 1.0, 0.92}}; }
} // namespace

int main()
{
    // ------------------------------------------ the conventions, both ways round
    {
        const double near = 0.05, far = 2000.0;
        const SD::DepthModel models[3] = {SD::DepthModel::ReversedInfinite(near), SD::DepthModel::Reversed(near, far),
                                          SD::DepthModel::Conventional(near, far)};
        for (const SD::DepthModel& m : models)
        {
            for (const double z : {0.05, 0.5, 1.5, 20.0, 1500.0})
            {
                CHECK(std::abs((m.InverseDepth(m.Device(z)) * z) - 1.0) < 1.0e-9);
            }
        }
        CHECK(std::abs(models[0].Device(near) - 1.0) < 1.0e-12);
        CHECK(std::abs(models[1].Device(near) - 1.0) < 1.0e-12 && std::abs(models[1].Device(far)) < 1.0e-12);
        CHECK(std::abs(models[2].Device(near)) < 1.0e-12 && std::abs(models[2].Device(far) - 1.0) < 1.0e-12);
        SD::DepthModel fitted;
        CHECK(SD::DepthModel::FromSamples(0.7, models[1].Device(0.7), 40.0, models[1].Device(40.0), fitted));
        CHECK(std::abs(fitted.a - models[1].a) < 1.0e-9 && std::abs(fitted.b - models[1].b) < 1.0e-12);
        CHECK(!SD::DepthModel::FromSamples(1.0, 0.5, 1.0, 0.7, fitted));
    }

    // ------------------------------------------ the test itself, at its thresholds
    {
        SD::Constants k;
        const float corners[8] = {100, 100, 500, 100, 500, 400, 100, 400};
        const float size[2] = {1280, 720};
        SD::Tuning tuning;
        tuning.toleranceMetres = 0.03F;
        tuning.relativeTolerance = 0.0F;
        tuning.softnessMetres = 0.02F;
        CHECK(SD::Build(corners, 2.0F, SD::DepthModel::ReversedInfinite(0.05), size, size, tuning, true, k) ==
              SD::Status::Ok);
        const SD::Shared::SdConstants c = SD::ToShared(k);
        const float plane = 1.0F / 2.0F;
        const auto vis = [&](const float scene) { return SD::Shared::SdVisibility(c, plane, 1.0F / scene); };
        CHECK(vis(2.0F) == 1.0F);                          // the prop's own surface, level with it
        CHECK(vis(1.975F) == 1.0F);                        // inside the tolerance
        CHECK(std::abs(vis(1.96F) - 0.5F) < 1.0e-3F);      // half-way through the fade
        CHECK(vis(1.949F) == 0.0F);                        // clearly in front
        CHECK(vis(50.0F) == 1.0F);                         // behind
        CHECK(SD::Shared::SdVisibility(c, plane, 0.0F) == 1.0F); // the sky
        CHECK(SD::Shared::SdVisibility(c, plane, -0.1F) == 1.0F);

        // Gather is asked for at the corner its 2x2 block shares, never at a texel
        // centre, where a GPU's fixed-point rounding picks the block next door
        // (found running the pass on a real D3D12 device: a row showed through a hand).
        for (const float t : {90.5F, 90.49F, 90.51F, 91.0F, 0.5F, 0.2F})
        {
            const SD::Hlsl::float2 uv = SD::Shared::SdGatherUv(c, SD::Hlsl::float2(t, t));
            const float base = std::floor(t - 0.5F);
            CHECK(std::abs((uv.x * 1280.0F) - (base + 1.0F)) < 1.0e-3F);
            CHECK(std::abs((uv.y * 720.0F) - (base + 1.0F)) < 1.0e-3F);
        }
    }

    std::cout << "ScreenDepth scenes:\n";
    // ------------------------------------------ a hand over a set, head on
    {
        Scene s;
        s.name = "head-on set, hand in front, conventional 24-bit depth";
        s.tv = Television(1.16, 0.66, 1.5, 0.0);
        s.occluders = {Hand()};
        s.model = SD::DepthModel::Conventional(0.05, 1000.0);
        s.precision = Precision::Unorm24;
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.inside > 100000);
        CHECK(o.shownTruth < o.inside && o.shownTruth > o.inside / 2);   // the hand hides a part
        CHECK(o.mismatchesAwayFromEdges == 0);
        CHECK(o.mismatches < o.inside / 200);
        CHECK(o.worstUvTexels < 0.05);
        CHECK(o.worstInverseDepth < 2.0e-4);
    }
    // ------------------------------------------ turned, a hand and a pillar, reversed Z
    {
        Scene s;
        s.name = "set turned 50 degrees, hand and pillar, reversed infinite float depth";
        s.tv = Television(1.16, 0.66, 1.5, 50.0);
        s.occluders = {Hand(), Pillar()};
        s.model = SD::DepthModel::ReversedInfinite(0.05);
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.shownTruth < o.inside);
        CHECK(o.mismatchesAwayFromEdges == 0);
        CHECK(o.mismatches < o.inside / 100);
        CHECK(o.worstUvTexels < 0.05);
        CHECK(o.worstInverseDepth < 2.0e-4);
    }
    // ------------------------------------------ something that passes THROUGH the set
    {
        // A pillar that crosses the screen's plane: nearer than the screen on one
        // side of the crossing, behind it on the other. The shader shows the
        // picture wherever the pillar is less than the tolerance in front, so the
        // truth and the shader may only disagree inside that band.
        Scene s;
        s.name = "a pillar through the set's plane (the tolerance band)";
        s.tv = Television(1.16, 0.66, 1.5, 50.0);
        s.occluders = {Box{{-0.30, -1.0, 1.10}, {-0.18, 1.0, 1.25}}};
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.shownTruth < o.inside);
        // Every disagreement is the shader showing the picture where the pillar is
        // within tolerance plus softness of the plane, never hiding it wrongly.
        CHECK(o.shownShader >= o.shownTruth);
        CHECK(o.mismatches < o.inside / 20);
    }
    // ------------------------------------------ an upscaler: depth at half resolution
    {
        Scene s;
        s.name = "set turned 50 degrees, hand and pillar, depth at half resolution (an upscaler)";
        s.tv = Television(1.16, 0.66, 1.5, 50.0);
        s.occluders = {Hand(), Pillar()};
        s.model = SD::DepthModel::Reversed(0.05, 5000.0);
        s.depthScale = 0.5;
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.mismatchesAwayFromEdges == 0);
        CHECK(o.mismatches < o.inside / 50);
    }
    // ------------------------------------------ dynamic resolution: depth in a corner of its texture
    {
        // The set runs off the right and the bottom of the frame, so the last column
        // and row of pixels read the last the game drew, next to texels it did not
        // draw this frame. A hand covers part of it.
        Scene s;
        s.name = "set off the corner of the frame, depth drawn into a quarter of its texture";
        s.tv = Television(1.16, 0.66, 1.5, -50.0, 1.3, -0.3);
        s.occluders = {Hand()};
        s.depthScale = 0.5;
        s.textureScale = 2;
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.shownTruth < o.inside);
        CHECK(o.mismatchesAwayFromEdges == 0);
        CHECK(o.mismatches < o.inside / 50);
    }
    // ------------------------------------------ a cinema screen far away, a pillar
    {
        Scene s;
        s.name = "100 ft screen at 20 m turned 35 degrees, pillar, 24-bit reversed";
        s.tv = Television(30.48, 17.34, 20.4, 35.0);
        s.fov = 70.0;
        s.occluders = {Box{{-1.2, -30.0, 9.0}, {0.3, 30.0, 10.5}}};
        s.model = SD::DepthModel::Reversed(0.05, 5000.0);
        s.precision = Precision::Unorm24;
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.shownTruth < o.inside);
        CHECK(o.mismatchesAwayFromEdges == 0);
        CHECK(o.worstUvTexels < 0.1);
        CHECK(o.worstInverseDepth < 5.0e-4);
    }
    // ------------------------------------------ nothing in front: all of it shows
    {
        Scene s;
        s.name = "nothing in front, only the prop's own screen 5 mm proud";
        s.tv = Television(1.16, 0.66, 1.5, 30.0);
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.shownTruth == o.inside);
        CHECK(o.shownShader == o.inside);
    }
    // ------------------------------------------ ...and the tolerance is why
    {
        Scene s;
        s.name = "the same with no tolerance at all";
        s.tv = Television(1.16, 0.66, 1.5, 30.0);
        s.tuning.toleranceMetres = 0.0F;
        s.tuning.relativeTolerance = 0.0F;
        s.tuning.softnessMetres = 0.001F;
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == o.inside); // all of it hidden or faded by its own prop
        // The prop's own mesh would hide the picture it carries.
        CHECK(o.shownShader < o.inside / 10);
    }
    // ------------------------------------------ behind a wall: none of it shows
    {
        Scene s;
        s.name = "a wall between the camera and the set";
        s.tv = Television(1.16, 0.66, 3.0, 20.0);
        s.occluders = {Box{{-10.0, -10.0, 1.0}, {10.0, 10.0, 1.2}}};
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.shownTruth == 0);
        CHECK(o.shownShader == 0);
    }
    // ------------------------------------------ depth switched off: the picture only
    {
        Scene s;
        s.name = "depth test off (a game whose depth cannot be read)";
        s.tv = Television(1.16, 0.66, 1.5, 50.0);
        s.occluders = {Hand()};
        s.useDepth = false;
        const Outcome o = Run(s);
        Report(s, o);
        CHECK(o.fadedAwayFromEdges == 0);
        CHECK(o.shownShader == o.inside);
        CHECK(o.worstUvTexels < 0.05);
    }

    // ------------------------------------------ what is not a screen is refused
    {
        SD::Constants k;
        k.look[1] = 42.0F; // a refusal leaves the output alone
        const float size[2] = {1280, 720};
        const float corners[8] = {100, 100, 500, 100, 500, 400, 100, 400};
        const float line[8] = {100, 100, 200, 100, 300, 100, 400, 100};
        const SD::DepthModel m = SD::DepthModel::ReversedInfinite(0.05);
        CHECK(SD::Build(line, 2.0F, m, size, size, SD::Tuning{}, true, k) == SD::Status::BadQuad);
        CHECK(SD::Build(corners, 0.0F, m, size, size, SD::Tuning{}, true, k) == SD::Status::BadDepth);
        CHECK(SD::Build(corners, -1.0F, m, size, size, SD::Tuning{}, true, k) == SD::Status::BadDepth);
        CHECK(SD::Build(corners, std::nanf(""), m, size, size, SD::Tuning{}, true, k) == SD::Status::BadDepth);
        const float none[2] = {0, 720};
        CHECK(SD::Build(corners, 2.0F, m, none, size, SD::Tuning{}, true, k) == SD::Status::BadSize);
        CHECK(SD::Build(corners, 2.0F, m, size, none, SD::Tuning{}, true, k) == SD::Status::BadSize);
        const float quarter[2] = {640, 360}; // a texture smaller than what was drawn in it
        CHECK(SD::Build(corners, 2.0F, m, size, size, quarter, SD::Tuning{}, true, k) == SD::Status::BadSize);
        CHECK(SD::Build(corners, 2.0F, SD::DepthModel{0.0, 0.0}, size, size, SD::Tuning{}, true, k) ==
              SD::Status::BadDepth);
        CHECK(k.look[1] == 42.0F);
        CHECK(std::string(SD::Describe(SD::Status::BadQuad)) == "bad_quad");
    }

    // ------------------------------------------ the constants are the shader's registers
    {
        CHECK(sizeof(SD::Constants) == 128);
        CHECK(offsetof(SD::Constants, plane) == 48);
        CHECK(offsetof(SD::Constants, depth) == 64);
        CHECK(offsetof(SD::Constants, look) == 96);
        CHECK(offsetof(SD::Constants, extent) == 112);
    }

    std::cout << "ScreenDepth: conventions, thresholds, picture and depth exact, hidden exactly behind "
                 "what is in front, refusals OK\n";
    return 0;
}
