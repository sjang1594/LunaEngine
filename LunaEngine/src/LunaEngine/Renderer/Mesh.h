#pragma once
#include <DirectXMath.h>
#include "D3D12MemAlloc.h"
#include "Graphics/Material.h"

using namespace DirectX;

namespace Luna
{

struct GPUObjectData
{
    XMFLOAT4X4 model;           // 64 B — world transform
    XMFLOAT4   boundingSphere;  // 16 B — xyz=centre (object-space), w=radius
    UINT       meshIndex;       //  4 B — index into MeshDrawInfo[]
    UINT       materialIndex;   //  4 B — SRV heap base for bindless textures
    D3D12_GPU_VIRTUAL_ADDRESS materialCBAddr; //  8 B — material constants GPU VA → 96 B total
};
static_assert(sizeof(GPUObjectData) == 96, "GPUObjectData must be 96 bytes");

// Per-mesh geometry offsets into the merged VB/IB for indirect drawing.
struct MeshDrawInfo
{
    UINT indexCount;    // number of indices
    UINT firstIndex;    // offset into merged index buffer
    INT  vertexOffset;  // base vertex offset into merged vertex buffer
    UINT _pad;          // align to 16 B
};
static_assert(sizeof(MeshDrawInfo) == 16, "MeshDrawInfo must be 16 bytes");

// Layout must match D3D12 command signature byte order (materialCBAddr, materialIndex, objectIndex, DRAW_INDEXED).
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
    std::string name;  // display name (from glTF mesh/node name)

    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    D3D12MA::Allocation*   vbAlloc = nullptr;
    D3D12MA::Allocation*   ibAlloc = nullptr;

    D3D12_VERTEX_BUFFER_VIEW vbView = {};
    D3D12_INDEX_BUFFER_VIEW  ibView = {};
    UINT                     indexCount    = 0;
    UINT                     materialIndex = 0;  // index into scene material array

    XMFLOAT4                 boundingSphere = {0,0,0,0};  // object-space (xyz=centre, w=radius)

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
