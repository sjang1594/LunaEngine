#include "LunaPCH.h"
#include "DX12Pipeline.h"

namespace Luna {

bool DX12Pipeline::Initialize(ComPtr<ID3D12Device> device,
                              const std::wstring &vsPath,
                              const std::wstring &psPath) {
  ComPtr<ID3DBlob> vsBlob;
  ComPtr<ID3DBlob> psBlob;

  if (!LoadShader(vsPath, "vs_5_0", vsBlob))
    return false;
  if (!LoadShader(psPath, "ps_5_0", psBlob))
    return false;

  if (!CreateRootSignature(device))
    return false;
  if (!CreatePipelineState(device, vsBlob, psBlob))
    return false;
  return true;
}

bool DX12Pipeline::LoadShader(const std::wstring &path,
                              const std::string &target,
                              ComPtr<ID3DBlob> &blob) {
  ComPtr<ID3DBlob> errorBlob;
  HRESULT hr = D3DCompileFromFile(
      path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
      target.c_str(), D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errorBlob);

  if (FAILED(hr)) {
    if (errorBlob) {
      std::cerr << "[Shader Compiler] Error in "
                << std::string(path.begin(), path.end()) << ":\n"
                << static_cast<const char *>(errorBlob->GetBufferPointer())
                << std::endl;
    }
    return false;
  }
  return true;
}

bool DX12Pipeline::CreateRootSignature(const ComPtr<ID3D12Device> &device)
{
  CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
  rootSignatureDesc.Init(
      0, nullptr, 0, nullptr,
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

  ComPtr<ID3DBlob> serializedRootSignature;
  ComPtr<ID3DBlob> errorBlob;

  HRESULT hr = D3D12SerializeRootSignature(
      &rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1,
      &serializedRootSignature, &errorBlob);

  if (FAILED(hr)) {
    if (errorBlob) {
      std::cerr << "[Shader Compiler] Error in "
                << static_cast<const char *>(errorBlob->GetBufferPointer())
                << std::endl;
    }
  }

  hr = device->CreateRootSignature(
      0, serializedRootSignature->GetBufferPointer(),
      serializedRootSignature->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));

  return SUCCEEDED(hr);
}

bool DX12Pipeline::CreatePipelineState(ComPtr<ID3D12Device> device,
                                       ComPtr<ID3DBlob> vs,
                                       ComPtr<ID3DBlob> ps)
{
  D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
  psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
  psoDesc.pRootSignature = _rootSignature.Get();
  psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
  psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
  psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
  psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
  psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
  psoDesc.DepthStencilState.DepthEnable = false;
  psoDesc.DepthStencilState.StencilEnable = false;
  psoDesc.SampleMask = UINT_MAX;
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  psoDesc.SampleDesc.Count = 1;

  HRESULT hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&_pipelineState));
  if (FAILED(hr)) {
    std::cerr << "[Shader Compiler] Failed to create pipeline state." << std::endl;
    return false;
  }
  return true;
}

} // namespace Luna