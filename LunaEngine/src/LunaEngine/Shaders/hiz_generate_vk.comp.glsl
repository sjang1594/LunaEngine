// hiz_generate_vk.comp.glsl — Hi-Z depth pyramid mip generation (Vulkan, GLSL 4.60)
// Phase 23: Reads source mip (set=0, binding=0), writes destination mip (set=0, binding=1).
// Each dispatch generates one mip level: dst[xy] = min of 2×2 source texels.
//
// Push constants: srcWidth, srcHeight, dstWidth, dstHeight (16 bytes)
// Bindings (set=0):
//   binding=0 — sampler2D source mip (combined image sampler)
//   binding=1 — image2D (r32f) destination mip (storage image)
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform HiZConstants {
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    uint copyMode;   // 0 = 2x2 min downsample, 1 = 1:1 copy (depth → mip 0)
};

layout(set = 0, binding = 0) uniform sampler2D gSrcMip;
layout(set = 0, binding = 1, r32f) writeonly uniform image2D gDstMip;

void main()
{
    uvec2 dtid = gl_GlobalInvocationID.xy;
    if (dtid.x >= dstWidth || dtid.y >= dstHeight)
        return;

    if (copyMode != 0u)
    {
        // 1:1 copy (depth → Hi-Z mip 0)
        float d = texelFetch(gSrcMip, ivec2(dtid), 0).r;
        imageStore(gDstMip, ivec2(dtid), vec4(d, 0.0, 0.0, 0.0));
        return;
    }

    // 2×2 min downsample
    uvec2 srcBase = dtid * 2u;
    float d00 = texelFetch(gSrcMip, ivec2(min(srcBase + uvec2(0, 0), uvec2(srcWidth - 1, srcHeight - 1))), 0).r;
    float d10 = texelFetch(gSrcMip, ivec2(min(srcBase + uvec2(1, 0), uvec2(srcWidth - 1, srcHeight - 1))), 0).r;
    float d01 = texelFetch(gSrcMip, ivec2(min(srcBase + uvec2(0, 1), uvec2(srcWidth - 1, srcHeight - 1))), 0).r;
    float d11 = texelFetch(gSrcMip, ivec2(min(srcBase + uvec2(1, 1), uvec2(srcWidth - 1, srcHeight - 1))), 0).r;
    float result = min(min(d00, d10), min(d01, d11));

    imageStore(gDstMip, ivec2(dtid), vec4(result, 0.0, 0.0, 0.0));
}

