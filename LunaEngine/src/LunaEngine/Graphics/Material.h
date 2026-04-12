#pragma once
#include <DirectXMath.h>
#include <wrl/client.h>
#include "D3D12MemAlloc.h"
#include "Graphics/Texture.h"
#include <memory>

using Microsoft::WRL::ComPtr;

namespace Luna
{

// ---------------------------------------------------------------------------
// MaterialConstants — mapped to HLSL cbuffer b1 (MaterialBuffer)
//   float4 albedoFactor;
//   float  metallicFactor;
//   float  roughnessFactor;
//   float2 _pad;
// Must be 256-byte aligned for root CBV binding.
// ---------------------------------------------------------------------------
struct MaterialConstants
{
    DirectX::XMFLOAT4 albedoFactor    = {1.0f, 1.0f, 1.0f, 1.0f};
    float              metallicFactor  = 0.0f;
    float              roughnessFactor = 0.5f;
    float              _pad[2]         = {};
};
static_assert(sizeof(MaterialConstants) % 16 == 0,
              "MaterialConstants must be 16-byte aligned");

// ---------------------------------------------------------------------------
// Material — PBR material data bound per draw call.
//
// Texture slots (written into _imGuiSrvHeap by MeshLoader):
//   srvHeapStartIndex + 0 = albedo    (t0)
//   srvHeapStartIndex + 1 = normal    (t1)
//   srvHeapStartIndex + 2 = metalRough (t2)
//   t3 (shadowMap) is a backend-owned resource, not part of Material.
//
// constantBuffer / cbMapped / cbGPUAddr store the per-material b1 CB.
// cbAlloc must be Release()'d in the destructor (D3D12MA allocation).
// ---------------------------------------------------------------------------
struct Material
{
    std::shared_ptr<Texture> albedo;
    std::shared_ptr<Texture> normal;
    std::shared_ptr<Texture> metalRough;

    // Per-material constant buffer (HLSL b1 — MaterialBuffer)
    ComPtr<ID3D12Resource>   constantBuffer;
    D3D12MA::Allocation*     cbAlloc    = nullptr;
    void*                    cbMapped   = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS cbGPUAddr = 0;

    // First SRV slot allocated for this material (albedo=+0, normal=+1, metalRough=+2)
    UINT srvHeapStartIndex = 0;

    Material() = default;
    ~Material()
    {
        if (cbMapped && constantBuffer)
            constantBuffer->Unmap(0, nullptr);
        cbMapped = nullptr;
        if (cbAlloc) { cbAlloc->Release(); cbAlloc = nullptr; }
    }

    // Non-copyable, movable
    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;
    Material(Material&&) = default;
    Material& operator=(Material&&) = default;
};

} // namespace Luna
