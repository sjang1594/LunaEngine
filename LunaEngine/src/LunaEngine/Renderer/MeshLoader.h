#pragma once
#include "Renderer/Mesh.h"
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
namespace D3D12MA { class Allocator; }

namespace Luna
{

// Raw material description from a glTF/GLB file.
// Texture pixel data is pre-decoded RGBA8. Empty vector = texture absent; backend uses default.
struct MaterialCreateInfo
{
    // Pre-decoded RGBA8 pixel data for each map; empty = texture absent
    std::vector<uint8_t> albedoPixels;
    uint32_t             albedoW = 0, albedoH = 0;

    std::vector<uint8_t> normalPixels;
    uint32_t             normalW = 0, normalH = 0;

    std::vector<uint8_t> metalRoughPixels;
    uint32_t             metalRoughW = 0, metalRoughH = 0;

    std::vector<uint8_t> emissivePixels;
    uint32_t             emissiveW = 0, emissiveH = 0;

    // glTF PBR metallic-roughness factors
    DirectX::XMFLOAT4 albedoFactor    = {1.0f, 1.0f, 1.0f, 1.0f};
    float              metallicFactor  = 1.0f;
    float              roughnessFactor = 1.0f;

    bool hasAnyTexture() const
    {
        return !albedoPixels.empty() || !normalPixels.empty() || !metalRoughPixels.empty();
    }
};

struct LoadResult
{
    std::vector<std::unique_ptr<Mesh>>  meshes;
    std::vector<MaterialCreateInfo>     materials; // indexed by Mesh::materialIndex
    std::vector<DirectX::XMFLOAT4X4>   transforms; // per-mesh world transform from glTF node hierarchy
    std::vector<std::string>            meshNames;  // display name per mesh (from glTF node/mesh name)
};

// ---------------------------------------------------------------------------
// MeshLoader — loads glTF 2.0 / GLB files using cgltf.
// Returns Mesh objects uploaded to DEFAULT heap via D3D12MA staging, plus
// MaterialCreateInfo for each unique glTF material referenced by a primitive.
// The caller is responsible for executing the command list and waiting for
// GPU completion before using the mesh data.
// ---------------------------------------------------------------------------
class MeshLoader
{
  public:
    // path — file system path to .gltf or .glb
    // Geometry upload commands are recorded into cmdList; the caller must execute + wait.
    // MaterialCreateInfo is returned without GPU work; the caller builds GPU resources from it.
    static LoadResult LoadGLTF(
        const std::string&          path,
        ID3D12Device*               device,
        D3D12MA::Allocator*         allocator,
        ID3D12GraphicsCommandList*  cmdList);

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
