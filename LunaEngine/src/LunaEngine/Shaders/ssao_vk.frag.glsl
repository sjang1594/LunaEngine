// ssao_vk.frag.glsl — Screen-Space Ambient Occlusion (Vulkan GLSL 4.60)
// Half-res pass: 16-tap hemisphere kernel, writes raw occlusion to R8_UNORM.
// Paired with fullscreen.vert.hlsl: inUV at location 0.
//
// Descriptor layout:
//   set=0, binding=0 — SSAOConstants UBO
//   set=1, binding=0 — depthTex   (texture2D, D32 sampled as R32)
//   set=1, binding=1 — normalTex  (texture2D, RGBA16F)
//   set=1, binding=2 — noiseTex   (texture2D, R8G8_UNORM)
//   set=1, binding=3 — pointClamp (sampler)
//   set=1, binding=4 — pointWrap  (sampler)
#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out float outAO;

const int SAMPLE_COUNT = 16;

layout(set = 0, binding = 0, std140) uniform SSAOConstants {
    vec4               samples[SAMPLE_COUNT];
    layout(row_major) mat4 projection;
    layout(row_major) mat4 invProjection;
    layout(row_major) mat4 view;
    vec2               noiseScale;
    float              radius;
    float              bias;
};

layout(set = 1, binding = 0) uniform texture2D depthTex;
layout(set = 1, binding = 1) uniform texture2D normalTex;
layout(set = 1, binding = 2) uniform texture2D noiseTex;
layout(set = 1, binding = 3) uniform sampler   pointClamp;
layout(set = 1, binding = 4) uniform sampler   pointWrap;

vec3 ReconstructVSPos(vec2 uv, float depth)
{
    vec2  ndc  = vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
    vec4  clip = vec4(ndc, depth, 1.0);
    vec4  vsP  = clip * invProjection;
    return vsP.xyz / vsP.w;
}

void main()
{
    float depth = texture(sampler2D(depthTex, pointClamp), inUV).r;
    if (depth >= 1.0) { outAO = 1.0; return; }

    vec3 posVS    = ReconstructVSPos(inUV, depth);
    vec3 normalWS = normalize(texture(sampler2D(normalTex, pointClamp), inUV).rgb * 2.0 - 1.0);
    vec3 normalVS = normalize((vec4(normalWS, 0.0) * view).xyz);

    vec2 noiseVec  = texture(sampler2D(noiseTex, pointWrap), inUV * noiseScale).rg * 2.0 - 1.0;
    vec3 randomVec = normalize(vec3(noiseVec, 0.0));

    vec3 tangent   = normalize(randomVec - normalVS * dot(randomVec, normalVS));
    vec3 bitangent = cross(normalVS, tangent);
    mat3 TBN       = mat3(tangent, bitangent, normalVS);

    float occlusion = 0.0;
    for (int i = 0; i < SAMPLE_COUNT; i++)
    {
        vec3 sampleDir = TBN * samples[i].xyz;
        vec3 samplePos = posVS + sampleDir * radius;

        vec4  projPos  = vec4(samplePos, 1.0) * projection;
        projPos.xyz   /= projPos.w;
        vec2  sampleUV = clamp(vec2(projPos.x * 0.5 + 0.5,
                                    0.5 - projPos.y * 0.5), 0.0, 1.0);

        float storedDepth = texture(sampler2D(depthTex, pointClamp), sampleUV).r;
        vec3  storedVS    = ReconstructVSPos(sampleUV, storedDepth);

        float rangeCheck = smoothstep(0.0, 1.0,
                            radius / max(abs(posVS.z - storedVS.z), 0.0001));
        occlusion += (samplePos.z >= storedVS.z + bias) ? rangeCheck : 0.0;
    }

    outAO = 1.0 - occlusion / float(SAMPLE_COUNT);
}
