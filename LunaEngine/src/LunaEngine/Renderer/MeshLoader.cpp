#include "LunaPCH.h"

// cgltf single-header implementation — compiled here and only here
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// stb_image needed for decoding embedded texture data (stb_image.h is header-only;
// STB_IMAGE_IMPLEMENTATION is defined in Texture.cpp — include without re-defining here)
#include "stb_image.h"

#include "Renderer/MeshLoader.h"
#include "Renderer/Mesh.h"
#include "Logger/Logger.h"
#include <unordered_map>
#include <functional>

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
// Helper: decode a cgltf image to RGBA8 using stb_image
// ---------------------------------------------------------------------------
static bool DecodeImage(const cgltf_image* image, const std::string& gltfDir,
                        std::vector<uint8_t>& outPixels, uint32_t& outW, uint32_t& outH)
{
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = nullptr;

    if (image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data)
    {
        // Embedded GLB image — decode from memory
        const uint8_t* src  = static_cast<const uint8_t*>(image->buffer_view->buffer->data)
                              + image->buffer_view->offset;
        int            size = static_cast<int>(image->buffer_view->size);
        pixels = stbi_load_from_memory(src, size, &w, &h, &channels, 4);
    }
    else if (image->uri && image->uri[0] != '\0')
    {
        // External file — resolve relative to the glTF directory
        std::string imgPath = gltfDir + image->uri;
        pixels = stbi_load(imgPath.c_str(), &w, &h, &channels, 4);
    }

    if (!pixels) return false;

    outW = static_cast<uint32_t>(w);
    outH = static_cast<uint32_t>(h);
    outPixels.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

// ---------------------------------------------------------------------------
// LoadGLTF — parse glTF/GLB, interleave PBRVertex[], upload to GPU.
// Phase 21: walks glTF node tree to extract per-mesh world transforms.
// Also extracts MaterialCreateInfo for each unique material (no GPU work for textures).
// ---------------------------------------------------------------------------
LoadResult MeshLoader::LoadGLTF(
    const std::string&          path,
    ID3D12Device*               /*device*/,
    D3D12MA::Allocator*         allocator,
    ID3D12GraphicsCommandList*  cmdList)
{
    LoadResult result;

    cgltf_options opts   = {};
    cgltf_data*   data   = nullptr;

    cgltf_result cgltfResult = cgltf_parse_file(&opts, path.c_str(), &data);
    if (cgltfResult != cgltf_result_success)
    {
        LUNA_LOG_ERROR("cgltf: failed to parse %s (result=%d)", path.c_str(), (int)cgltfResult);
        return result;
    }

    cgltfResult = cgltf_load_buffers(&opts, data, path.c_str());
    if (cgltfResult != cgltf_result_success)
    {
        LUNA_LOG_ERROR("cgltf: failed to load buffers for %s", path.c_str());
        cgltf_free(data);
        return result;
    }

    // Compute the directory of the glTF file for resolving relative texture URIs
    std::string gltfDir;
    {
        size_t slash = path.find_last_of("/\\");
        gltfDir = (slash != std::string::npos) ? path.substr(0, slash + 1) : "";
    }

    struct StagingEntry { ComPtr<ID3D12Resource> buf; D3D12MA::Allocation* alloc = nullptr; };
    std::vector<StagingEntry> stagingBuffers;

    // Phase 21: texture decode dedup cache — avoids decoding the same cgltf_image multiple times
    struct DecodedImage { std::vector<uint8_t> pixels; uint32_t w = 0, h = 0; bool valid = false; };
    std::unordered_map<const cgltf_image*, DecodedImage> imageCache;

    auto DecodeImageCached = [&](const cgltf_image* img, std::vector<uint8_t>& outPx,
                                  uint32_t& outW, uint32_t& outH) -> bool {
        if (!img) return false;
        auto it = imageCache.find(img);
        if (it == imageCache.end()) {
            DecodedImage decoded;
            decoded.valid = DecodeImage(img, gltfDir, decoded.pixels, decoded.w, decoded.h);
            it = imageCache.emplace(img, std::move(decoded)).first;
        }
        if (!it->second.valid) return false;
        outPx = it->second.pixels;  // copy (shared across materials)
        outW  = it->second.w;
        outH  = it->second.h;
        return true;
    };

    // Phase 21: helper to process a single primitive with a given world transform
    auto ProcessPrimitive = [&](cgltf_mesh* gltfMesh, cgltf_size mi, cgltf_size pi,
                                 cgltf_primitive& prim, const XMFLOAT4X4& worldTransform)
    {
        if (prim.type != cgltf_primitive_type_triangles) return;

        // Find accessors for POSITION / NORMAL / TEXCOORD_0 / TANGENT
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
            return;
        }

        const cgltf_size vertCount = posAcc->count;
        std::vector<PBRVertex> vertices(vertCount);

        for (cgltf_size v = 0; v < vertCount; ++v)
        {
            float pos[3] = {0,0,0};
            cgltf_accessor_read_float(posAcc, v, pos, 3);
            vertices[v].position = {pos[0], pos[1], pos[2]};

            float nrm[3] = {0,1,0};
            if (nrmAcc) cgltf_accessor_read_float(nrmAcc, v, nrm, 3);
            vertices[v].normal = {nrm[0], nrm[1], nrm[2]};

            float uv[2] = {0,0};
            if (uvAcc) cgltf_accessor_read_float(uvAcc, v, uv, 2);
            vertices[v].uv = {uv[0], uv[1]};

            float tan[4] = {1,0,0,1};
            if (tanAcc) cgltf_accessor_read_float(tanAcc, v, tan, 4);
            vertices[v].tangent = {tan[0], tan[1], tan[2], tan[3]};
        }

        // Index buffer
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
            indices.resize(vertCount);
            for (cgltf_size i = 0; i < vertCount; ++i)
                indices[i] = static_cast<uint32_t>(i);
        }

        // Upload to DEFAULT heap
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

        // Phase 12: compute object-space bounding sphere (centroid + max radius)
        {
            XMVECTOR centroid = XMVectorZero();
            for (const auto& v : vertices)
                centroid = XMVectorAdd(centroid, XMLoadFloat3(&v.position));
            centroid = XMVectorScale(centroid, 1.0f / (float)vertices.size());
            XMFLOAT3 c; XMStoreFloat3(&c, centroid);

            float maxR2 = 0.0f;
            for (const auto& v : vertices)
            {
                XMVECTOR diff = XMVectorSubtract(XMLoadFloat3(&v.position), centroid);
                float r2 = XMVectorGetX(XMVector3LengthSq(diff));
                if (r2 > maxR2) maxR2 = r2;
            }
            mesh->boundingSphere = { c.x, c.y, c.z, sqrtf(maxR2) };
        }

        result.meshes.push_back(std::move(mesh));
        result.transforms.push_back(worldTransform);

        // Build display name: "MeshName/Prim_N" or "Mesh_M/Prim_N"
        std::string displayName;
        if (gltfMesh->name && gltfMesh->name[0])
            displayName = gltfMesh->name;
        else
            displayName = "Mesh_" + std::to_string(mi);
        if (gltfMesh->primitives_count > 1)
            displayName += "/Prim_" + std::to_string(pi);
        result.meshNames.push_back(std::move(displayName));
    };

    // Phase 21: recursive node tree traversal — extract world transforms
    std::function<void(const cgltf_node*, XMMATRIX)> TraverseNode;
    TraverseNode = [&](const cgltf_node* node, XMMATRIX parentWorld)
    {
        // Compute this node's local transform
        float localMat[16];
        cgltf_node_transform_local(node, localMat);
        // cgltf returns column-major; XMMATRIX constructor takes row-major → transpose
        XMMATRIX local = XMMatrixTranspose(XMMATRIX(localMat));
        XMMATRIX world = XMMatrixMultiply(local, parentWorld);

        // If this node has a mesh, process all its primitives
        if (node->mesh)
        {
            XMFLOAT4X4 worldF;
            XMStoreFloat4x4(&worldF, world);

            cgltf_size mi = (cgltf_size)(node->mesh - data->meshes);
            for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi)
                ProcessPrimitive(node->mesh, mi, pi, node->mesh->primitives[pi], worldF);
        }

        // Recurse children
        for (cgltf_size ci = 0; ci < node->children_count; ++ci)
            TraverseNode(node->children[ci], world);
    };

    // Walk all scenes (typically just one)
    XMMATRIX identity = XMMatrixIdentity();
    if (data->scenes_count > 0)
    {
        const cgltf_scene& scene = data->scene ? *data->scene : data->scenes[0];
        for (cgltf_size ni = 0; ni < scene.nodes_count; ++ni)
            TraverseNode(scene.nodes[ni], identity);
    }
    else
    {
        // No scene defined — fall back to iterating root nodes
        for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
        {
            if (data->nodes[ni].parent == nullptr)
                TraverseNode(&data->nodes[ni], identity);
        }
    }

    // Release geometry staging allocations (resources stay alive via ComPtr)
    for (auto& s : stagingBuffers)
        if (s.alloc) s.alloc->Release();

    // -----------------------------------------------------------------------
    // Phase 5B: Extract MaterialCreateInfo for each unique glTF material
    // Phase 21: uses texture decode cache for dedup
    // -----------------------------------------------------------------------
    result.materials.resize(data->materials_count);
    for (cgltf_size mi = 0; mi < data->materials_count; ++mi)
    {
        const cgltf_material& mat = data->materials[mi];
        MaterialCreateInfo&   info = result.materials[mi];

        // Factors
        if (mat.has_pbr_metallic_roughness)
        {
            const auto& pbr = mat.pbr_metallic_roughness;
            info.albedoFactor    = { pbr.base_color_factor[0], pbr.base_color_factor[1],
                                     pbr.base_color_factor[2], pbr.base_color_factor[3] };
            info.metallicFactor  = pbr.metallic_factor;
            info.roughnessFactor = pbr.roughness_factor;

            // Albedo / base-color texture
            if (pbr.base_color_texture.texture && pbr.base_color_texture.texture->image)
                DecodeImageCached(pbr.base_color_texture.texture->image,
                            info.albedoPixels, info.albedoW, info.albedoH);

            // Metallic-roughness texture (G=roughness, B=metallic per glTF spec)
            if (pbr.metallic_roughness_texture.texture && pbr.metallic_roughness_texture.texture->image)
                DecodeImageCached(pbr.metallic_roughness_texture.texture->image,
                            info.metalRoughPixels, info.metalRoughW, info.metalRoughH);
        }

        // Normal map
        if (mat.normal_texture.texture && mat.normal_texture.texture->image)
            DecodeImageCached(mat.normal_texture.texture->image,
                        info.normalPixels, info.normalW, info.normalH);

        // Emissive texture
        if (mat.emissive_texture.texture && mat.emissive_texture.texture->image)
            DecodeImageCached(mat.emissive_texture.texture->image,
                        info.emissivePixels, info.emissiveW, info.emissiveH);
    }

    LUNA_LOG_INFO("Texture cache: %zu unique images decoded (dedup savings)",
                  imageCache.size());

    cgltf_free(data);
    LUNA_LOG_INFO("Loaded %zu mesh(es), %zu material(s), %zu transforms from %s",
                  result.meshes.size(), result.materials.size(), result.transforms.size(), path.c_str());
    return result;
}

} // namespace Luna
