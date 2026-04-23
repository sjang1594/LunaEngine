// ssao_blur_vk.frag.glsl — SSAO 3×3 box blur (Vulkan GLSL 4.60)
// Paired with fullscreen.vert.hlsl: inUV at location 0.
//
// Descriptor layout:
//   set=0, binding=0 — ssaoTex    (texture2D, R8_UNORM half-res raw SSAO)
//   set=0, binding=1 — pointClamp (sampler)
#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out float outAO;

layout(set = 0, binding = 0) uniform texture2D ssaoTex;
layout(set = 0, binding = 1) uniform sampler   pointClamp;

void main()
{
    ivec2 texSize   = textureSize(sampler2D(ssaoTex, pointClamp), 0);
    vec2  texelSize = 1.0 / vec2(texSize);

    float ao = 0.0;
    for (int x = -1; x <= 1; x++)
    {
        for (int y = -1; y <= 1; y++)
        {
            vec2 offset = vec2(x, y) * texelSize;
            ao += texture(sampler2D(ssaoTex, pointClamp), inUV + offset).r;
        }
    }
    outAO = ao / 9.0;
}
