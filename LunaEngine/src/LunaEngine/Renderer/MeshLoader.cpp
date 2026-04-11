#include "LunaPCH.h"

// cgltf single-header implementation — compiled here and only here
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "Renderer/MeshLoader.h"
#include "Renderer/Mesh.h"
#include "Logger/Logger.h"

namespace Luna
{

// ---------------------------------------------------------------------------
// UploadBuffer — generic helper: CPU data → DEFAULT heap via UPLOAD staging
// ---------------------------------------------------------------------------
ComPtr<ID3D12Resource> MeshLoader::UploadBuffer(
    D3D12MA::Allocator*        allocator,
    ID3D12GraphicsCommandList* cmdList,
    const void*                data,
    UINT64                     byteSize,
    D3D12_RESOURCE_STATES      finalState,
    D3D12MA::Allocation**      outAlloc,
    ComPtr<ID3D12Resource>&    stagingOut,
    D3D12MA::Allocation**      stagingAllocOut)
{
    D3D12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize);

    // DEFAULT heap target
    D3D12MA::ALLOCATION_DESC dstDesc = {};
    dstDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> dstBuf;
    HRESULT hr = allocator->CreateResource(&dstDesc, &bufDesc,
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, outAlloc, IID_PPV_ARGS(&dstBuf));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("MeshLoader: DEFAULT alloc failed: 0x%08lX", (unsigned long)hr);
        return nullptr;
    }

    // UPLOAD staging
    D3D12MA::ALLOCATION_DESC stagingDesc = {};
    stagingDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    hr = allocator->CreateResource(&stagingDesc, &bufDesc,
                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                   nullptr, stagingAllocOut, IID_PPV_ARGS(&stagingOut));
    if (FAILED(hr))
    {
        LUNA_LOG_ERROR("MeshLoader: UPLOAD staging alloc failed: 0x%08lX", (unsigned long)hr);
        return nullptr;
    }

    // CPU → staging
    void* mapped = nullptr;
    D3D12_RANGE noRead = {0, 0};
    stagingOut->Map(0, &noRead, &mapped);
    memcpy(mapped, data, static_cast<size_t>(byteSize));
    stagingOut->Unmap(0, nullptr);

    // Staging → DEFAULT
    cmdList->CopyBufferRegion(dstBuf.Get(), 0, stagingOut.Get(), 0, byteSize);

    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        dstBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, finalState);
    cmdList->ResourceBarrier(1, &barrier);

    return dstBuf;
}

// ---------------------------------------------------------------------------
// LoadGLTF — parse glTF/GLB, interleave PBRVertex[], upload to GPU
// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<Mesh>> MeshLoader::LoadGLTF(
    const std::string&          path,
    ID3D12Device*               /*device*/,
    D3D12MA::Allocator*         allocator,
    ID3D12GraphicsCommandList*  cmdList)
{
    std::vector<std::unique_ptr<Mesh>> meshes;

    cgltf_options opts = {};
    cgltf_data*   data = nullptr;

    cgltf_result result = cgltf_parse_file(&opts, path.c_str(), &data);
    if (result != cgltf_result_success)
    {
        LUNA_LOG_ERROR("cgltf: failed to parse %s (result=%d)", path.c_str(), (int)result);
        return meshes;
    }

    result = cgltf_load_buffers(&opts, data, path.c_str());
    if (result != cgltf_result_success)
    {
        LUNA_LOG_ERROR("cgltf: failed to load buffers for %s", path.c_str());
        cgltf_free(data);
        return meshes;
    }

    // Staging buffers must stay alive until the caller executes + waits the cmdList
    // We keep them in a local vector that gets moved out via lambda captures (intentionally leaked
    // until the next WaitForFrame in the caller).
    struct StagingEntry { ComPtr<ID3D12Resource> buf; D3D12MA::Allocation* alloc = nullptr; };
    std::vector<StagingEntry> stagingBuffers;

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
    {
        cgltf_mesh& gltfMesh = data->meshes[mi];

        for (cgltf_size pi = 0; pi < gltfMesh.primitives_count; ++pi)
        {
            cgltf_primitive& prim = gltfMesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            // ----------------------------------------------------------------
            // Find accessors for POSITION / NORMAL / TEXCOORD_0 / TANGENT
            // ----------------------------------------------------------------
            cgltf_accessor* posAcc  = nullptr;
            cgltf_accessor* nrmAcc  = nullptr;
            cgltf_accessor* uvAcc   = nullptr;
            cgltf_accessor* tanAcc  = nullptr;

            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
            {
                cgltf_attribute& attr = prim.attributes[ai];
                if      (attr.type == cgltf_attribute_type_position)  posAcc = attr.data;
                else if (attr.type == cgltf_attribute_type_normal)    nrmAcc = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvAcc  = attr.data;
                else if (attr.type == cgltf_attribute_type_tangent)   tanAcc = attr.data;
            }

            if (!posAcc)
            {
                LUNA_LOG_WARN("cgltf: primitive %zu/%zu has no POSITION — skipped", mi, pi);
                continue;
            }

            const cgltf_size vertCount = posAcc->count;
            std::vector<PBRVertex> vertices(vertCount);

            for (cgltf_size v = 0; v < vertCount; ++v)
            {
                // POSITION
                float pos[3] = {0,0,0};
                cgltf_accessor_read_float(posAcc, v, pos, 3);
                vertices[v].position = {pos[0], pos[1], pos[2]};

                // NORMAL (optional — default face normal)
                float nrm[3] = {0,1,0};
                if (nrmAcc) cgltf_accessor_read_float(nrmAcc, v, nrm, 3);
                vertices[v].normal = {nrm[0], nrm[1], nrm[2]};

                // TEXCOORD_0 (optional)
                float uv[2] = {0,0};
                if (uvAcc) cgltf_accessor_read_float(uvAcc, v, uv, 2);
                vertices[v].uv = {uv[0], uv[1]};

                // TANGENT (optional — default {1,0,0,1})
                float tan[4] = {1,0,0,1};
                if (tanAcc) cgltf_accessor_read_float(tanAcc, v, tan, 4);
                vertices[v].tangent = {tan[0], tan[1], tan[2], tan[3]};
            }

            // ----------------------------------------------------------------
            // Index buffer
            // ----------------------------------------------------------------
            std::vector<uint32_t> indices;
            if (prim.indices)
            {
                const cgltf_size idxCount = prim.indices->count;
                indices.resize(idxCount);
                for (cgltf_size i = 0; i < idxCount; ++i)
                    indices[i] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
            }
            else
            {
                // No index buffer — generate a sequential index list
                indices.resize(vertCount);
                for (cgltf_size i = 0; i < vertCount; ++i)
                    indices[i] = static_cast<uint32_t>(i);
            }

            // ----------------------------------------------------------------
            // Upload to DEFAULT heap
            // ----------------------------------------------------------------
            auto mesh = std::make_unique<Mesh>();

            StagingEntry vbStaging, ibStaging;

            mesh->vertexBuffer = UploadBuffer(
                allocator, cmdList,
                vertices.data(), vertices.size() * sizeof(PBRVertex),
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                &mesh->vbAlloc,
                vbStaging.buf, &vbStaging.alloc);

            mesh->indexBuffer = UploadBuffer(
                allocator, cmdList,
                indices.data(), indices.size() * sizeof(uint32_t),
                D3D12_RESOURCE_STATE_INDEX_BUFFER,
                &mesh->ibAlloc,
                ibStaging.buf, &ibStaging.alloc);

            stagingBuffers.push_back(std::move(vbStaging));
            stagingBuffers.push_back(std::move(ibStaging));

            mesh->vbView.BufferLocation = mesh->vertexBuffer->GetGPUVirtualAddress();
            mesh->vbView.SizeInBytes    = static_cast<UINT>(vertices.size() * sizeof(PBRVertex));
            mesh->vbView.StrideInBytes  = sizeof(PBRVertex);

            mesh->ibView.BufferLocation = mesh->indexBuffer->GetGPUVirtualAddress();
            mesh->ibView.SizeInBytes    = static_cast<UINT>(indices.size() * sizeof(uint32_t));
            mesh->ibView.Format         = DXGI_FORMAT_R32_UINT;

            mesh->indexCount    = static_cast<UINT>(indices.size());
            mesh->materialIndex = (prim.material)
                ? static_cast<UINT>(prim.material - data->materials)
                : 0;

            meshes.push_back(std::move(mesh));
        }
    }

    // Release staging allocations (resources stay alive via ComPtr until stagingBuffers
    // goes out of scope here — after GPU has uploaded since the caller will execute + wait)
    for (auto& s : stagingBuffers)
        if (s.alloc) s.alloc->Release();

    cgltf_free(data);
    LUNA_LOG_INFO("Loaded %zu mesh(es) from %s", meshes.size(), path.c_str());
    return meshes;
}

} // namespace Luna
