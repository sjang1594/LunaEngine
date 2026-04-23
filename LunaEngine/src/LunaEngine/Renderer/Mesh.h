#pragma once
#include <DirectXMath.h>
#include "D3D12MemAlloc.h"
#include "Graphics/Material.h"

using namespace DirectX;

namespace Luna
{

// Phase 12: Per-instance data uploaded to a GPU SSBO for indirect drawing + GPU culling.
struct GPUObjectData
{
    XMFLOAT4X4 model;           // 64 B — world transform
    XMFLOAT4   boundingSphere;  // 16 B — xyz=centre (object-space), w=radius
    UINT       meshIndex;       //  4 B — index into MeshDrawInfo[]
    UINT       materialIndex;   //  4 B — SRV heap base for bindless textures
    D3D12_GPU_VIRTUAL_ADDRESS materialCBAddr; //  8 B — material constants GPU VA → 96 B total
};
static_assert(sizeof(GPUObjectData) == 96, "GPUObjectData must be 96 bytes");

// Phase 12: Per-mesh geometry info (offsets into merged VB/IB). Uploaded once at load time.
struct MeshDrawInfo
{
    UINT indexCount;    // number of indices
    UINT firstIndex;    // offset into merged index buffer
    INT  vertexOffset;  // base vertex offset into merged vertex buffer
    UINT _pad;          // align to 16 B
};
static_assert(sizeof(MeshDrawInfo) == 16, "MeshDrawInfo must be 16 bytes");

// Phase 12: Layout must match D3D12 command signature byte order:
//   [0] materialCBAddr (CBV, root param 1)      — offset  0 (8 B)
//   [1] materialIndex  (CONSTANT, root param 2) — offset  8
//   [2] objectIndex    (CONSTANT, root param 3) — offset 12
//   [3] DRAW_INDEXED (IndexCountPerInstance … StartInstanceLocation) — offset 16
//   pad to 40 B
struct IndirectDrawCommand
{
    D3D12_GPU_VIRTUAL_ADDRESS materialCBAddr;     // offset  0 — CBV root param 1 (b1)
    UINT materialIndex;           // offset  8 — root const b2
    UINT objectIndex;             // offset 12 — root const b3
    UINT indexCountPerInstance;   // offset 16
    UINT instanceCount;           // offset 20
    UINT startIndexLocation;      // offset 24
    INT  baseVertexLocation;      // offset 28
    UINT startInstanceLocation;   // offset 32
    UINT _pad;                    // offset 36
};
static_assert(sizeof(IndirectDrawCommand) == 40, "IndirectDrawCommand must be 40 bytes");

// PBR vertex layout — matches pbr.vert.hlsl input layout
struct PBRVertex
{
    XMFLOAT3 position;  // POSITION : 0
    XMFLOAT3 normal;    // NORMAL   : 0
    XMFLOAT2 uv;        // TEXCOORD : 0
    XMFLOAT4 tangent;   // TANGENT  : 0  (xyz=tangent, w=bitangent sign)
};

struct Mesh
{
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12MA::Allocation*   vbAlloc = nullptr;
    D3D12MA::Allocation*   ibAlloc = nullptr;

    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    D3D12_INDEX_BUFFER_VIEW  ibView = {};
    UINT                     indexCount    = 0;
    UINT                     materialIndex = 0;  // index into scene material array

    // Phase 12: object-space bounding sphere (xyz=centre, w=radius), computed at load time
    XMFLOAT4                 boundingSphere = {0,0,0,0};

    // Phase 5B: owning pointer to the GPU material (textures + CB).
    // Null = use mesh_preview (normal-diffuse) fallback pipeline.
    std::shared_ptr<Material> material;

    Mesh() = default;
    ~Mesh()
    {
        if (vbAlloc) { vbAlloc->Release(); vbAlloc = nullptr; }
        if (ibAlloc) { ibAlloc->Release(); ibAlloc = nullptr; }
    }

    // Non-copyable
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) = default;
    Mesh& operator=(Mesh&&) = default;
};

} // namespace Luna
