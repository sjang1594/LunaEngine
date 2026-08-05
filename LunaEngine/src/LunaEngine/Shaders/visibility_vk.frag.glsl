#version 460
// Phase 32: Visibility buffer — fragment shader (Vulkan GLSL)
// Writes packed uint: bits[31:23] = objectIndex, bits[22:0] = gl_PrimitiveID

layout(location = 0) flat in uint inObjectIdx;

layout(location = 0) out uint outVis;

void main()
{
    outVis = ((inObjectIdx + 1u) << 23u) | (uint(gl_PrimitiveID) & 0x7FFFFFu);
}
