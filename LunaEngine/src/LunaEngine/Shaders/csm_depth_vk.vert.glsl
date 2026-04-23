// csm_depth_vk.vert.glsl — CSM depth-only vertex shader (Vulkan GLSL 4.60)
// Renders scene geometry depth from the directional light's perspective.
// One draw call per cascade; the light-space MVP is passed as a push constant (64 B).
#version 460

layout(location = 0) in vec3 inPosition;

layout(push_constant) uniform PC {
    layout(row_major) mat4 lightMVP;
} pc;

void main()
{
    gl_Position   = vec4(inPosition, 1.0) * pc.lightMVP;
    gl_Position.y = -gl_Position.y; // Vulkan NDC Y-down (replaces DXC -fvk-invert-y)
}
