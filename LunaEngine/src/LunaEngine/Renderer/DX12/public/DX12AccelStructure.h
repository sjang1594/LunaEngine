#pragma once
#include <d3d12.h>
#include <wrl.h>

namespace Luna
{
// Phase 4A stub — DXR acceleration structure (BLAS + TLAS).
// Full implementation deferred to Phase 5B+.
class DX12AccelStructure
{
  public:
    DX12AccelStructure() = default;
    ~DX12AccelStructure() = default;

    // Returns GPU virtual address of the Top-Level Acceleration Structure
    D3D12_GPU_VIRTUAL_ADDRESS GetTLASAddress() const { return 0; }
};
} // namespace Luna
