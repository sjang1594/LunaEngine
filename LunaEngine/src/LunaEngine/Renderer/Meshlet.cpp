#include "LunaPCH.h"
#include "Renderer/Meshlet.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <limits>

namespace Luna
{

MeshletBuildResult BuildMeshlets(
    const DirectX::XMFLOAT3* vertexPositions,
    uint32_t vertexCount,
    const uint32_t* indices,
    uint32_t indexCount)
{
    MeshletBuildResult result;
    if (!indices || indexCount == 0 || indexCount % 3 != 0) return result;

    const uint32_t triCount = indexCount / 3;

    // Simple greedy meshlet builder:
    // Walk triangles in order, add to current meshlet until vertex or triangle limit hit.
    std::vector<uint32_t> localVertexMap(vertexCount, UINT32_MAX);  // global vtx -> local idx
    uint32_t currentMeshletVerts = 0;
    uint32_t currentMeshletTris  = 0;

    Meshlet currentMeshlet{};
    currentMeshlet.vertexOffset   = 0;
    currentMeshlet.triangleOffset = 0;

    auto flushMeshlet = [&]() {
        if (currentMeshletTris == 0) return;
        currentMeshlet.vertexCount   = currentMeshletVerts;
        currentMeshlet.triangleCount = currentMeshletTris;
        result.meshlets.push_back(currentMeshlet);

        // Compute bounding sphere for this meshlet
        MeshletBounds b{};
        float minX = std::numeric_limits<float>::max();
        float minY = minX, minZ = minX;
        float maxX = -minX, maxY = -minX, maxZ = -minX;

        uint32_t base = currentMeshlet.vertexOffset;
        for (uint32_t i = 0; i < currentMeshletVerts; ++i)
        {
            uint32_t gv = result.meshletVertices[base + i];
            const auto& p = vertexPositions[gv];
            minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
            minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
        }
        b.cx = (minX + maxX) * 0.5f;
        b.cy = (minY + maxY) * 0.5f;
        b.cz = (minZ + maxZ) * 0.5f;
        b.radius = 0.0f;
        for (uint32_t i = 0; i < currentMeshletVerts; ++i)
        {
            uint32_t gv = result.meshletVertices[base + i];
            const auto& p = vertexPositions[gv];
            float dx = p.x - b.cx, dy = p.y - b.cy, dz = p.z - b.cz;
            float d = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (d > b.radius) b.radius = d;
        }
        result.bounds.push_back(b);

        // Reset local map for vertices used in this meshlet
        for (uint32_t i = 0; i < currentMeshletVerts; ++i)
            localVertexMap[result.meshletVertices[base + i]] = UINT32_MAX;

        // Start new meshlet
        currentMeshlet.vertexOffset   = static_cast<uint32_t>(result.meshletVertices.size());
        currentMeshlet.triangleOffset = static_cast<uint32_t>(result.meshletTriangles.size());
        currentMeshletVerts = 0;
        currentMeshletTris  = 0;
    };

    for (uint32_t t = 0; t < triCount; ++t)
    {
        uint32_t i0 = indices[t * 3 + 0];
        uint32_t i1 = indices[t * 3 + 1];
        uint32_t i2 = indices[t * 3 + 2];

        // Count how many new vertices this triangle would add
        uint32_t newVerts = 0;
        if (localVertexMap[i0] == UINT32_MAX) ++newVerts;
        if (localVertexMap[i1] == UINT32_MAX) ++newVerts;
        if (localVertexMap[i2] == UINT32_MAX) ++newVerts;

        // If adding this tri would exceed limits, flush
        if (currentMeshletVerts + newVerts > MAX_MESHLET_VERTICES ||
            currentMeshletTris + 1 > MAX_MESHLET_TRIANGLES)
        {
            flushMeshlet();
        }

        // Add vertices
        auto addVertex = [&](uint32_t globalIdx) -> uint32_t {
            if (localVertexMap[globalIdx] == UINT32_MAX)
            {
                localVertexMap[globalIdx] = currentMeshletVerts;
                result.meshletVertices.push_back(globalIdx);
                currentMeshletVerts++;
            }
            return localVertexMap[globalIdx];
        };

        uint32_t l0 = addVertex(i0);
        uint32_t l1 = addVertex(i1);
        uint32_t l2 = addVertex(i2);

        // Pack 3 local indices into a uint32: byte0=l0, byte1=l1, byte2=l2
        uint32_t packed = l0 | (l1 << 8) | (l2 << 16);
        result.meshletTriangles.push_back(packed);
        currentMeshletTris++;
    }

    // Flush remaining
    flushMeshlet();

    return result;
}

} // namespace Luna

