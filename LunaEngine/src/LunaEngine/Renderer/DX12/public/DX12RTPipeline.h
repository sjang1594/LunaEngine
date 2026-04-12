#pragma once
#include <d3d12.h>
#include <wrl.h>
using Microsoft::WRL::ComPtr;

namespace Luna
{
// Phase 4A stub — DXR ray-tracing pipeline.
// Full implementation deferred to Phase 5B+.
class DX12RTPipeline
{
  public:
    DX12RTPipeline() = default;
    ~DX12RTPipeline() = default;

    // Dispatch shadow rays into the shadow UAV
    void DispatchShadows(
        ID3D12GraphicsCommandList4* /*cmdList*/,
        D3D12_GPU_VIRTUAL_ADDRESS   /*tlasAddress*/,
        ID3D12Resource*             /*shadowUAV*/,
        ID3D12Resource*             /*depthSRV*/,
        UINT                        /*width*/,
        UINT                        /*height*/,
        D3D12_GPU_VIRTUAL_ADDRESS   /*mvpCBAddr*/)
    {}
};
} // namespace Luna
