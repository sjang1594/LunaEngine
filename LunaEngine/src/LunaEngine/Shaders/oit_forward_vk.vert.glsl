#version 460
// oit_forward_vk.vert.glsl — Phase 31: WBOIT geometry vertex shader (Vulkan)
// Vertex input matches PBRVertex (position+normal+uv+tangent, stride 48B).
//
// Push constants (80B < 128B Vulkan minimum guarantee):
//   model[16] (64B) + alpha (4B) + _pad[3] (12B)
// Descriptor layout:
//   set=0, binding=0 — OITSceneUBO (view, proj, lightDir, lightColor)

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTangent;  // unused in OIT VS

layout(location = 0) out vec3 outPosWS;
layout(location = 1) out vec3 outNormalWS;
layout(location = 2) out vec2 outUV;

layout(push_constant) uniform PC {
    layout(row_major) mat4 model;
    float alpha;
    float _pad[3];
} pc;

layout(set = 0, binding = 0, std140) uniform OITSceneUBO {
    layout(row_major) mat4 viewMatrix;
    layout(row_major) mat4 projMatrix;
    vec3  lightDir;   float _p0;
    vec4  lightColor;
} scene;

void main()
{
    vec4 posWS  = pc.model * vec4(inPosition, 1.0);
    outPosWS    = posWS.xyz;
    gl_Position = scene.projMatrix * scene.viewMatrix * posWS;
    outNormalWS = normalize(mat3(pc.model) * inNormal);
    outUV       = inUV;
}
