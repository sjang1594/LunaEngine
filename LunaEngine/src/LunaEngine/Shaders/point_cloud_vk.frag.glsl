#version 460
// point_cloud_vk.frag.glsl — S3: LiDAR point cloud viewport overlay (Vulkan)
// Mirrors point_cloud.frag.hlsl: pass the interpolated false-colour straight through.

layout(location = 0) in  vec4 inColor;
layout(location = 0) out vec4 outFragColor;

void main()
{
    outFragColor = inColor;
}
