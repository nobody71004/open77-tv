// See ScreenDepthPass.hpp. Windows only.

#include "ScreenDepthPass.hpp"

#if defined(_WIN32)
#include "ScreenDepthShaderSource.hpp"

#include <d3dcompiler.h>
#include <windows.h>

#include <cstring>

namespace op77::WorldOverlay::ScreenDepth
{
namespace
{
template <typename T>
void SafeRelease(T*& aPointer)
{
    if (aPointer != nullptr)
    {
        aPointer->Release();
        aPointer = nullptr;
    }
}

// Serves the one #include the composite shader has, from the embedded text.
class EmbeddedInclude final : public ID3DInclude
{
public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR aName, LPCVOID, LPCVOID* aData, UINT* aBytes) noexcept override
    {
        if (aName == nullptr || std::strcmp(aName, ShaderSource::kIncludeName) != 0)
        {
            return E_FAIL;
        }
        *aData = ShaderSource::kInclude;
        *aBytes = static_cast<UINT>(std::strlen(ShaderSource::kInclude));
        return S_OK;
    }

    HRESULT __stdcall Close(LPCVOID) noexcept override { return S_OK; }
};

using D3DCompileFunction = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                             LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

// d3dcompiler_47.dll is part of Windows; it is loaded when the pass is created
// rather than linked, so the host does not refuse to start without it.
D3DCompileFunction LoadCompiler(std::string& aError)
{
    HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
    if (module == nullptr)
    {
        aError = "d3dcompiler_47.dll is not available";
        return nullptr;
    }
    auto* function = reinterpret_cast<D3DCompileFunction>(reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile")));
    if (function == nullptr)
    {
        aError = "D3DCompile is missing from d3dcompiler_47.dll";
    }
    return function;
}

// D3D12SerializeRootSignature is taken from the d3d12.dll the game has loaded, as
// ImGui's own DX12 backend does, so the host need not link d3d12.lib for it. (The
// type is spelled out: MinGW's d3d12.h has no PFN_D3D12_SERIALIZE_ROOT_SIGNATURE.)
using SerialiseFunction = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                           ID3DBlob**);

SerialiseFunction LoadSerialiser(std::string& aError)
{
    HMODULE module = GetModuleHandleW(L"d3d12.dll");
    if (module == nullptr)
    {
        module = LoadLibraryW(L"d3d12.dll");
    }
    if (module == nullptr)
    {
        aError = "d3d12.dll is not loaded";
        return nullptr;
    }
    auto* function = reinterpret_cast<SerialiseFunction>(
        reinterpret_cast<void*>(GetProcAddress(module, "D3D12SerializeRootSignature")));
    if (function == nullptr)
    {
        aError = "D3D12SerializeRootSignature is missing from d3d12.dll";
    }
    return function;
}

ID3DBlob* Compile(D3DCompileFunction aCompile, const char* aEntry, const char* aTarget, std::string& aError)
{
    EmbeddedInclude include;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result = aCompile(ShaderSource::kComposite, std::strlen(ShaderSource::kComposite),
                                    "ScreenDepthComposite.hlsl", nullptr, &include, aEntry, aTarget,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code,
                                    &errors);
    if (FAILED(result))
    {
        aError = std::string(aEntry) + ": ";
        if (errors != nullptr)
        {
            aError.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        else
        {
            aError += "D3DCompile failed";
        }
        SafeRelease(errors);
        SafeRelease(code);
        return nullptr;
    }
    SafeRelease(errors);
    return code;
}
} // namespace

bool DepthShaderFormat(const DXGI_FORMAT aResourceFormat, DXGI_FORMAT& aOut)
{
    switch (aResourceFormat)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        aOut = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        aOut = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        aOut = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        aOut = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

Pass::~Pass()
{
    Release();
}

void Pass::Release()
{
    SafeRelease(m_pipeline);
    SafeRelease(m_rootSignature);
    m_format = DXGI_FORMAT_UNKNOWN;
}

bool Pass::Create(ID3D12Device* aDevice, const DXGI_FORMAT aRenderTargetFormat, std::string& aError)
{
    Release();
    if (aDevice == nullptr)
    {
        aError = "no device";
        return false;
    }
    const D3DCompileFunction compile = LoadCompiler(aError);
    const SerialiseFunction serialise = compile != nullptr ? LoadSerialiser(aError) : nullptr;
    if (compile == nullptr || serialise == nullptr)
    {
        return false;
    }

    // Root signature: the two constant blocks as root constants, one table per view,
    // two static samplers, no input assembler.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    for (UINT i = 0; i < 2; ++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        ranges[i].RegisterSpace = 0;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;
    }
    D3D12_ROOT_PARAMETER parameters[4] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister = 0;
    parameters[0].Constants.Num32BitValues = sizeof(Constants) / 4;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 1;
    parameters[1].Constants.Num32BitValues = sizeof(Geometry) / 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    for (UINT i = 0; i < 2; ++i)
    {
        parameters[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        parameters[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    for (UINT i = 0; i < 2; ++i)
    {
        samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[i].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].RegisterSpace = 0;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC description = {};
    description.NumParameters = 4;
    description.pParameters = parameters;
    description.NumStaticSamplers = 2;
    description.pStaticSamplers = samplers;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ID3DBlob* serialised = nullptr;
    ID3DBlob* errors = nullptr;
    if (FAILED(serialise(&description, D3D_ROOT_SIGNATURE_VERSION_1, &serialised, &errors)))
    {
        aError = "root signature: ";
        if (errors != nullptr)
        {
            aError.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        SafeRelease(errors);
        SafeRelease(serialised);
        return false;
    }
    SafeRelease(errors);
    const HRESULT made = aDevice->CreateRootSignature(0, serialised->GetBufferPointer(), serialised->GetBufferSize(),
                                                      IID_PPV_ARGS(&m_rootSignature));
    SafeRelease(serialised);
    if (FAILED(made))
    {
        aError = "CreateRootSignature failed";
        return false;
    }

    // Shader model 5.0: what Direct3D 12's own samples compile to, and what every
    // D3DCompile implementation accepts (Wine's does not take 5.1).
    ID3DBlob* vertex = Compile(compile, "VSMain", "vs_5_0", aError);
    if (vertex == nullptr)
    {
        Release();
        return false;
    }
    ID3DBlob* pixel = Compile(compile, "PSMain", "ps_5_0", aError);
    if (pixel == nullptr)
    {
        SafeRelease(vertex);
        Release();
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = m_rootSignature;
    pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    // Premultiplied alpha, as the overlay's other drawing and as CEF paints.
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pipeline.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // a mirrored quad is still a quad
    pipeline.RasterizerState.DepthClipEnable = FALSE;
    pipeline.DepthStencilState.DepthEnable = FALSE; // the test is in the shader, against the game's depth
    pipeline.DepthStencilState.StencilEnable = FALSE;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = aRenderTargetFormat;
    pipeline.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pipeline.SampleDesc.Count = 1;
    const HRESULT built = aDevice->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_pipeline));
    SafeRelease(vertex);
    SafeRelease(pixel);
    if (FAILED(built))
    {
        aError = "CreateGraphicsPipelineState failed";
        Release();
        return false;
    }
    m_format = aRenderTargetFormat;
    return true;
}

void Pass::Draw(ID3D12GraphicsCommandList* aList, const Constants& aConstants, const Geometry& aGeometry,
                const D3D12_GPU_DESCRIPTOR_HANDLE aPicture, const D3D12_GPU_DESCRIPTOR_HANDLE aDepth) const
{
    if (aList == nullptr || m_pipeline == nullptr)
    {
        return;
    }
    aList->SetGraphicsRootSignature(m_rootSignature);
    aList->SetPipelineState(m_pipeline);
    aList->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &aConstants, 0);
    aList->SetGraphicsRoot32BitConstants(1, sizeof(Geometry) / 4, &aGeometry, 0);
    aList->SetGraphicsRootDescriptorTable(2, aPicture);
    aList->SetGraphicsRootDescriptorTable(3, aDepth);
    aList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    aList->DrawInstanced(kGeometryVertices, 1, 0, 0);
}
} // namespace op77::WorldOverlay::ScreenDepth
#endif
