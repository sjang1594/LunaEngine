// pbr_indirect_vk.vert.glsl — Vulkan indirect G-buffer vertex shader (GLSL 4.60)
// Phase 15B: Reads model matrix + materialIndex from an SSBO indexed by gl_InstanceIndex.
// firstInstance = objectIndex in VkDrawIndexedIndirectCommand.
//
// Descriptor layout:
//   set=0, binding=0 — ViewProjCB (view + proj matrices, std140 UBO)
//   set=0, binding=1 — GPUObjectData SSBO (readonly, std430)
#version 460

// Diagnostic: set to 1 to bypass SSBO model matrix (use identity)
// If stripes disappear, the issue is in SSBO data or row_major interpretation.
#define DEBUG_IDENTITY_MODEL 0

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 outPosWS;
layout(location = 1) out vec3 outNormalWS;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec3 outTangentWS;
layout(location = 4) out vec3 outBitanWS;
layout(location = 5) flat out uint outMaterialIndex;

struct GPUObjectData {
    mat4  model;          // 64 B — row_major applied at block level below
    vec4  boundingSphere; // 16 B
    uint  meshIndex;      //  4 B
    uint  materialIndex;  //  4 B
    uvec2 _unused;        //  8 B
};

layout(set = 0, binding = 0, std140) uniform ViewProjCB {
    layout(row_major) mat4 viewMatrix;
    layout(row_major) mat4 projectionMatrix;
};

layout(set = 0, binding = 1, std430, row_major) readonly buffer ObjectDataBuffer {
    GPUObjectData gObjectData[];
};

void main()
{
    uint objIdx = uint(gl_InstanceIndex);
    GPUObjectData obj = gObjectData[objIdx];

#if DEBUG_IDENTITY_MODEL
    mat4  modelMat = mat4(1.0);
#else
    mat4  modelMat = obj.model;
#endif
    vec4 worldPos   = vec4(inPosition, 1.0) * modelMat;
    gl_Position     = worldPos * viewMatrix * projectionMatrix;
    gl_Position.y   = -gl_Position.y; // Vulkan NDC Y-down

    outPosWS = worldPos.xyz;

    mat3 normalMat   = mat3(modelMat);
    outNormalWS      = normalize(inNormal      * normalMat);
    outTangentWS     = normalize(inTangent.xyz * normalMat);
    outBitanWS       = normalize(cross(outNormalWS, outTangentWS) * inTangent.w);

    outUV            = inUV;
    outMaterialIndex = obj.materialIndex;
}
