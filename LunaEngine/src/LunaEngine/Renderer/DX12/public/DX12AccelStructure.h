#pragma once
#include <vector>
#include "Renderer/Mesh.h"

namespace Luna
{

// ---------------------------------------------------------------------------
// DX12AccelStructure — builds and stores DXR BLAS (per mesh) + TLAS (scene).
// Requires ID3D12Device5 (DXR Tier 1.0+).
// ---------------------------------------------------------------------------

// One entry in the TLAS: which BLAS (by index into the BLASes built so far)
// and its world-space transform.
struct TLASInstanceDesc
{
    UINT                      blasIndex;
    DirectX::XMFLOAT4X4       worldTransform;
};

class DX12AccelStructure
{
  public:
    DX12AccelStructure() = default;
    ~DX12AccelStructure() = default;

    // Build a BLAS for a single mesh. cmdList must be a DXR-capable list.
    bool BuildBLAS(ID3D12Device5*              device,
                   ID3D12GraphicsCommandList4* cmdList,
                   const Mesh&                 mesh);

    // Build a TLAS from the provided instance list (blasIndex + world transform).
    bool BuildTLAS(ID3D12Device5*                         device,
                   ID3D12GraphicsCommandList4*             cmdList,
                   const std::vector<TLASInstanceDesc>&   instances);

    D3D12_GPU_VIRTUAL_ADDRESS GetTLASAddress() const
    {
        return _tlas ? _tlas->GetGPUVirtualAddress() : 0;
    }

    bool IsValid() const { return _tlas != nullptr; }

  private:
    // Per-mesh BLAS resources
    struct BLASEntry
    {
        ComPtr<ID3D12Resource> blas;
        ComPtr<ID3D12Resource> scratch;
    };
    std::vector<BLASEntry> _blases;

    // Scene TLAS
    ComPtr<ID3D12Resource> _tlas;
    ComPtr<ID3D12Resource> _tlasScratch;
    ComPtr<ID3D12Resource> _instanceDescs;  // UPLOAD heap: array of D3D12_RAYTRACING_INSTANCE_DESC
};

} // namespace Luna
