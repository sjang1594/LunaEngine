#pragma once
#include "Graphics/IPipeline.h"
#include <dxcapi.h>

#ifdef LUNA_ENABLE_SLANG
#include <slang/slang-com-ptr.h>  // Slang::ComPtr — not included transitively by slang.h
#include <slang/slang.h>
#endif

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
    ID3D12PipelineState*        GetPSO()           const { return _pipelineState.Get(); }
    ComPtr<ID3D12RootSignature> GetRootSignature() const { return _rootSignature; }

    // Shared DXC library instances (created once, reused across all pipelines)
    static ComPtr<IDxcUtils>     s_DxcUtils;
    static ComPtr<IDxcCompiler3> s_DxcCompiler;
    static void EnsureDXCInitialized();

#ifdef LUNA_ENABLE_SLANG
    // Shared Slang global session (created once alongside DXC, reused across all pipelines)
    static Slang::ComPtr<slang::IGlobalSession> s_SlangGlobalSession;
    static void EnsureSlangInitialized();
#endif

  private:
    bool LoadShaderDXC(const std::wstring &path, const std::wstring &target,
                       ComPtr<IDxcBlob> &outBlob);

#ifdef LUNA_ENABLE_SLANG
    // Slang-based shader compilation (SM 6.0+, .slang source -> DXIL)
    // Returns an IDxcBlob-compatible wrapper around the Slang DXIL output.
    bool LoadShaderSlang(const std::wstring &path, const std::wstring &target,
                         ComPtr<IDxcBlob> &outBlob);
#endif

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
