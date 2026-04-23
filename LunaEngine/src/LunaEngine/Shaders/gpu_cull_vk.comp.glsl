// gpu_cull_vk.comp.glsl — Vulkan GPU frustum culling compute shader (GLSL 4.60)
// Phase 15B: Tests each object's bounding sphere against 6 frustum planes.
// Visible objects are appended to the VkDrawIndexedIndirectCommand buffer via atomic counter.
//
// Push constants: CullConstants (frustum planes + objectCount) — 112 bytes
// Bindings (set=0):
//   binding=0 — GPUObjectData SSBO  (readonly)
//   binding=1 — MeshDrawInfo SSBO   (readonly)
//   binding=2 — VkDrawCmd SSBO      (read/write)
//   binding=3 — draw count buffer   (read/write, atomic)
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
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
} _cc;

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

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= _cc.objectCount) return;

    GPUObjectData obj = gObjects[idx];

    vec4  centreWS = vec4(obj.boundingSphere.xyz, 1.0) * obj.model;
    float radius   = obj.boundingSphere.w;

    // Extract scale from model matrix columns (same as length of each column xyz)
    float sx = length(vec3(obj.model[0][0], obj.model[0][1], obj.model[0][2]));
    float sy = length(vec3(obj.model[1][0], obj.model[1][1], obj.model[1][2]));
    float sz = length(vec3(obj.model[2][0], obj.model[2][1], obj.model[2][2]));
    radius  *= max(sx, max(sy, sz));

    if (!FrustumTestSphere(centreWS.xyz, radius)) return;

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
