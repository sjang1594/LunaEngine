#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace Luna
{

// Phase 25: Meshlet data structures for mesh shader rendering
static constexpr uint32_t MAX_MESHLET_VERTICES  = 64;
static constexpr uint32_t MAX_MESHLET_TRIANGLES = 124;

// GPU-visible meshlet descriptor (16 bytes)
struct Meshlet
{
    uint32_t vertexOffset;    // offset into meshletVertices[]
    uint32_t triangleOffset;  // offset into meshletTriangles[] (byte offset / 4)
    uint32_t vertexCount;     // # unique vertices in this meshlet
    uint32_t triangleCount;   // # triangles in this meshlet
};
static_assert(sizeof(Meshlet) == 16, "Meshlet must be 16 bytes");

// GPU-visible meshlet bounding sphere (16 bytes)
struct MeshletBounds
{
    float cx, cy, cz;  // object-space centre
    float radius;       // bounding radius
};
static_assert(sizeof(MeshletBounds) == 16, "MeshletBounds must be 16 bytes");

// Per-mesh meshlet info — parallel to MeshDrawInfo (16 bytes)
struct MeshletMeshInfo
{
    uint32_t meshletOffset;   // first meshlet index in the global meshlet array
    uint32_t meshletCount;    // number of meshlets for this mesh
    uint32_t _pad[2];
};
static_assert(sizeof(MeshletMeshInfo) == 16, "MeshletMeshInfo must be 16 bytes");

// Result of meshlet generation for one mesh
struct MeshletBuildResult
{
    std::vector<Meshlet>        meshlets;
    std::vector<MeshletBounds>  bounds;
    std::vector<uint32_t>       meshletVertices;   // vertex indices into the mesh's vertex buffer
    std::vector<uint32_t>       meshletTriangles;  // packed: 3 local indices per triangle as uint32 (idx0 | idx1<<8 | idx2<<16)
};

// Build meshlets from a triangle list.
// vertices: only used for bounding sphere computation (positions at stride offsets)
// indices: triangle index buffer (3 per tri)
// vertexPositions: array of float3 positions
// vertexCount: total vertices
// indexCount: total indices (must be multiple of 3)
MeshletBuildResult BuildMeshlets(
    const DirectX::XMFLOAT3* vertexPositions,
    uint32_t vertexCount,
    const uint32_t* indices,
    uint32_t indexCount);

} // namespace Luna

