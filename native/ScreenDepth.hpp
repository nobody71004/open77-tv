#pragma once

// A television's picture, placed in the game's depth: the CPU half.
//
// The pixel shader (ScreenDepthComposite.hlsl) draws a screen's quad with
// two things the overlay's ImGui path cannot do: an exact per-pixel texture
// coordinate, and a depth test against the game's own depth buffer. It needs one
// constant buffer per screen, and this file fills it from what the producer
// already has:
//
//   * the four projected corners (the same eight floats `DrawSurfaceQuad` gets);
//   * the view depth of the screen's centre, which `MediaScreens.cpp` computes for
//     its behind-the-camera gate (`depth = Dot(relative, forward)`) and already
//     publishes on the overlay item (`Item::depth`);
//   * the game's depth convention, as two numbers A and B with device = A + B / z
//     (`DepthModel`): its direction from the value the game clears the buffer
//     to, B measured from the screens themselves (SceneDepthPolicy::Calibration);
//   * the size of the depth the game drew relative to the picture (an upscaler
//     renders depth smaller), and of the texture it drew it in.
//
// The arithmetic the shader runs per pixel is ScreenDepth.hlsli, included
// here as C++ through the shim below, so the tests exercise the shader's own
// code. Pure and free of the engine, D3D and ImGui.

#include "ScreenClip.hpp"
#include "ScreenTessellation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace op77::WorldOverlay::ScreenDepth
{
// -- the HLSL subset ScreenDepth.hlsli uses, as C++ -------------------------------
namespace Hlsl
{
struct float2
{
    float x{}, y{};
    float2() = default;
    constexpr float2(const float aX, const float aY) : x(aX), y(aY) {}
};
struct float3
{
    float x{}, y{}, z{};
    float3() = default;
    constexpr float3(const float aX, const float aY, const float aZ) : x(aX), y(aY), z(aZ) {}
};
struct float4
{
    float x{}, y{}, z{}, w{};
    float4() = default;
    constexpr float4(const float aX, const float aY, const float aZ, const float aW)
        : x(aX), y(aY), z(aZ), w(aW) {}
};

inline float2 operator-(const float2 aA, const float2 aB) { return float2(aA.x - aB.x, aA.y - aB.y); }
inline float dot(const float3 aA, const float3 aB) { return (aA.x * aB.x) + (aA.y * aB.y) + (aA.z * aB.z); }
inline float saturate(const float aV) { return std::min(1.0F, std::max(0.0F, aV)); }
inline float lerp(const float aA, const float aB, const float aT) { return aA + ((aB - aA) * aT); }
inline float2 floor(const float2 aV) { return float2(std::floor(aV.x), std::floor(aV.y)); }
using std::max;
using std::min;
} // namespace Hlsl

// The shader's own source, compiled as C++.
namespace Shared
{
using namespace Hlsl;
#include "ScreenDepth.hlsli"
} // namespace Shared

/// The constant buffer, as the GPU sees it: eight float4 registers.
struct alignas(16) Constants
{
    float inverse0[4]{};
    float inverse1[4]{};
    float inverse2[4]{};
    float plane[4]{};
    float depth[4]{};
    float sampling[4]{};
    float look[4]{};
    float extent[4]{};
};
static_assert(sizeof(Constants) == 8 * 16, "one screen's constants are eight registers");

/// A projection's depth convention: device depth = a + b / (view depth).
///
/// Every perspective projection has this form, which is why two numbers are the
/// whole of what the shader needs to know about the game's.
struct DepthModel
{
    double a{0.0};
    double b{0.05};

    /// Reversed-Z with an infinite far plane: 1 at the near plane, 0 at infinity.
    [[nodiscard]] static constexpr DepthModel ReversedInfinite(const double aNear) { return {0.0, aNear}; }

    /// Reversed-Z with a far plane: 1 at the near plane, 0 at the far plane.
    [[nodiscard]] static constexpr DepthModel Reversed(const double aNear, const double aFar)
    {
        return {-aNear / (aFar - aNear), (aNear * aFar) / (aFar - aNear)};
    }

    /// The conventional way round: 0 at the near plane, 1 at the far plane.
    [[nodiscard]] static constexpr DepthModel Conventional(const double aNear, const double aFar)
    {
        return {aFar / (aFar - aNear), -(aNear * aFar) / (aFar - aNear)};
    }

    /// From two points whose view depth and device depth are both known, e.g. read
    /// from a capture. False when they cannot determine a model.
    [[nodiscard]] static bool FromSamples(const double aDepth1, const double aDevice1, const double aDepth2,
                                          const double aDevice2, DepthModel& aOut)
    {
        const double inverse1 = 1.0 / aDepth1;
        const double inverse2 = 1.0 / aDepth2;
        if (!std::isfinite(inverse1) || !std::isfinite(inverse2) || std::abs(inverse1 - inverse2) < 1.0e-12)
        {
            return false;
        }
        const double b = (aDevice1 - aDevice2) / (inverse1 - inverse2);
        if (!std::isfinite(b) || b == 0.0)
        {
            return false;
        }
        aOut = {aDevice1 - (b * inverse1), b};
        return true;
    }

    [[nodiscard]] double Device(const double aViewDepth) const { return a + (b / aViewDepth); }
    [[nodiscard]] double InverseDepth(const double aDevice) const { return (aDevice - a) / b; }
};

/// How the test is judged. In metres, the game's world unit.
struct Tuning
{
    /// The scene may be this much nearer than the screen and still not hide it:
    /// the prop's own screen mesh is in the depth buffer, a few millimetres off
    /// the picture's plane, and must not hide the picture it carries.
    float toleranceMetres = 0.03F;
    /// ...or this fraction of the distance, whichever is more: depth precision
    /// falls off with distance, and a cinema screen 20 m away needs more room.
    float relativeTolerance = 0.01F;
    /// The width of the fade between shown and hidden, so a hand's edge does not
    /// crawl as depth changes by a texel from frame to frame.
    float softnessMetres = 0.02F;
    /// The picture's opacity, as the ImGui path's alpha.
    float opacity = 1.0F;
};

enum class Status : std::uint8_t
{
    Ok,
    /// The corners are not a screen seen through a camera (see ScreenTessellation).
    BadQuad,
    /// The centre's view depth is not a positive, finite number.
    BadDepth,
    /// A size is zero or not finite.
    BadSize,
};

[[nodiscard]] inline const char* Describe(const Status aStatus)
{
    switch (aStatus)
    {
    case Status::Ok: return "ok";
    case Status::BadQuad: return "bad_quad";
    case Status::BadDepth: return "bad_depth";
    case Status::BadSize: return "bad_size";
    }
    return "unknown";
}

/// The sizes and the depth model, as every builder below checks them.
[[nodiscard]] inline Status CheckSizes(const DepthModel& aModel, const float aOverlaySize[2],
                                       const float aDepthSize[2], const float aDepthTextureSize[2])
{
    for (int i = 0; i < 2; ++i)
    {
        if (!std::isfinite(aOverlaySize[i]) || !(aOverlaySize[i] > 0.0F) || !std::isfinite(aDepthSize[i]) ||
            !(aDepthSize[i] >= 1.0F) || !std::isfinite(aDepthTextureSize[i]) ||
            !(aDepthTextureSize[i] >= aDepthSize[i]))
        {
            return Status::BadSize;
        }
    }
    if (!std::isfinite(aModel.a) || !std::isfinite(aModel.b) || aModel.b == 0.0)
    {
        return Status::BadDepth;
    }
    return Status::Ok;
}

/// Everything but the screen's own rows: the depth model, the tuning, the sampling.
inline void FillShared(const DepthModel& aModel, const float aOverlaySize[2], const float aDepthSize[2],
                       const float aDepthTextureSize[2], const Tuning& aTuning, const bool aUseDepth,
                       Constants& aOut)
{
    aOut.depth[0] = static_cast<float>(aModel.a);
    aOut.depth[1] = static_cast<float>(1.0 / aModel.b);
    aOut.depth[2] = std::max(0.0F, aTuning.toleranceMetres);
    aOut.depth[3] = std::max(0.0F, aTuning.relativeTolerance);
    aOut.sampling[0] = aDepthSize[0] / aOverlaySize[0];
    aOut.sampling[1] = aDepthSize[1] / aOverlaySize[1];
    aOut.sampling[2] = aDepthTextureSize[0];
    aOut.sampling[3] = aDepthTextureSize[1];
    aOut.look[0] = std::max(1.0e-4F, aTuning.softnessMetres);
    aOut.look[1] = std::clamp(aTuning.opacity, 0.0F, 1.0F);
    aOut.look[2] = aUseDepth ? 1.0F : 0.0F;
    aOut.extent[0] = aDepthSize[0];
    aOut.extent[1] = aDepthSize[1];
}

/// Fills one screen's constants. `aCorners` in texture order (top-left, top-right,
/// bottom-right, bottom-left) in overlay pixels; `aCentreDepth` the view depth of
/// the screen's centre; `aOverlaySize` the presented image's size in pixels;
/// `aDepthSize` the size, in texels, of the depth the game drew this frame (smaller
/// than the picture under an upscaler); `aDepthTextureSize` the size of the texture
/// holding it, when the game draws into the top-left corner of a larger one
/// (dynamic resolution, or an upscaler's input allocated at the output size).
/// `aUseDepth` false fills everything but switches the test off (perspective-
/// correct picture, no occlusion), for a game whose depth cannot be read.
/// A refusal leaves `aOut` untouched.
[[nodiscard]] inline Status Build(const float (&aCorners)[8], const float aCentreDepth,
                                  const DepthModel& aModel, const float aOverlaySize[2],
                                  const float aDepthSize[2], const float aDepthTextureSize[2],
                                  const Tuning& aTuning, const bool aUseDepth, Constants& aOut)
{
    ScreenTessellation::Homography map;
    if (ScreenTessellation::Solve(aCorners, map) != ScreenTessellation::Status::Ok)
    {
        return Status::BadQuad;
    }
    if (!std::isfinite(aCentreDepth) || !(aCentreDepth > 0.0F))
    {
        return Status::BadDepth;
    }
    if (const Status sizes = CheckSizes(aModel, aOverlaySize, aDepthSize, aDepthTextureSize); sizes != Status::Ok)
    {
        return sizes;
    }

    // H maps (u, v, 1) to (x w, y w, w): rows (a b c), (d e f), (g h 1).
    const double h[3][3] = {{map.a, map.b, map.c}, {map.d, map.e, map.f}, {map.g, map.h, 1.0}};
    const double det = (h[0][0] * ((h[1][1] * h[2][2]) - (h[1][2] * h[2][1]))) -
                       (h[0][1] * ((h[1][0] * h[2][2]) - (h[1][2] * h[2][0]))) +
                       (h[0][2] * ((h[1][0] * h[2][1]) - (h[1][1] * h[2][0])));
    if (!std::isfinite(det) || std::abs(det) < 1.0e-12)
    {
        return Status::BadQuad;
    }
    double inverse[3][3];
    inverse[0][0] = ((h[1][1] * h[2][2]) - (h[1][2] * h[2][1])) / det;
    inverse[0][1] = ((h[0][2] * h[2][1]) - (h[0][1] * h[2][2])) / det;
    inverse[0][2] = ((h[0][1] * h[1][2]) - (h[0][2] * h[1][1])) / det;
    inverse[1][0] = ((h[1][2] * h[2][0]) - (h[1][0] * h[2][2])) / det;
    inverse[1][1] = ((h[0][0] * h[2][2]) - (h[0][2] * h[2][0])) / det;
    inverse[1][2] = ((h[0][2] * h[1][0]) - (h[0][0] * h[1][2])) / det;
    inverse[2][0] = ((h[1][0] * h[2][1]) - (h[1][1] * h[2][0])) / det;
    inverse[2][1] = ((h[0][1] * h[2][0]) - (h[0][0] * h[2][1])) / det;
    inverse[2][2] = ((h[0][0] * h[1][1]) - (h[0][1] * h[1][0])) / det;

    // The third row of the exact inverse is 1 / W at a pixel, and W is view depth
    // over corner 0's; corner 0's view depth follows from the centre's.
    const double centreW = map.Denominator(0.5, 0.5);
    const double cornerDepth = static_cast<double>(aCentreDepth) / centreW;
    if (!std::isfinite(cornerDepth) || !(cornerDepth > 0.0))
    {
        return Status::BadDepth;
    }

    Constants out;
    for (int column = 0; column < 3; ++column)
    {
        out.inverse0[column] = static_cast<float>(inverse[0][column]);
        out.inverse1[column] = static_cast<float>(inverse[1][column]);
        out.inverse2[column] = static_cast<float>(inverse[2][column]);
        out.plane[column] = static_cast<float>(inverse[2][column] / cornerDepth);
    }
    FillShared(aModel, aOverlaySize, aDepthSize, aDepthTextureSize, aTuning, aUseDepth, out);
    aOut = out;
    return Status::Ok;
}

/// The same constants for a screen with part of it behind the camera
/// (ScreenClip.hpp). There are no four corners to start from -- some of them have
/// no projection -- so the producer hands over the plane's own map instead: texture
/// space to overlay pixels, its third row the view depth in metres. Its inverse is
/// the shader's three rows as they stand, and that inverse's third row is already
/// 1 / view depth, the `plane` register. The pass draws only the part of the screen
/// in front of the camera (`MakePolygonGeometry`), where every one of these is
/// finite and positive.
[[nodiscard]] inline Status BuildFromMap(const ScreenClip::Map& aMap, const DepthModel& aModel,
                                         const float aOverlaySize[2], const float aDepthSize[2],
                                         const float aDepthTextureSize[2], const Tuning& aTuning,
                                         const bool aUseDepth, Constants& aOut)
{
    double inverse[3][3];
    if (!ScreenClip::Invert(aMap, inverse))
    {
        return Status::BadQuad;
    }
    if (const Status sizes = CheckSizes(aModel, aOverlaySize, aDepthSize, aDepthTextureSize); sizes != Status::Ok)
    {
        return sizes;
    }
    Constants out;
    for (int column = 0; column < 3; ++column)
    {
        out.inverse0[column] = static_cast<float>(inverse[0][column]);
        out.inverse1[column] = static_cast<float>(inverse[1][column]);
        out.inverse2[column] = static_cast<float>(inverse[2][column]);
        out.plane[column] = static_cast<float>(inverse[2][column]);
    }
    FillShared(aModel, aOverlaySize, aDepthSize, aDepthTextureSize, aTuning, aUseDepth, out);
    aOut = out;
    return Status::Ok;
}

/// The same, for a game that draws depth over the whole of its depth texture.
[[nodiscard]] inline Status Build(const float (&aCorners)[8], const float aCentreDepth,
                                  const DepthModel& aModel, const float aOverlaySize[2],
                                  const float aDepthSize[2], const Tuning& aTuning, const bool aUseDepth,
                                  Constants& aOut)
{
    return Build(aCorners, aCentreDepth, aModel, aOverlaySize, aDepthSize, aDepthSize, aTuning, aUseDepth, aOut);
}

// -- the shader's own arithmetic, for the tests and for a CPU fallback -------------

[[nodiscard]] inline Shared::SdConstants ToShared(const Constants& aC)
{
    const auto vec = [](const float (&aV)[4]) { return Hlsl::float4(aV[0], aV[1], aV[2], aV[3]); };
    Shared::SdConstants s;
    s.inverse0 = vec(aC.inverse0);
    s.inverse1 = vec(aC.inverse1);
    s.inverse2 = vec(aC.inverse2);
    s.plane = vec(aC.plane);
    s.depth = vec(aC.depth);
    s.sampling = vec(aC.sampling);
    s.look = vec(aC.look);
    s.extent = vec(aC.extent);
    return s;
}
} // namespace op77::WorldOverlay::ScreenDepth
