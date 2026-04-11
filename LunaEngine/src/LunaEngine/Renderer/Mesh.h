#pragma once
#include <DirectXMath.h>
#include "D3D12MemAlloc.h"

using namespace DirectX;

namespace Luna
{

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
    UINT                     indexCount   = 0;
    UINT                     materialIndex = 0;  // index into scene material array

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
