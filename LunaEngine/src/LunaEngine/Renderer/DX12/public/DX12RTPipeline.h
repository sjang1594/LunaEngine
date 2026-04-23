#pragma once
#include <dxcapi.h>

// D3D12 types are available via LunaPCH.h which is force-included for all engine TUs.
// Forward-declare only the interfaces needed to avoid including the full header.
struct ID3D12Device5;
struct ID3D12GraphicsCommandList4;
struct ID3D12Resource;

namespace Luna
{

// ---------------------------------------------------------------------------
// DX12RTPipeline — creates and owns the DXR RTPSO + shader table for shadow rays.
// ---------------------------------------------------------------------------
class DX12RTPipeline
{
  public:
    DX12RTPipeline() = default;
    ~DX12RTPipeline() = default;

    bool Initialize(ID3D12Device5* device, const std::wstring& shaderPath);

    // Dispatch shadow rays: writes to shadowUAV, reads from depthSRV.
    void DispatchShadows(ID3D12GraphicsCommandList4*  cmdList,
                         UINT64                       tlas,      // D3D12_GPU_VIRTUAL_ADDRESS
                         ID3D12Resource*              shadowUAV,
                         ID3D12Resource*              depthSRV,
                         UINT                         width,
                         UINT                         height,
                         UINT64                       shadowCBGPU); // D3D12_GPU_VIRTUAL_ADDRESS

    bool IsReady() const { return _rtpso != nullptr; }

  private:
    bool CompileShaderLibrary(ID3D12Device5* device, const std::wstring& path);
    bool BuildShaderTable(ID3D12Device5* device);

    ComPtr<ID3D12StateObject>      _rtpso;
    ComPtr<ID3D12RootSignature>    _globalRootSig;
    ComPtr<ID3D12Resource>         _shaderTable;
    ComPtr<IDxcBlob>               _shaderLib;

    UINT64 _shaderTableStride = 0;
    UINT   _shaderTableSize   = 0;
};

} // namespace Luna
