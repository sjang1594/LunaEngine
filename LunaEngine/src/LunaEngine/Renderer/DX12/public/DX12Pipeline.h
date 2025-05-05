#pragma once
#include "Graphics/IPipeline.h"

// Root Signature Creation
// Pipeline State Object (PSO)
// Shader Loading via D3DCompileFromFile

namespace Luna
{
class DX12Pipeline : public IPipeline
{
  public:
    DX12Pipeline() = default;
    ~DX12Pipeline() override = default;

    bool Initialize(const ComPtr<ID3D12Device> &device, const std::wstring &vsPath,
                    const std::wstring &psPath, const PipelineStateDesc &desc);

    ComPtr<ID3D12PipelineState> GetPipelineState() const
    {
        return _pipelineState;
    }
    ComPtr<ID3D12RootSignature> GetRootSignature() const
    {
        return _rootSignature;
    }

  private:
    bool LoadShader(const std::wstring &path, const std::string &target, ComPtr<ID3DBlob> &blob);
    bool CreateRootSignature(const ComPtr<ID3D12Device> &device);
    bool CreatePipelineState(ComPtr<ID3D12Device> device, ComPtr<ID3DBlob> vs, ComPtr<ID3DBlob> ps);

    PipelineStateDesc _desc;
    ComPtr<ID3D12PipelineState> _pipelineState;
    ComPtr<ID3D12RootSignature> _rootSignature;
};
} // namespace Luna