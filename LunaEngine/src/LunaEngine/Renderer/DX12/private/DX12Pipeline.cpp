#include "LunaPCH.h"
#include "Renderer/DX12/Public/DX12Backend.h"
#include "Renderer/DX12/Public/DX12Pipeline.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Renderer/HAL/Public/IRenderContext.h"
#include "Logger/Logger.h"

#include <dxcapi.h>
#include <fstream>
#include <vector>

namespace Luna
{

// Static DXC instances — created once on first use, shared across all pipelines
ComPtr<IDxcUtils>     DX12Pipeline::s_DxcUtils;
ComPtr<IDxcCompiler3> DX12Pipeline::s_DxcCompiler;

void DX12Pipeline::EnsureDXCInitialized()
{
    if (s_DxcUtils && s_DxcCompiler) return;

    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&s_DxcUtils));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DxcCreateInstance (IDxcUtils) failed: 0x%08lX", (unsigned long)hr);
        return;
    }

    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&s_DxcCompiler));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DxcCreateInstance (IDxcCompiler3) failed: 0x%08lX", (unsigned long)hr);
        return;
    }
}

bool DX12Pipeline::Initialize(const ComPtr<ID3D12Device> &device, const std::wstring &vsPath,
                              const std::wstring &psPath, const PipelineStateDesc &desc)
{
    _desc = desc;

    EnsureDXCInitialized();

    std::wstring vsFullPath = GetShaderFullPath(vsPath);
    std::wstring psFullPath = GetShaderFullPath(psPath);

    if (!LoadShaderDXC(vsFullPath, L"vs_6_0", _vsBlob)) return false;
    if (!LoadShaderDXC(psFullPath, L"ps_6_0", _psBlob)) return false;
    if (!CreateRootSignature(device))                    return false;
    if (!CreatePipelineState(device, _vsBlob, _psBlob))  return false;

    return true;
}

// ---------------------------------------------------------------------------
// Phase 2B: DXC compilation — DXIL output (SM 6.0+)
// ---------------------------------------------------------------------------
bool DX12Pipeline::LoadShaderDXC(const std::wstring &path, const std::wstring &target,
                                  ComPtr<IDxcBlob> &outBlob)
{
    if (!s_DxcUtils || !s_DxcCompiler)
    {
        LUNA_LOG_ERROR("DXC not initialized");
        return false;
    }

    // Load source file
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = s_DxcUtils->LoadFile(path.c_str(), nullptr, &sourceBlob);
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DXC: failed to load %ls: 0x%08lX", path.c_str(), (unsigned long)hr);
        return false;
    }

    DxcBuffer srcBuf = {};
    srcBuf.Ptr      = sourceBlob->GetBufferPointer();
    srcBuf.Size     = sourceBlob->GetBufferSize();
    srcBuf.Encoding = DXC_CP_ACP;

    LPCWSTR args[] = {
        path.c_str(),      // filename hint for error messages
        L"-E", L"main",    // entry point
        L"-T", target.c_str(), // target profile (vs_6_0 / ps_6_0)
        L"-HV", L"2021",   // HLSL 2021 semantics
#ifdef _DEBUG
        L"-Zs",            // embed debug info in debug builds
        L"-Od",            // disable optimizer in debug builds
#endif
    };

    ComPtr<IDxcResult> result;
    hr = s_DxcCompiler->Compile(
        &srcBuf,
        args, _countof(args),
        nullptr,            // no include handler (shaders are self-contained)
        IID_PPV_ARGS(&result));

    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("DXC: Compile() call failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    // Check for compile errors
    HRESULT compileStatus = S_OK;
    result->GetStatus(&compileStatus);

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    if (FAILED(compileStatus))
    {
        if (errors && errors->GetStringLength() > 0)
            LUNA_LOG_ERROR("Shader compile error in %ls:\n%s", path.c_str(), errors->GetStringPointer());
        else
            LUNA_LOG_ERROR("Shader compile failed (no error message): %ls", path.c_str());
        return false;
    }

    if (errors && errors->GetStringLength() > 0)
        LUNA_LOG_WARN("Shader warnings in %ls:\n%s", path.c_str(), errors->GetStringPointer());

    hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&outBlob), nullptr);
    if (FAILED(hr) || !outBlob)
    {
        LUNA_LOG_ERROR("DXC: failed to retrieve shader object for %ls", path.c_str());
        return false;
    }

    return true;
}

bool DX12Pipeline::CreateRootSignature(const ComPtr<ID3D12Device> &device)
{
    CD3DX12_ROOT_PARAMETER params[1];
    params[0].InitAsConstantBufferView(0); // b0 — MVP transform

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init(1, params, 0, nullptr,
                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &serializedBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
            LUNA_LOG_ERROR("Root signature serialize error:\n%s",
                           static_cast<const char *>(errorBlob->GetBufferPointer()));
        else
            LUNA_LOG_ERROR("Root signature serialize failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    hr = device->CreateRootSignature(0, serializedBlob->GetBufferPointer(),
                                     serializedBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&_rootSignature));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("CreateRootSignature failed: 0x%08lX", (unsigned long)hr);
        return false;
    }

    return true;
}

bool DX12Pipeline::CreatePipelineState(const ComPtr<ID3D12Device>& device,
                                       ComPtr<IDxcBlob> vs, ComPtr<IDxcBlob> ps)
{
    // TriangleVertex: { Vec3 position (12 B), Vec4 color (16 B) } — stride 28 B
    D3D12_INPUT_ELEMENT_DESC triangleLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    // PBRVertex: { pos(12) + normal(12) + uv(8) + tangent(16) } — stride 48 B
    D3D12_INPUT_ELEMENT_DESC pbrLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_INPUT_ELEMENT_DESC* inputLayout      = triangleLayout;
    UINT                      inputLayoutCount = _countof(triangleLayout);
    if (_desc.vertexLayout == VertexLayout::PBR)
    {
        inputLayout      = pbrLayout;
        inputLayoutCount = _countof(pbrLayout);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout        = {inputLayout, inputLayoutCount};
    psoDesc.pRootSignature     = _rootSignature.Get();
    // IDxcBlob satisfies GetBufferPointer/GetBufferSize — compatible with D3D12_SHADER_BYTECODE
    psoDesc.VS                 = {vs->GetBufferPointer(), vs->GetBufferSize()};
    psoDesc.PS                 = {ps->GetBufferPointer(), ps->GetBufferSize()};
    psoDesc.RasterizerState    = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState         = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState  = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask         = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets   = 1;
    psoDesc.RTVFormats[0]      = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count   = 1;
    psoDesc.NodeMask           = 0;
    psoDesc.Flags              = D3D12_PIPELINE_STATE_FLAG_NONE;

    if (_desc.enableWireFrame)
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;

    psoDesc.DepthStencilState.DepthEnable = _desc.enableDepthTest ? TRUE : FALSE;
    psoDesc.DSVFormat = _desc.enableDepthTest ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("CreateGraphicsPipelineState failed: 0x%08lX", (unsigned long)hr);
        return false;
    }
    return true;
}

} // namespace Luna
