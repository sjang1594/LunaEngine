// fullscreen_vk.vert.glsl — Vulkan fullscreen triangle vertex shader (GLSL 4.60)
// Phase 28: GLSL equivalent of fullscreen_vk.vert.hlsl for use with GLSL fragment shaders.
#version 460

layout(location = 0) out vec2 outUV;

void main() {
    vec2 positions[3] = vec2[](vec2(-1.0,  3.0),
                                vec2(-1.0, -1.0),
                                vec2( 3.0, -1.0));
    vec2 uvs[3]       = vec2[](vec2(0.0, -1.0),
                                vec2(0.0,  1.0),
                                vec2(2.0,  1.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    outUV = uvs[gl_VertexIndex];
    outUV.y = 1.0 - outUV.y;  // Vulkan inverted Y
}

