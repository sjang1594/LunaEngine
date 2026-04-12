#pragma once
#include "Renderer/Mesh.h"
#include <string>
#include <vector>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12DescriptorHeap;
namespace D3D12MA { class Allocator; }

namespace Luna
{

// ---------------------------------------------------------------------------
// MeshLoader — loads glTF 2.0 / GLB files using cgltf.
// Returns a vector of Mesh objects uploaded to DEFAULT heap via D3D12MA staging.
// The caller is responsible for executing the command list and waiting for
// GPU completion before the staging buffers (internal to LoadGLTF) are released.
// ---------------------------------------------------------------------------
class MeshLoader
{
  public:
    // path        — file system path to .gltf or .glb
    // srvHeap     — optional: CBV/SRV/UAV descriptor heap for material texture SRVs
    // srvDescSize — descriptor increment size (obtained from GetDescriptorHandleIncrementSize)
    // srvAllocIdx — in/out: next free SRV slot index; advanced by 3 per material with textures
    //
    // When srvHeap is non-null, cgltf materials are parsed and mesh->material is populated.
    // Staging upload commands are recorded into cmdList; the caller must execute + wait.
    static std::vector<std::unique_ptr<Mesh>> LoadGLTF(
        const std::string&          path,
        ID3D12Device*               device,
        D3D12MA::Allocator*         allocator,
        ID3D12GraphicsCommandList*  cmdList,
        ID3D12DescriptorHeap*       srvHeap     = nullptr,
        UINT                        srvDescSize = 0,
        UINT*                       srvAllocIdx = nullptr);

  private:
    // Upload a raw CPU buffer to a DEFAULT heap D3D12 resource, recording
    // a CopyBufferRegion + barrier into cmdList.
    // Returns the DEFAULT resource; stagingOut receives the UPLOAD staging buffer
    // which must remain alive until the cmdList is executed and GPU-waited.
    static ComPtr<ID3D12Resource> UploadBuffer(
        D3D12MA::Allocator*        allocator,
        ID3D12GraphicsCommandList* cmdList,
        const void*                data,
        UINT64                     byteSize,
        D3D12_RESOURCE_STATES      finalState,
        D3D12MA::Allocation**      outAlloc,
        ComPtr<ID3D12Resource>&    stagingOut,
        D3D12MA::Allocation**      stagingAllocOut);
};

} // namespace Luna
