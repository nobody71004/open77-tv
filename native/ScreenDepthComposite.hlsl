// Draws one television's picture into the overlay: in perspective, and behind
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
