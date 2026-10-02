#pragma once

// The Direct3D 12 half of drawing a television in the game's depth.
//
// One pipeline (a root signature and a pipeline state) and one call per screen.
// Everything a screen needs travels as root constants -- its eight registers of
// ScreenDepth::Constants and four of Geometry -- so there is no upload heap, no
// constant buffer to manage and nothing to fence. Two descriptor tables carry
// the screen's picture and the game's depth, which the caller already has in the
// shader-visible heap it has bound (the overlay's own ImGui heap).
//
// What this does NOT do, because only the host that owns the frame can:
//
//   * find the game's depth buffer and make a copy of it a shader can read: that
//     is SceneDepth.cpp, which also binds the copy's view in the overlay's heap;
//   * bind the render target, viewport, scissor and descriptor heap. Called from
//     an ImGui draw callback, ImGui's DX12 backend and the host have done all four,
//     and `ImDrawCallback_ResetRenderState` puts ImGui's own pipeline back
//     afterwards.
//
// The D3D half is compiled for Windows only; the geometry the vertex shader reads
// is plain data, compiled everywhere so a test can draw its fan
// (tests/ScreenClipTests.cpp). Checked against a CPU reference pixel for pixel on
// Wine's Direct3D 12 (vkd3d over Mesa's software Vulkan) in the open77-tv
// repository, where SceneDepth was also run end to end; not on a GPU driver.

#include "ScreenDepth.hpp"

#include <string>

namespace op77::WorldOverlay::ScreenDepth
{
/// The part of the screen to shade and the overlay's size, as the vertex shader
/// reads them: up to six corners in order round a convex polygon, drawn as the fan
/// (0 1 2), (0 2 3), (0 3 4), (0 4 5). A whole screen is its four corners in
/// texture order -- the fan's first two triangles, the quad drawn before -- and a
/// screen cut at the camera's near plane (ScreenClip.hpp) is three to five. Corners
/// past the last repeat it, so the triangles they would make have no area.
struct alignas(16) Geometry
{
    float corners01[4]{}; // x0 y0 x1 y1
    float corners23[4]{}; // x2 y2 x3 y3
    float corners45[4]{}; // x4 y4 x5 y5
    float viewport[4]{};  // width, height, 1 / width, 1 / height
};
static_assert(sizeof(Geometry) == 4 * 16, "the geometry is four registers");

/// The vertices one draw submits: four triangles of the fan.
inline constexpr unsigned kGeometryVertices = 12;

/// `aCorners`: `aCount` (1 to 6) x, y pairs in overlay pixels, in order round a
/// convex polygon.
[[nodiscard]] inline Geometry MakePolygonGeometry(const float* const aCorners, const int aCount, const float aWidth,
                                                  const float aHeight)
{
    Geometry g;
    float points[12]{};
    const int count = aCount < 1 ? 1 : (aCount > 6 ? 6 : aCount);
    for (int i = 0; i < 6; ++i)
    {
        const int source = i < count ? i : count - 1;
        points[i * 2] = aCorners[source * 2];
        points[(i * 2) + 1] = aCorners[(source * 2) + 1];
    }
    for (int i = 0; i < 4; ++i)
    {
        g.corners01[i] = points[i];
        g.corners23[i] = points[4 + i];
        g.corners45[i] = points[8 + i];
    }
    g.viewport[0] = aWidth;
    g.viewport[1] = aHeight;
    g.viewport[2] = aWidth > 0.0F ? 1.0F / aWidth : 0.0F;
    g.viewport[3] = aHeight > 0.0F ? 1.0F / aHeight : 0.0F;
    return g;
}

/// A whole screen: its four corners in texture order.
[[nodiscard]] inline Geometry MakeGeometry(const float (&aCorners)[8], const float aWidth, const float aHeight)
{
    return MakePolygonGeometry(aCorners, 4, aWidth, aHeight);
}
} // namespace op77::WorldOverlay::ScreenDepth

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgiformat.h>

namespace op77::WorldOverlay::ScreenDepth
{
/// The format a shader reads a depth buffer through, for the buffer's own
/// (usually typeless) format. False for a format that is not a depth format.
[[nodiscard]] bool DepthShaderFormat(DXGI_FORMAT aResourceFormat, DXGI_FORMAT& aOut);

class Pass
{
public:
    Pass() = default;
    Pass(const Pass&) = delete;
    Pass& operator=(const Pass&) = delete;
    ~Pass();

    /// Builds the root signature and the pipeline for a render target format. The
    /// shaders are compiled at start-up with D3DCompile (d3dcompiler_47.dll, which
    /// every Windows has), from the source embedded in ScreenDepthShaderSource.hpp.
    [[nodiscard]] bool Create(ID3D12Device* aDevice, DXGI_FORMAT aRenderTargetFormat, std::string& aError);

    [[nodiscard]] bool Ready() const { return m_pipeline != nullptr; }
    [[nodiscard]] DXGI_FORMAT Format() const { return m_format; }

    /// Records one screen. The caller has bound the render target, viewport,
    /// scissor and the shader-visible heap holding both views, and has the depth
    /// buffer in PIXEL_SHADER_RESOURCE. When the constants say depth is off, the
    /// depth table is still bound (to the picture's view is fine) and not read.
    void Draw(ID3D12GraphicsCommandList* aList, const Constants& aConstants, const Geometry& aGeometry,
              D3D12_GPU_DESCRIPTOR_HANDLE aPicture, D3D12_GPU_DESCRIPTOR_HANDLE aDepth) const;

    void Release();

private:
    ID3D12RootSignature* m_rootSignature = nullptr;
    ID3D12PipelineState* m_pipeline = nullptr;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
};
} // namespace op77::WorldOverlay::ScreenDepth
#endif
