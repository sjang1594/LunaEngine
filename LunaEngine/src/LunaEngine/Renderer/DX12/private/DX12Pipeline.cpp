#include "LunaPCH.h"
#include "Renderer/DX12/Public/DX12Backend.h"
#include "Renderer/DX12/Public/DX12Pipeline.h"
#include "LunaEngine/Utils/FileSystemUtil.h"
#include "Renderer/HAL/Public/IRenderContext.h"
#include "Logger/Logger.h"

namespace Luna
{
bool DX12Pipeline::Initialize(const ComPtr<ID3D12Device> &device, const std::wstring &vsPath,
                              const std::wstring &psPath, const PipelineStateDesc &desc)
{
    _desc = desc;

    std::wstring vsFullPath = GetShaderFullPath(vsPath);
    std::wstring psFullPath = GetShaderFullPath(psPath);

    if (!LoadShader(vsFullPath, "vs_5_0", _vsBlob))   return false;
    if (!LoadShader(psFullPath, "ps_5_0", _psBlob))   return false;
    if (!CreateRootSignature(device))                  return false;
    if (!CreatePipelineState(device, _vsBlob, _psBlob)) return false;

    return true;
}

bool DX12Pipeline::LoadShader(const std::wstring &path, const std::string &target,
                              ComPtr<ID3DBlob> &blob)
{
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    "main", target.c_str(),
                                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob)
        {
            LUNA_LOG_ERROR("Shader compile error in %ls:\n%s",
                           path.c_str(),
                           static_cast<const char *>(errorBlob->GetBufferPointer()));
        }
        else
        {
            LUNA_LOG_ERROR("Shader compile failed: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
        }
        return false;
    }
    return true;
}

bool DX12Pipeline::CreateRootSignature(const ComPtr<ID3D12Device> &device)
{
    CD3DX12_ROOT_PARAMETER params[2];
    params[0].InitAsConstantBufferView(0); // b0
    params[1].InitAsConstantBufferView(1); // b1

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init(2, params, 0, nullptr,
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
            LUNA_LOG_ERROR("Root signature serialize failed: HRESULT 0x%08lX",
                           static_cast<unsigned long>(hr));
        return false;
    }

    hr = device->CreateRootSignature(0, serializedBlob->GetBufferPointer(),
                                     serializedBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&_rootSignature));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("CreateRootSignature failed: HRESULT 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    return true;
}

bool DX12Pipeline::CreatePipelineState(ComPtr<ID3D12Device> device, ComPtr<ID3DBlob> vs,
                                       ComPtr<ID3DBlob> ps)
{
    // Matches: struct Vertex { Vec3 position (12 B), Vec4 color (16 B) }
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout        = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature     = _rootSignature.Get();
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
        LUNA_LOG_ERROR("CreateGraphicsPipelineState failed: HRESULT 0x%08lX",
                       static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

} // namespace Luna
