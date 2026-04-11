#pragma once
#include "Graphics/IPipeline.h"
#include <dxcapi.h>

namespace Luna
{
class DX12Pipeline : public IPipeline
{
  public:
    DX12Pipeline() = default;
    ~DX12Pipeline() override = default;

    bool Initialize(const ComPtr<ID3D12Device> &device, const std::wstring &vsPath,
                    const std::wstring &psPath, const PipelineStateDesc &desc);

    ComPtr<ID3D12PipelineState> GetPipelineState() const { return _pipelineState; }
    ComPtr<ID3D12RootSignature> GetRootSignature() const { return _rootSignature; }

    // Shared DXC library instances (created once, reused across all pipelines)
    static ComPtr<IDxcUtils>     s_DxcUtils;
    static ComPtr<IDxcCompiler3> s_DxcCompiler;
    static void EnsureDXCInitialized();

  private:
    // Phase 2B: DXC-based shader compilation (SM 6.0+)
    bool LoadShaderDXC(const std::wstring &path, const std::wstring &target,
                       ComPtr<IDxcBlob> &outBlob);
    bool CreateRootSignature(const ComPtr<ID3D12Device> &device);
    bool CreatePipelineState(const ComPtr<ID3D12Device>& device,
                             ComPtr<IDxcBlob> vs, ComPtr<IDxcBlob> ps);

    PipelineStateDesc           _desc;
    ComPtr<ID3D12PipelineState> _pipelineState;
    ComPtr<ID3D12RootSignature> _rootSignature;
    ComPtr<IDxcBlob>            _vsBlob;
    ComPtr<IDxcBlob>            _psBlob;
};
} // namespace Luna
