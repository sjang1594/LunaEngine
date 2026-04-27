// gbuffer_mesh_vk.mesh.glsl — Mesh shader for G-buffer fill (Vulkan, GLSL 4.60)
// Phase 27: Reads meshlet vertex/index data from SSBOs, outputs triangles
// matching the G-buffer fragment shader input layout.
#version 460
#extension GL_EXT_mesh_shader : require

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;

struct Meshlet {
    uint vertexOffset;
    uint triangleOffset;
    uint vertexCount;
    uint triangleCount;
};

struct GPUObjectData {
    mat4   model;
    vec4   boundingSphere;
    uint   meshIndex;
    uint   materialIndex;
    uvec2  _pad;
};

struct PBRVertex {
    float px, py, pz;    // position
    float nx, ny, nz;    // normal
    float u, v;          // uv
    float tx, ty, tz, tw;// tangent (xyz + handedness w)
};

// set=0 SSBOs
layout(set = 0, binding = 0, std430) readonly buffer ObjectBuffer       { GPUObjectData gObjectData[];   };
layout(set = 0, binding = 1, std430) readonly buffer MeshletBuffer      { Meshlet        gMeshlets[];    };
// binding=2 = bounds (not needed in mesh shader)
layout(set = 0, binding = 3, std430) readonly buffer VertexBuffer       { PBRVertex      gVertices[];    };
layout(set = 0, binding = 4, std430) readonly buffer MeshletVertBuffer  { uint           gMeshletVerts[];};
layout(set = 0, binding = 5, std430) readonly buffer MeshletTriBuffer   { uint           gMeshletTris[]; };

// Push constants (shared with task shader)
layout(push_constant) uniform PushConstants {
    mat4  viewMatrix;
    mat4  projMatrix;
    vec4  frustumPlanes[6];
    uint  objectIndex;
    uint  meshletOffset;
    uint  meshletCount;
    uint  _pad0;
};

// Payload from task shader
struct TaskPayload {
    uint meshletIndices[32];
    uint objectIndex;
    uint materialIndex;
};
taskPayloadSharedEXT TaskPayload payload;

// Per-vertex outputs (match gbuffer_indirect_vk.frag.glsl inputs)
layout(location = 0) out vec3 outPosWS[];
layout(location = 1) out vec3 outNormalWS[];
layout(location = 2) out vec2 outUV[];
layout(location = 3) out vec3 outTangentWS[];
layout(location = 4) out vec3 outBitanWS[];
layout(location = 5) out flat uint outMaterialIndex[];

void main() {
    uint gtid = gl_LocalInvocationIndex;
    uint gid  = gl_WorkGroupID.x;  // one workgroup per visible meshlet

    uint meshletGlobalIdx = payload.meshletIndices[gid];
    Meshlet ml = gMeshlets[meshletGlobalIdx];

    SetMeshOutputsEXT(ml.vertexCount, ml.triangleCount);

    mat4 modelMatrix  = gObjectData[payload.objectIndex].model;
    mat3 normalMatrix = mat3(modelMatrix);
    uint matIdx       = payload.materialIndex;

    // Process vertices (loop if vertexCount > 128)
    for (uint v = gtid; v < ml.vertexCount; v += 128u) {
        uint globalVertIdx = gMeshletVerts[ml.vertexOffset + v];
        PBRVertex vtx = gVertices[globalVertIdx];

        vec4 worldPos = modelMatrix * vec4(vtx.px, vtx.py, vtx.pz, 1.0);
        gl_MeshVerticesEXT[v].gl_Position = projMatrix * viewMatrix * worldPos;

        vec3 nWS = normalize(normalMatrix * vec3(vtx.nx, vtx.ny, vtx.nz));
        vec3 tWS = normalize(normalMatrix * vec3(vtx.tx, vtx.ty, vtx.tz));
        vec3 bWS = normalize(cross(nWS, tWS) * vtx.tw);

        outPosWS[v]          = worldPos.xyz;
        outNormalWS[v]       = nWS;
        outUV[v]             = vec2(vtx.u, vtx.v);
        outTangentWS[v]      = tWS;
        outBitanWS[v]        = bWS;
        outMaterialIndex[v]  = matIdx;
    }

    // Process triangles (loop if triangleCount > 128)
    for (uint t = gtid; t < ml.triangleCount; t += 128u) {
        uint packed = gMeshletTris[ml.triangleOffset + t];
        uint i0 = (packed >>  0u) & 0xFFu;
        uint i1 = (packed >>  8u) & 0xFFu;
        uint i2 = (packed >> 16u) & 0xFFu;
        gl_PrimitiveTriangleIndicesEXT[t] = uvec3(i0, i1, i2);
    }
}

