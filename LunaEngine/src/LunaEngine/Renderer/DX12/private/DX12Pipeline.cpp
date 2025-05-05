#include "LunaPCH.h"
#include "Renderer/DX12/public/DX12Backend.h"
#include "Renderer/DX12/public/DX12Pipeline.h"
#include "LunaEngine/utils/FileSystemUtil.h"

#include "Renderer/IRenderContext.h"

namespace Luna
{
bool DX12Pipeline::Initialize(const ComPtr<ID3D12Device> &device, const std::wstring &vsPath,
                              const std::wstring &psPath, const PipelineStateDesc &desc)
{
    _desc = desc;
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    
    std::wstring vsFullPath = GetShaderFullPath(vsPath);
    std::wstring psFullPath = GetShaderFullPath(psPath);
    
    if (!LoadShader(vsFullPath, "vs_5_0", vsBlob))
        return false;
    if (!LoadShader(psFullPath, "ps_5_0", psBlob))
        return false;
    
    if (!CreateRootSignature(device))
    {
        cout << "Failed to create root signature" << endl;
        return false;
    }
    if (!CreatePipelineState(device, vsBlob, psBlob))
    {
        cout << "Failed to create pipeline state" << endl;
        return false;
    }
    return true;
}

bool DX12Pipeline::LoadShader(const std::wstring &path, const std::string &target,
                              ComPtr<ID3DBlob> &blob)
{
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr =
        D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
                           target.c_str(), D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::cerr << "[Shader Compiler] Error in " << std::string(path.begin(), path.end())
                      << ":\n"
                      << static_cast<const char *>(errorBlob->GetBufferPointer()) << std::endl;
        }
        return false;
    }
    return true;
}

bool DX12Pipeline::CreateRootSignature(const ComPtr<ID3D12Device> &device)
{
    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(0, nullptr, 0, nullptr,
                           D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &serializedRootSignature, &errorBlob);

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            std::cerr << "[Shader Compiler] Error in "
                      << static_cast<const char *>(errorBlob->GetBufferPointer()) << std::endl;
        }
        else
        {
            std::cerr << "[RootSignature] Serialize Error: HRESULT 0x" << std::hex << hr << std::endl;
        }
        return false;
    }

    hr = device->CreateRootSignature(0, serializedRootSignature->GetBufferPointer(),
                                     serializedRootSignature->GetBufferSize(),
                                     IID_PPV_ARGS(&_rootSignature));
    if (FAILED(hr))
    {
        std::cerr << "[RootSignature] Failed to create root signature. HRESULT 0x" << std::hex << hr << std::endl;
        return false;
    }

    return true;
}

bool DX12Pipeline::CreatePipelineState(ComPtr<ID3D12Device> device, ComPtr<ID3DBlob> vs,
                                       ComPtr<ID3DBlob> ps)
{
    if (device == nullptr)
    {
        cout << "Device is null" << endl;
        return false;
    }
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                                               D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
{ "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputLayout, _countof(inputLayout)};
    psoDesc.pRootSignature = _rootSignature.Get();
    psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    if (_desc.enableWireFrame)
    {
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    }
    psoDesc.NodeMask = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState = blendDesc;
    
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = _desc.enableDepthTest ? TRUE : FALSE;
    psoDesc.DSVFormat = _desc.enableDepthTest ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
    if (FAILED(hr))
    {
        std::cerr << "[Shader Compiler] Failed to create pipeline state. HRESULT: 0x"
                  << std::hex << hr << std::endl;
    
        return false;
    }
    return true;
}

} // namespace Luna