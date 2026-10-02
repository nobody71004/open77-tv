#pragma once

// The depth pass's shaders as text, for D3DCompile at start-up (ScreenDepthPass.cpp).
// Generated from ScreenDepthComposite.hlsl and ScreenDepth.hlsli beside it; edit
// those, then regenerate: Open77.ScreenDepthShaderSource.Tests fails while this
// file and those two disagree.

namespace op77::WorldOverlay::ScreenDepth::ShaderSource
{
inline constexpr const char kComposite[] = R"rbhlsl(// Draws one television's picture into the overlay: in perspective, and behind
// whatever in the game is in front of it.
//
// One draw per screen, twelve vertices, no vertex buffer: the corners of the part
// of the screen to shade come in the Geometry constants and SV_VertexID picks them,
// as a fan of up to four triangles -- a whole screen's four corners, or the three to
// five of a screen cut at the camera's near plane. The pixel shader works out the
// exact texture coordinate of every pixel from the screen's own map (so the triangle
// interpolation does not matter), and compares the screen's depth there with the
// game's depth buffer. The arithmetic is ScreenDepth.hlsli, which the C++ tests
// run too.
//
// Bindings (root signature in ScreenDepthPass.cpp):
//   b0  SdConstants   one screen's constants (ScreenDepth.hpp, Build)
//   b1  SdGeometry    the corners to shade and the overlay's size
//   t0  the screen's picture (CEF's premultiplied BGRA)
//   t1  the game's depth, as a single-channel float view
//   s0  linear, clamped (the picture)
//   s1  point, clamped (the depth; Gather ignores filtering but not addressing)

#include "ScreenDepth.hlsli"

struct SdGeometry
{
    float4 corners01;  // x0 y0 x1 y1, in overlay pixels: a whole screen's top-left, top-right
    float4 corners23;  // x2 y2 x3 y3: its bottom-right, bottom-left
    float4 corners45;  // x4 y4 x5 y5: a cut screen's fifth corner; unused ones repeat the last
    float4 viewport;   // xy: overlay size in pixels, zw: 1 / size
};

cbuffer ScreenConstants : register(b0)
{
    SdConstants g_screen;
};

cbuffer ScreenGeometry : register(b1)
{
    SdGeometry g_geometry;
};

Texture2D<float4> g_picture : register(t0);
Texture2D<float> g_depth : register(t1);
SamplerState g_linear : register(s0);
SamplerState g_point : register(s1);

struct VsOut
{
    float4 position : SV_Position;
};

VsOut VSMain(uint vertex : SV_VertexID)
{
    // The fan (0 1 2), (0 2 3), (0 3 4), (0 4 5). A whole screen is the first two,
    // over its corners in texture order; the corners a polygon does not have repeat
    // its last one, so the triangles past it have no area.
    uint fan = (vertex / 3u) % 4u;
    uint within = vertex % 3u;
    uint corner = within == 0u ? 0u : fan + within;
    float2 pixel;
    if (corner == 0) pixel = g_geometry.corners01.xy;
    else if (corner == 1) pixel = g_geometry.corners01.zw;
    else if (corner == 2) pixel = g_geometry.corners23.xy;
    else if (corner == 3) pixel = g_geometry.corners23.zw;
    else if (corner == 4) pixel = g_geometry.corners45.xy;
    else pixel = g_geometry.corners45.zw;

    VsOut output;
    float2 ndc = pixel * g_geometry.viewport.zw * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    output.position = float4(ndc, 0.0f, 1.0f);
    return output;
}

float4 PSMain(VsOut input) : SV_Target
{
    float2 pixel = input.position.xy;
    float2 uv = SdScreenUv(g_screen, pixel);
    // The triangles cover exactly the screen (or the part of it in front of the
    // camera); this only trims float rounding at an edge.
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
    {
        discard;
    }
    if (g_screen.look.w > 0.5f)
    {
        // The debug view: the game's depth itself over the quad, lighter where
        // nearer (white at the eye, mid grey at 2 m, black at the sky).
        int2 texel = int2(min(uv * g_screen.extent.xy, g_screen.extent.xy - 1.0f));
        float inverse = SdSceneInverseDepth(g_screen, g_depth.Load(int3(texel, 0)));
        float grey = inverse > 0.0f ? saturate(inverse / (inverse + 0.5f)) : 0.0f;
        return float4(grey, grey, grey, 1.0f) * g_screen.look.y;
    }
    float4 colour = g_picture.SampleLevel(g_linear, uv, 0.0f);

    float visibility = 1.0f;
    if (g_screen.look.z > 0.5f)
    {
        float2 texel = SdDepthTexel(g_screen, pixel);
        float4 devices = g_depth.GatherRed(g_point, SdGatherUv(g_screen, texel));
        visibility = SdVisibility4(g_screen, SdPlaneInverseDepth(g_screen, pixel), devices, texel);
    }
    if (visibility <= 0.0f)
    {
        discard;
    }
    // Premultiplied alpha in, premultiplied alpha out: scaling all four channels
    // fades the picture without changing its colour.
    return colour * (g_screen.look.y * visibility);
}
)rbhlsl";

inline constexpr const char kIncludeName[] = "ScreenDepth.hlsli";

inline constexpr const char kInclude[] = R"rbhlsl(// A television's picture, placed in the game's depth: the per-pixel arithmetic.
//
// This one file is compiled twice. As HLSL it is the pixel shader's arithmetic
// (ScreenDepthComposite.hlsl includes it). As C++ it is included by
// ScreenDepth.hpp, through a shim that supplies the few HLSL types and
// intrinsics used here, and tests/ScreenDepthTests.cpp checks it against an
// independent ray-traced scene. So the numbers the tests pin are the numbers the
// shader computes, not a transcription of them.
//
// Keep it to that shared subset: float2/float3/float4 (no swizzles: C++ has none),
// dot, saturate, min, max, floor, lerp, float literals with an f, and plain
// functions over the constants struct.
//
// Everything is in the overlay's own pixels (the presented image's, the space the
// producer's four corners are in and the space SV_Position is in).
//
//   * The picture: the four corners fix a homography H from texture space to
//     pixels. Its inverse, applied per pixel, gives the exact texture coordinate:
//     no affine warp, whatever the angle.
//   * The screen's depth: for a plane, inverse view depth is an affine function of
//     the pixel. It is H's inverse's third row over the view depth of corner 0,
//     which the producer's centre depth gives (C++ side, ScreenDepth.hpp).
//   * The game's depth: a device value d is affine in inverse view depth for any
//     perspective projection, d = A + B / z, whichever way round the game runs it.
//     So 1/z = (d - A) / B; only A and B, found once per game, are needed.
//   * The test: the screen is hidden where the scene is nearer than the screen by
//     more than a tolerance (the prop's own screen mesh sits in the depth buffer a
//     few millimetres from the picture's plane, and must not hide it), faded over a
//     softness band so an edge does not crawl.

#ifndef OP77_SCREEN_DEPTH_HLSLI
#define OP77_SCREEN_DEPTH_HLSLI

#ifdef __cplusplus
#define SD_FN inline
#else
#define SD_FN
#endif

// One screen's constants. Eight float4s: the same bytes as a constant buffer of
// eight registers, so the C++ struct is uploaded as it stands.
struct SdConstants
{
    float4 inverse0;   // xyz: rows of H's inverse, for (x, y, 1) in pixels
    float4 inverse1;
    float4 inverse2;
    float4 plane;      // xyz: the screen's inverse view depth over pixels; w: unused
    float4 depth;      // x: A, y: 1 / B (device = A + B / z), z: tolerance (m), w: relative tolerance
    float4 sampling;   // xy: depth texels per overlay pixel, zw: depth texture size in texels
    float4 look;       // x: softness (m), y: picture opacity, z: 1 when depth is used,
                       // w: 1 to show the depth itself instead of the picture (debug view)
    float4 extent;     // xy: the part of the depth texture the game drew, in texels from its
                       // top-left corner (all of it, unless the game renders into a corner
                       // of a larger texture, as dynamic resolution does); zw: unused
};

SD_FN float3 SdXyz(float4 v)
{
    return float3(v.x, v.y, v.z);
}

// The texture coordinate of a pixel on the screen; outside 0..1 is off the screen.
SD_FN float2 SdScreenUv(SdConstants c, float2 pixel)
{
    float3 p = float3(pixel.x, pixel.y, 1.0f);
    float w = dot(SdXyz(c.inverse2), p);
    return float2(dot(SdXyz(c.inverse0), p) / w, dot(SdXyz(c.inverse1), p) / w);
}

// 1 / (the screen's view depth) at a pixel.
SD_FN float SdPlaneInverseDepth(SdConstants c, float2 pixel)
{
    return dot(SdXyz(c.plane), float3(pixel.x, pixel.y, 1.0f));
}

// 1 / (the scene's view depth) from a device depth value. Zero or less is as far
// away as can be (the sky, or nothing drawn).
SD_FN float SdSceneInverseDepth(SdConstants c, float device)
{
    return (device - c.depth.x) * c.depth.y;
}

// How much of the screen shows at a pixel, given the scene's inverse depth there:
// 1 where nothing is in front of it, 0 where something is, a ramp across the
// softness band between.
SD_FN float SdVisibility(SdConstants c, float planeInverse, float sceneInverse)
{
    if (planeInverse <= 0.0f || sceneInverse <= 0.0f)
    {
        return 1.0f;
    }
    float planeDepth = 1.0f / planeInverse;
    float sceneDepth = 1.0f / sceneInverse;
    float tolerance = max(c.depth.z, c.depth.w * planeDepth);
    float softness = max(c.look.x, 1.0e-4f);
    // Visible when the scene is at least (plane - tolerance) away; hidden from
    // (plane - tolerance - softness) nearer.
    return saturate((sceneDepth - (planeDepth - tolerance - softness)) / softness);
}

// Where, in depth-texture texels, a pixel of the overlay falls. Kept half a texel
// inside the drawn part, so the four texels weighted around it never include one
// beyond it with any weight: past the drawn part is whatever an earlier frame, at
// another resolution, left there.
SD_FN float2 SdDepthTexel(SdConstants c, float2 pixel)
{
    return float2(max(0.5f, min(pixel.x * c.sampling.x, c.extent.x - 0.5f)),
                  max(0.5f, min(pixel.y * c.sampling.y, c.extent.y - 0.5f)));
}

// Where to Gather so the hardware returns exactly the 2x2 block this file weights:
// the corner the four texels share. Gathering at the point itself is ambiguous
// when it sits on a texel centre, and a GPU's fixed-point rounding then picks the
// neighbouring block (measured: a whole row showed through a hand).
SD_FN float2 SdGatherUv(SdConstants c, float2 texel)
{
    float2 base = floor(texel - float2(0.5f, 0.5f));
    return float2((base.x + 1.0f) / c.sampling.z, (base.y + 1.0f) / c.sampling.w);
}

// Visibility from the four depth texels around a point, weighted as a bilinear
// filter would weight them. `devices` is in HLSL's Gather order: (0,1), (1,1),
// (1,0), (0,0) relative to the top-left texel of the 2x2 block.
SD_FN float SdVisibility4(SdConstants c, float planeInverse, float4 devices, float2 texel)
{
    float2 f = texel - float2(0.5f, 0.5f);
    f = f - floor(f);
    float v00 = SdVisibility(c, planeInverse, SdSceneInverseDepth(c, devices.w));
    float v10 = SdVisibility(c, planeInverse, SdSceneInverseDepth(c, devices.z));
    float v01 = SdVisibility(c, planeInverse, SdSceneInverseDepth(c, devices.x));
    float v11 = SdVisibility(c, planeInverse, SdSceneInverseDepth(c, devices.y));
    float top = lerp(v00, v10, f.x);
    float bottom = lerp(v01, v11, f.x);
    return lerp(top, bottom, f.y);
}

#endif
)rbhlsl";
} // namespace op77::WorldOverlay::ScreenDepth::ShaderSource
