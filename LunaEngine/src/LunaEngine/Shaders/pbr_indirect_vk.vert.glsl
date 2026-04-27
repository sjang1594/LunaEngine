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

// Set to 1 to output fixed screen-filling triangle (bypass ALL transforms)
// If nothing shows, issue is pipeline state (culling, depth, attachment, etc.)
// If green triangle fills screen, issue is in vertex transform/matrices
#define DEBUG_FIXED_NDC 0

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

#if DEBUG_FIXED_NDC
    // Output a screen-filling triangle at fixed NDC positions (z=0.5, inside clip space)
    // If this doesn't show, the render pass / framebuffer / pipeline is broken
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -3.0),  // off-screen top
        vec2(-1.0,  1.0),  // bottom-left
        vec2( 3.0,  1.0)   // off-screen right
    );
    int vid = gl_VertexIndex % 3;
    gl_Position = vec4(positions[vid], 0.5, 1.0);
    
    outPosWS = vec3(0.0);
    outNormalWS = vec3(0.0, 0.0, 1.0);
    outTangentWS = vec3(1.0, 0.0, 0.0);
    outBitanWS = vec3(0.0, 1.0, 0.0);
    outUV = vec2(0.5);
    outMaterialIndex = 0u;
    return;
#endif

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
