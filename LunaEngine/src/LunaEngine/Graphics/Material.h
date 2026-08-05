#pragma once
#include "Graphics/Texture.h"
#include <DirectXMath.h>
#include <memory>

// Forward declarations to avoid pulling in D3D12 headers from Mesh.h
struct ID3D12Resource;

namespace Luna
{

// Phase 5B: Material constant buffer — b1 in the PBR root signature.
// Must be 256-byte aligned when placed in an UPLOAD heap CB.
struct MaterialConstants
{
    DirectX::XMFLOAT4 albedoFactor    = {1.0f, 1.0f, 1.0f, 1.0f};
    float              metallicFactor  = 1.0f;
    float              roughnessFactor = 1.0f;
    float              _pad[2]         = {};
    // Total: 32 bytes — padded to 256 when allocating the CB resource
};
static_assert(sizeof(MaterialConstants) == 32, "MaterialConstants size changed");

// Phase 5B: Per-material GPU resources.
// Owns four Texture shared_ptrs (albedo / normalMap / metalRough / emissive) and a small
// constant buffer (MaterialConstants).  Descriptor slots [srvTableStart, srvTableStart+3]
// in the shared SRV heap are reserved for the four SRVs.
struct Material
{
    std::shared_ptr<Texture> albedo;       // SRV at t0 (srvTableStart + 0)
    std::shared_ptr<Texture> normalMap;    // SRV at t1 (srvTableStart + 1)
    std::shared_ptr<Texture> metalRough;   // SRV at t2 (srvTableStart + 2) — G=roughness, B=metallic
    std::shared_ptr<Texture> emissive;     // SRV at t3 (srvTableStart + 3)

    ComPtr<ID3D12Resource>    constantBuffer;
    void*                     cbMapped  = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS cbGPUAddr = 0;

    UINT  srvTableStart = 0;    // index of first of 3 consecutive SRV heap slots
    float alpha         = 1.0f; // Phase 31: OIT — <1.0 routes mesh to transparent pass

    Material()                           = default;
    ~Material()                          = default;
    Material(const Material&)            = delete;
    Material& operator=(const Material&) = delete;
    Material(Material&&)                 = default;
    Material& operator=(Material&&)      = default;
};

} // namespace Luna
