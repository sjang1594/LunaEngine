// meshlet_cull_vk.task.glsl — Task shader for mesh shader pipeline (Vulkan, GLSL 4.60)
// Phase 27: Per-meshlet frustum culling. One workgroup per batch of 32 meshlets.
// Visible meshlets are dispatched to the mesh shader via EmitMeshTasksEXT().
#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 32, local_size_y = 1, local_size_z = 1) in;

struct Meshlet {
    uint vertexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;
};

struct MeshletBounds {
    float cx, cy, cz;
    float radius;
};

struct GPUObjectData {
    mat4   model;
    vec4   boundingSphere;
    uint   meshIndex;
    uint   materialIndex;
    uvec2  _pad;
};

// set=0 SSBOs
layout(set = 0, binding = 0, std430) readonly buffer ObjectBuffer   { GPUObjectData gObjectData[]; };
layout(set = 0, binding = 1, std430) readonly buffer MeshletBuffer  { Meshlet        gMeshlets[];  };
layout(set = 0, binding = 2, std430) readonly buffer BoundsBuffer   { MeshletBounds  gBounds[];    };

// Push constants (shared with mesh shader)
layout(push_constant) uniform PushConstants {
    mat4  viewMatrix;        // 64 B
    mat4  projMatrix;        // 64 B
    vec4  frustumPlanes[6];  // 96 B
    uint  objectIndex;       //  4 B
    uint  meshletOffset;     //  4 B
    uint  meshletCount;      //  4 B
    uint  _pad0;             //  4 B = 240 B total
};

// Payload to mesh shader
struct TaskPayload {
    uint meshletIndices[32];
    uint objectIndex;
    uint materialIndex;
};
taskPayloadSharedEXT TaskPayload payload;

shared uint s_visibleCount;

bool FrustumTestSphere(vec3 center, float radius) {
    for (uint i = 0; i < 6; ++i) {
        float d = dot(frustumPlanes[i].xyz, center) + frustumPlanes[i].w;
        if (d < -radius)
            return false;
    }
    return true;
}

void main() {
    uint gtid = gl_LocalInvocationIndex;
    uint gid  = gl_WorkGroupID.x;
    uint meshletLocalIdx = gid * 32u + gtid;
    bool visible = false;

    if (gtid == 0u) s_visibleCount = 0u;
    barrier();

    if (meshletLocalIdx < meshletCount) {
        uint globalIdx = meshletOffset + meshletLocalIdx;
        MeshletBounds mb = gBounds[globalIdx];

        mat4 modelMatrix = gObjectData[objectIndex].model;
        vec3 centerWS = (modelMatrix * vec4(mb.cx, mb.cy, mb.cz, 1.0)).xyz;

        float sx = length(modelMatrix[0].xyz);
        float sy = length(modelMatrix[1].xyz);
        float sz = length(modelMatrix[2].xyz);
        float radiusWS = mb.radius * max(sx, max(sy, sz));

        visible = FrustumTestSphere(centerWS, radiusWS);
    }

    // Compact visible meshlets using shared atomic
    if (visible) {
        uint slot = atomicAdd(s_visibleCount, 1u);
        payload.meshletIndices[slot] = meshletOffset + meshletLocalIdx;
    }
    barrier();

    // First thread writes shared fields and dispatches
    if (gtid == 0u) {
        payload.objectIndex   = objectIndex;
        payload.materialIndex = gObjectData[objectIndex].materialIndex;
        EmitMeshTasksEXT(s_visibleCount, 1u, 1u);
    }
}

