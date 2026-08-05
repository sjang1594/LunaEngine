#version 460
#extension GL_EXT_nonuniform_qualifier : require  // nonuniformEXT() for bindless texture indexing
// Phase 32: Visibility buffer — material evaluation compute (Vulkan GLSL)
// Reconstructs barycentrics from screen position, interpolates attributes,
// samples material textures, writes to G-buffer storage images.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// ── Structs ──────────────────────────────────────────────────────────────────
struct PBRVertex {
    vec3 position;  // offset  0
    vec3 normal;    // offset 12
    vec2 uv;        // offset 24
    vec4 tangent;   // offset 32 (xyz=tangent, w=bitangent sign)
};                  // stride 48

struct GPUObjectData {
    mat4  model;
    vec4  boundingSphere;
    uint  meshIndex;
    uint  materialIndex;
    uvec2 _matCBAddr;
};

struct MeshDrawInfo {
    uint indexCount;
    uint firstIndex;
    int  vertexOffset;
    uint _pad;
};

// ── Descriptors ──────────────────────────────────────────────────────────────
layout(set = 0, binding = 0, row_major) uniform VisShadeUBO {
    mat4  view;
    mat4  proj;
    mat4  viewProj;
    uvec2 screenSize;
    uint  numObjects;
    uint  _pad;
} ubo;

layout(set = 0, binding = 1, r32ui) uniform readonly uimage2D gVisBuf;

layout(set = 1, binding = 0, std430) readonly buffer VB { PBRVertex vertices[]; };
layout(set = 1, binding = 1, std430) readonly buffer IB { uint indices[]; };
layout(set = 1, binding = 2, std430) readonly buffer ObjSSBO { GPUObjectData objects[]; };
layout(set = 1, binding = 3, std430) readonly buffer MeshInfoSSBO { MeshDrawInfo meshInfos[]; };

// G-buffer UAVs
layout(set = 2, binding = 0, rgba8)          uniform image2D gGB0;
layout(set = 2, binding = 1, rgba16f)         uniform image2D gGB1;
layout(set = 2, binding = 2, rgba8)           uniform image2D gGB2;

// Material textures — matches _indirectMaterialLayout (set=3 in vis shade pipeline layout)
// binding 0: matFactorSSBO (ignored here, we use GPUObjectData.materialIndex for array index)
// binding 1..3,5: per-type sampled image arrays (albedo, normal, metalRough, emissive)
// binding 4: sampler
layout(set = 3, binding = 1) uniform texture2D gAlbedoTextures[];
layout(set = 3, binding = 2) uniform texture2D gNormalTextures[];
layout(set = 3, binding = 3) uniform texture2D gMetalRoughTextures[];
layout(set = 3, binding = 4) uniform sampler   gMatSampler;
layout(set = 3, binding = 5) uniform texture2D gEmissiveTextures[];

// ── Barycentric reconstruction ────────────────────────────────────────────────
vec3 ComputeBarycentrics(vec3 p0, vec3 p1, vec3 p2, mat4 model, uint px, uint py)
{
    mat4 mvp = ubo.viewProj * model;
    vec4 c0 = mvp * vec4(p0, 1.0);
    vec4 c1 = mvp * vec4(p1, 1.0);
    vec4 c2 = mvp * vec4(p2, 1.0);

    vec2 n0 = c0.xy / c0.w;
    vec2 n1 = c1.xy / c1.w;
    vec2 n2 = c2.xy / c2.w;

    // Pixel → NDC (Vulkan: origin top-left, +Y down; NDC +Y up after gl_Position.y flip in VS)
    vec2 pixNDC;
    pixNDC.x =  ((float(px) + 0.5) / float(ubo.screenSize.x)) * 2.0 - 1.0;
    pixNDC.y = -((float(py) + 0.5) / float(ubo.screenSize.y)) * 2.0 + 1.0;

    vec2 e01 = n1 - n0, e02 = n2 - n0, ep = pixNDC - n0;
    float denom = e01.x * e02.y - e01.y * e02.x;
    float lam1  = (ep.x * e02.y - ep.y * e02.x) / denom;
    float lam2  = (e01.x * ep.y  - e01.y * ep.x) / denom;
    float lam0  = 1.0 - lam1 - lam2;

    float w0 = 1.0 / c0.w, w1 = 1.0 / c1.w, w2 = 1.0 / c2.w;
    float wSum = lam0 * w0 + lam1 * w1 + lam2 * w2;
    return vec3(lam0 * w0, lam1 * w1, lam2 * w2) / wSum;
}

void main()
{
    uvec2 tid = gl_GlobalInvocationID.xy;
    uint px = tid.x, py = tid.y;
    if (px >= ubo.screenSize.x || py >= ubo.screenSize.y) return;

    uint packed = imageLoad(gVisBuf, ivec2(px, py)).r;

    if (packed == 0u) {
        imageStore(gGB0, ivec2(px,py), vec4(0));
        imageStore(gGB1, ivec2(px,py), vec4(0));
        imageStore(gGB2, ivec2(px,py), vec4(0));
        return;
    }

    uint objIdx = (packed >> 23u) - 1u;  // -1: objectIdx stored as objectIdx+1
    uint primID = packed & 0x7FFFFFu;

    GPUObjectData obj  = objects[objIdx];
    MeshDrawInfo  info = meshInfos[obj.meshIndex];

    uint triBase = info.firstIndex + primID * 3u;
    uint i0 = indices[triBase + 0u] + uint(info.vertexOffset);
    uint i1 = indices[triBase + 1u] + uint(info.vertexOffset);
    uint i2 = indices[triBase + 2u] + uint(info.vertexOffset);

    PBRVertex v0 = vertices[i0];
    PBRVertex v1 = vertices[i1];
    PBRVertex v2 = vertices[i2];

    vec3 bary = ComputeBarycentrics(v0.position, v1.position, v2.position, obj.model, px, py);
    float b0 = bary.x, b1 = bary.y, b2 = bary.z;

    vec2 uv         = b0 * v0.uv      + b1 * v1.uv      + b2 * v2.uv;
    vec3 normalOS   = normalize(b0 * v0.normal  + b1 * v1.normal  + b2 * v2.normal);
    vec4 tanOS      = vec4(normalize(b0 * v0.tangent.xyz + b1 * v1.tangent.xyz + b2 * v2.tangent.xyz),
                           v0.tangent.w);

    mat3 modelRS    = mat3(obj.model);
    vec3 normalWS   = normalize(modelRS * normalOS);
    vec3 tangentWS  = normalize(modelRS * tanOS.xyz);
    vec3 bitangWS   = cross(normalWS, tangentWS) * tanOS.w;

    uint matIdx     = obj.materialIndex;  // indexes per-type texture arrays directly
    // nonuniformEXT required: matIdx differs per thread
    vec4 albedo4    = texture(sampler2D(gAlbedoTextures[nonuniformEXT(matIdx)],     gMatSampler), uv);
    vec3 normalMap  = texture(sampler2D(gNormalTextures[nonuniformEXT(matIdx)],     gMatSampler), uv).xyz;
    vec2 metalRough = texture(sampler2D(gMetalRoughTextures[nonuniformEXT(matIdx)], gMatSampler), uv).bg;
    float emissiveA = texture(sampler2D(gEmissiveTextures[nonuniformEXT(matIdx)],   gMatSampler), uv).r;

    vec3 nTS  = normalMap * 2.0 - 1.0;
    vec3 nWS  = normalize(nTS.x * tangentWS + nTS.y * bitangWS + nTS.z * normalWS);

    imageStore(gGB0, ivec2(px,py), vec4(albedo4.rgb, emissiveA));
    imageStore(gGB1, ivec2(px,py), vec4(nWS * 0.5 + 0.5, 0.0));
    imageStore(gGB2, ivec2(px,py), vec4(metalRough.x, metalRough.y, albedo4.a, 0.0));
}
