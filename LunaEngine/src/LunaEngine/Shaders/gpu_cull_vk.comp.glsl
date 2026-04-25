// gpu_cull_vk.comp.glsl — Vulkan GPU frustum + Hi-Z occlusion culling (GLSL 4.60)
// Phase 15B: Tests each object's bounding sphere against 6 frustum planes.
// Phase 23: After frustum test, projects bounding sphere to screen AABB and tests
//           against Hi-Z depth pyramid via combined image sampler.
// Visible objects are appended to the VkDrawIndexedIndirectCommand buffer via atomic counter.
//
// Push constants: CullConstants (frustum planes + objectCount + hizFlags) — 128 bytes
// Bindings (set=0):
//   binding=0 — GPUObjectData SSBO  (readonly)
//   binding=1 — MeshDrawInfo SSBO   (readonly)
//   binding=2 — VkDrawCmd SSBO      (read/write)
//   binding=3 — draw count buffer   (read/write, atomic)
//   binding=4 — HiZParams UBO       (viewProj + screenSize + mipCount)
//   binding=5 — Hi-Z combined image sampler (Phase 23)
#version 460

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct GPUObjectData {
    mat4  model;       // row_major applied at block level below
    vec4  boundingSphere;
    uint  meshIndex;
    uint  materialIndex;
    uvec2 _unused;
};

struct MeshDrawInfo {
    uint indexCount;
    uint firstIndex;
    int  vertexOffset;
    uint _pad;
};

struct VkDrawCmd {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

layout(push_constant) uniform CullPush {
    vec4  plane0;
    vec4  plane1;
    vec4  plane2;
    vec4  plane3;
    vec4  plane4;
    vec4  plane5;
    uint  objectCount;
    uint  enableHiZ;       // Phase 23: 0=frustum only, 1=frustum+Hi-Z
    uint  hizMipCount;     // Phase 23: number of Hi-Z mip levels
    uint  _pad0;
    vec4  projParams;      // Phase 23: (proj[0][0], proj[1][1], proj[2][2], proj[3][2])
} _cc;                     // total 128 B

layout(set = 0, binding = 0, std430, row_major) readonly buffer ObjectDataBuffer {
    GPUObjectData gObjects[];
};
layout(set = 0, binding = 1, std430) readonly buffer MeshDrawInfoBuffer {
    MeshDrawInfo gMeshInfo[];
};
layout(set = 0, binding = 2, std430) buffer DrawArgsBuffer {
    VkDrawCmd gDrawArgs[];
};
layout(set = 0, binding = 3, std430) buffer DrawCountBuffer {
    uint data[];
} gDrawCount;

// Phase 23: Hi-Z UBO (viewProj matrix + screen size)
layout(set = 0, binding = 4, std140) uniform HiZParamsUBO {
    mat4 viewProj;         // 64 B
    vec2 screenSize;       //  8 B
    vec2 _hizPad;          //  8 B → 80 B
} hizParams;

layout(set = 0, binding = 5) uniform sampler2D gHiZ;

bool FrustumTestSphere(vec3 centre, float radius)
{
    if (dot(_cc.plane0.xyz, centre) + _cc.plane0.w < -radius) return false;
    if (dot(_cc.plane1.xyz, centre) + _cc.plane1.w < -radius) return false;
    if (dot(_cc.plane2.xyz, centre) + _cc.plane2.w < -radius) return false;
    if (dot(_cc.plane3.xyz, centre) + _cc.plane3.w < -radius) return false;
    if (dot(_cc.plane4.xyz, centre) + _cc.plane4.w < -radius) return false;
    if (dot(_cc.plane5.xyz, centre) + _cc.plane5.w < -radius) return false;
    return true;
}

// Phase 23: Hi-Z occlusion test. Returns true if VISIBLE.
bool HiZTestSphere(vec3 centreWS, float radius)
{
    vec4 clipCentre = vec4(centreWS, 1.0) * hizParams.viewProj;

    // Behind near plane — conservatively visible
    if (clipCentre.w <= 0.0)
        return true;

    vec3 ndc = clipCentre.xyz / clipCentre.w;

    // Project sphere radius to screen-space extent
    float screenRadiusX = abs(radius * hizParams.viewProj[0][0] / clipCentre.w) * 0.5;
    float screenRadiusY = abs(radius * hizParams.viewProj[1][1] / clipCentre.w) * 0.5;

    // NDC → [0,1] UV (Vulkan NDC Y is top-down, adjust accordingly)
    vec2 uvCentre = ndc.xy * 0.5 + 0.5;

    vec2 uvMin = clamp(uvCentre - vec2(screenRadiusX, screenRadiusY), vec2(0.0), vec2(1.0));
    vec2 uvMax = clamp(uvCentre + vec2(screenRadiusX, screenRadiusY), vec2(0.0), vec2(1.0));

    // AABB size in pixels
    vec2 aabbPixels = (uvMax - uvMin) * hizParams.screenSize;
    float maxDim = max(aabbPixels.x, aabbPixels.y);

    // Mip where one texel covers the AABB
    float mipF = (maxDim <= 1.0) ? 0.0 : ceil(log2(maxDim));
    float mip = min(mipF, float(_cc.hizMipCount - 1u));

    // Sample Hi-Z at 4 AABB corners
    float d0 = textureLod(gHiZ, uvMin, mip).r;
    float d1 = textureLod(gHiZ, vec2(uvMax.x, uvMin.y), mip).r;
    float d2 = textureLod(gHiZ, vec2(uvMin.x, uvMax.y), mip).r;
    float d3 = textureLod(gHiZ, uvMax, mip).r;

    float occluderDepth = min(min(d0, d1), min(d2, d3));

    // Standard depth: 0=near, 1=far. Cull if object is entirely behind.
    return ndc.z <= occluderDepth;
}

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= _cc.objectCount) return;

    GPUObjectData obj = gObjects[idx];

    vec4  centreWS = vec4(obj.boundingSphere.xyz, 1.0) * obj.model;
    float radius   = obj.boundingSphere.w;

    // Extract scale from model matrix columns
    float sx = length(vec3(obj.model[0][0], obj.model[0][1], obj.model[0][2]));
    float sy = length(vec3(obj.model[1][0], obj.model[1][1], obj.model[1][2]));
    float sz = length(vec3(obj.model[2][0], obj.model[2][1], obj.model[2][2]));
    radius  *= max(sx, max(sy, sz));

    if (!FrustumTestSphere(centreWS.xyz, radius)) return;

    // Phase 23: Hi-Z occlusion test (1-frame depth lag)
    if (_cc.enableHiZ != 0u)
    {
        if (!HiZTestSphere(centreWS.xyz, radius)) return;
    }

    uint drawIdx = atomicAdd(gDrawCount.data[0], 1u);

    MeshDrawInfo mi = gMeshInfo[obj.meshIndex];

    VkDrawCmd cmd;
    cmd.indexCount    = mi.indexCount;
    cmd.instanceCount = 1u;
    cmd.firstIndex    = mi.firstIndex;
    cmd.vertexOffset  = mi.vertexOffset;
    cmd.firstInstance = idx;

    gDrawArgs[drawIdx] = cmd;
}
