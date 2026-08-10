#version 460
// point_cloud_vk.vert.glsl — S3: LiDAR point cloud viewport overlay (Vulkan)
//
// Mirrors point_cloud.vert.hlsl. Vertex stream is { vec3 position, float intensity },
// stride 16 B — the same layout the DX12 path uploads, so both backends read the
// identical vertex buffer contents.
//
// Push constants (64 B, well inside the 128 B Vulkan minimum guarantee):
//   viewProj[16]

layout(location = 0) in vec3  inPosition;
layout(location = 1) in float inIntensity;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    layout(row_major) mat4 viewProj;
} pc;

void main()
{
    // The push constant carries the engine's DirectXMath view-projection, which is
    // built for ROW-vector maths (clip = pos * VP) — the same convention the DX12
    // shader uses via mul(float4(pos,1), gVP). Multiplying the other way round
    // (VP * pos) applies the transpose and collapses the cloud onto a line.
    gl_Position = vec4(inPosition, 1.0) * pc.viewProj;

    // The engine uses positive-height viewports everywhere and relies on DXC's
    // -fvk-invert-y to reconcile D3D clip space with Vulkan's flipped Y. That flag
    // only applies to HLSL, so a hand-written GLSL shader fed the same D3D-convention
    // view-projection has to do the flip itself or it renders upside down.
    gl_Position.y = -gl_Position.y;

    // Vulkan has no fixed-function point size: unlike D3D it must be written by the
    // vertex shader, otherwise the point is undefined (and typically invisible).
    gl_PointSize = 2.0;

    // False-colour by intensity: blue(0) -> cyan -> green -> yellow -> red(1)
    float t = clamp(inIntensity, 0.0, 1.0);
    vec4  c;
    if      (t < 0.25) c = mix(vec4(0, 0, 1, 1), vec4(0, 1, 1, 1),  t         * 4.0);
    else if (t < 0.50) c = mix(vec4(0, 1, 1, 1), vec4(0, 1, 0, 1), (t - 0.25) * 4.0);
    else if (t < 0.75) c = mix(vec4(0, 1, 0, 1), vec4(1, 1, 0, 1), (t - 0.50) * 4.0);
    else               c = mix(vec4(1, 1, 0, 1), vec4(1, 0, 0, 1), (t - 0.75) * 4.0);
    outColor = c;
}
