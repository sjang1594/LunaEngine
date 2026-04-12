#include "LunaPCH.h"

// cgltf single-header implementation — compiled here and only here
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "Renderer/MeshLoader.h"
#include "Renderer/Mesh.h"
#include "Graphics/Material.h"
#include "Graphics/Texture.h"
#include "Logger/Logger.h"

#include <filesystem>

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
// LoadGLTF — parse glTF/GLB, interleave PBRVertex[], upload to GPU.
// When srvHeap/srvDescSize/srvAllocIdx are provided, cgltf materials are parsed
// and mesh->material is populated with textures + a per-material constant buffer.
// ---------------------------------------------------------------------------
std::vector<std::unique_ptr<Mesh>> MeshLoader::LoadGLTF(
    const std::string&          path,
    ID3D12Device*               device,
    D3D12MA::Allocator*         allocator,
    ID3D12GraphicsCommandList*  cmdList,
    ID3D12DescriptorHeap*       srvHeap,
    UINT                        srvDescSize,
    UINT*                       srvAllocIdx)
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

    // Base path for resolving relative texture URIs inside the glTF file
    const std::filesystem::path gltfDir = std::filesystem::path(path).parent_path();

    const bool loadMaterials = (srvHeap != nullptr) && (srvDescSize > 0)
                               && (srvAllocIdx != nullptr) && (device != nullptr);

    // Staging buffers must stay alive until the caller executes + waits the cmdList
    // We keep them in a local vector that gets moved out via lambda captures (intentionally leaked
    // until the next WaitForFrame in the caller).
    struct StagingEntry { ComPtr<ID3D12Resource> buf; D3D12MA::Allocation* alloc = nullptr; };
    std::vector<StagingEntry> stagingBuffers;

    // Helper: create a null SRV (black/zero) for a slot that has no texture.
    // Required to avoid unbound resource errors on the GPU.
    auto WriteNullSRV = [&](UINT slotIndex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        cpuHandle.ptr = srvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                        + static_cast<SIZE_T>(slotIndex) * srvDescSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
        nullDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Texture2D.MipLevels     = 1;
        device->CreateShaderResourceView(nullptr, &nullDesc, cpuHandle);
    };

    // Helper: load a texture from a cgltf_image and write its SRV into slotIndex.
    // Handles both external URI textures and GLB-embedded buffer_view textures.
    // Returns the shared_ptr<Texture> on success, nullptr (+ null SRV) on failure.
    auto LoadTextureSRV = [&](const cgltf_image* img, UINT slotIndex) -> std::shared_ptr<Texture>
    {
        if (!img)
        {
            WriteNullSRV(slotIndex);
            return nullptr;
        }

        auto tex = std::make_shared<Texture>();
        ComPtr<ID3D12Resource> staging;
        D3D12MA::Allocation*   stagingAlloc = nullptr;
        bool loaded = false;

        if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data)
        {
            // GLB embedded image — decode from in-memory bytes (JPEG or PNG)
            const uint8_t* imgData =
                static_cast<const uint8_t*>(img->buffer_view->buffer->data)
                + img->buffer_view->offset;
            size_t imgSize = img->buffer_view->size;
            loaded = tex->LoadFromMemory(imgData, imgSize, device, allocator,
                                         cmdList, staging, &stagingAlloc);
        }
        else if (img->uri && img->uri[0] != '\0')
        {
            // External texture file referenced by relative URI
            std::string texPath = (gltfDir / img->uri).string();
            loaded = tex->Load(texPath, device, allocator, cmdList, staging, &stagingAlloc);
        }

        if (!loaded)
        {
            WriteNullSRV(slotIndex);
            return nullptr;
        }

        stagingBuffers.push_back({staging, stagingAlloc});

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        cpuHandle.ptr = srvHeap->GetCPUDescriptorHandleForHeapStart().ptr
                        + static_cast<SIZE_T>(slotIndex) * srvDescSize;
        tex->CreateSRV(device, cpuHandle);
        return tex;
    };

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

            // ----------------------------------------------------------------
            // Material loading (Phase 5B) — only when srvHeap is provided
            // ----------------------------------------------------------------
            if (loadMaterials && prim.material)
            {
                cgltf_material* m = prim.material;
                auto mat = std::make_shared<Material>();

                // Allocate 3 consecutive SRV slots: albedo(+0), normal(+1), metalRough(+2)
                mat->srvHeapStartIndex = *srvAllocIdx;
                *srvAllocIdx += 3;

                // t0 — albedo (base colour texture)
                const cgltf_image* albedoImg = nullptr;
                if (m->has_pbr_metallic_roughness &&
                    m->pbr_metallic_roughness.base_color_texture.texture)
                    albedoImg = m->pbr_metallic_roughness.base_color_texture.texture->image;
                mat->albedo = LoadTextureSRV(albedoImg, mat->srvHeapStartIndex + 0);

                // t1 — normal map
                const cgltf_image* normalImg = nullptr;
                if (m->normal_texture.texture)
                    normalImg = m->normal_texture.texture->image;
                mat->normal = LoadTextureSRV(normalImg, mat->srvHeapStartIndex + 1);

                // t2 — metallic-roughness map (G=roughness, B=metallic per glTF spec)
                const cgltf_image* metalRoughImg = nullptr;
                if (m->has_pbr_metallic_roughness &&
                    m->pbr_metallic_roughness.metallic_roughness_texture.texture)
                    metalRoughImg = m->pbr_metallic_roughness.metallic_roughness_texture.texture->image;
                mat->metalRough = LoadTextureSRV(metalRoughImg, mat->srvHeapStartIndex + 2);

                // Per-material constant buffer (b1 — MaterialBuffer, 256-byte aligned)
                {
                    const UINT cbSize = (sizeof(MaterialConstants) + 255) & ~255;
                    D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
                    D3D12_RESOURCE_DESC   cbDesc    = CD3DX12_RESOURCE_DESC::Buffer(cbSize);

                    D3D12MA::ALLOCATION_DESC cbAllocDesc = {};
                    cbAllocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

                    HRESULT hr = allocator->CreateResource(
                        &cbAllocDesc, &cbDesc,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                        &mat->cbAlloc,
                        IID_PPV_ARGS(&mat->constantBuffer));

                    if (SUCCEEDED(hr))
                    {
                        D3D12_RANGE noRead = {0, 0};
                        mat->constantBuffer->Map(0, &noRead, &mat->cbMapped);
                        mat->cbGPUAddr = mat->constantBuffer->GetGPUVirtualAddress();

                        MaterialConstants mc = {};
                        if (m->has_pbr_metallic_roughness)
                        {
                            auto& pbr = m->pbr_metallic_roughness;
                            mc.albedoFactor    = { pbr.base_color_factor[0],
                                                   pbr.base_color_factor[1],
                                                   pbr.base_color_factor[2],
                                                   pbr.base_color_factor[3] };
                            mc.metallicFactor  = pbr.metallic_factor;
                            mc.roughnessFactor = pbr.roughness_factor;
                        }
                        memcpy(mat->cbMapped, &mc, sizeof(mc));
                    }
                    else
                    {
                        LUNA_LOG_WARN("MeshLoader: failed to create material CB: 0x%08lX",
                                      (unsigned long)hr);
                    }
                }

                mesh->material = std::move(mat);
            }

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
