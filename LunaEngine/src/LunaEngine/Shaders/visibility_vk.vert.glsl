#version 460
// Phase 32: Visibility buffer — vertex shader (Vulkan GLSL)
// Mirrors pbr_indirect_vk.vert.glsl descriptor layout so the same descriptor
// set + indirect arg buffer can be reused.
// gl_InstanceIndex = firstInstance (objectIndex) in VkDrawIndexedIndirectCommand.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;

layout(location = 0) flat out uint outObjectIdx;

struct GPUObjectData {
    mat4  model;           // row_major inherited from SSBO block qualifier
    vec4  boundingSphere;
    uint  meshIndex;
    uint  materialIndex;
    uvec2 _unused;
};

layout(set = 0, binding = 0, std140) uniform ViewProjCB {
    layout(row_major) mat4 viewMatrix;
    layout(row_major) mat4 projMatrix;
} vpCB;

layout(set = 0, binding = 1, std430, row_major) readonly buffer ObjectSSBO {
    GPUObjectData objects[];
};

void main()
{
    uint objectIdx = gl_InstanceIndex;  // firstInstance = objectIndex (set by indirect cmd)
    mat4 model     = objects[objectIdx].model;

    vec4 worldPos = model * vec4(inPosition, 1.0);
    vec4 clipPos  = vpCB.projMatrix * vpCB.viewMatrix * worldPos;
    clipPos.y     = -clipPos.y;  // Vulkan NDC +Y down → invert

    gl_Position  = clipPos;
    outObjectIdx = objectIdx;
}
