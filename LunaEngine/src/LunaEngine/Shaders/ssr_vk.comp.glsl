// ssr_vk.comp.glsl — Screen-Space Reflections compute shader (Vulkan GLSL 4.60, Phase 16C)
// set=0: UBO + all textures + storage image + samplers
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform SSRConstants {
    layout(row_major) mat4 view;
    layout(row_major) mat4 proj;
    layout(row_major) mat4 invViewProj;
    vec3   eyePos;      float maxDistance;
    uvec2  screenSize;  uint  maxSteps; float stepSize;
    float  thickness;   float maxRoughness; vec2  _pad0;
    vec4   _pad1;
};

layout(set = 0, binding = 1) uniform texture2D  depthTex;
layout(set = 0, binding = 2) uniform texture2D  normalTex;
layout(set = 0, binding = 3) uniform texture2D  metalRoughTex;
layout(set = 0, binding = 4) uniform texture2D  sceneHDR;
layout(set = 0, binding = 5, rgba16f) uniform image2D ssrOut;
layout(set = 0, binding = 6) uniform sampler    pointClamp;
layout(set = 0, binding = 7) uniform sampler    linearClamp;

vec3 ReconstructWorldPos(vec2 uv, float depth)
{
    vec4 ndc;
    ndc.x =  uv.x * 2.0 - 1.0;
    ndc.y = -uv.y * 2.0 + 1.0;
    ndc.z = depth;
    ndc.w = 1.0;
    vec4 ws = ndc * invViewProj;
    return ws.xyz / ws.w;
}

vec3 WorldToViewPos(vec3 posWS)
{
    vec4 vs = vec4(posWS, 1.0) * view;
    return vs.xyz;
}

vec2 ProjectToUV(vec3 posVS)
{
    vec4 clip = vec4(posVS, 1.0) * proj;
    clip.xyz /= clip.w;
    return vec2(clip.x * 0.5 + 0.5, -clip.y * 0.5 + 0.5);
}

float SampleLinearDepthVS(vec2 uv)
{
    float d  = textureLod(sampler2D(depthTex, pointClamp), uv, 0).r;
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, -uv.y * 2.0 + 1.0, d, 1.0);
    vec4 vs  = ndc * invViewProj;
    vs.xyz  /= vs.w;
    vec4 vsPos = vec4(vs.xyz, 1.0) * view;
    return vsPos.z;
}

void main()
{
    uvec2 tid = gl_GlobalInvocationID.xy;
    if (tid.x >= screenSize.x || tid.y >= screenSize.y)
    {
        imageStore(ssrOut, ivec2(tid), vec4(0.0));
        return;
    }

    vec2  uv    = (vec2(tid) + 0.5) / vec2(screenSize);
    float depth = textureLod(sampler2D(depthTex, pointClamp), uv, 0).r;

    if (depth >= 1.0)
    {
        imageStore(ssrOut, ivec2(tid), vec4(0.0));
        return;
    }

    vec4  mr        = textureLod(sampler2D(metalRoughTex, pointClamp), uv, 0);
    float metallic  = mr.r;
    float roughness = mr.g;

    if (roughness > maxRoughness || metallic < 0.05)
    {
        imageStore(ssrOut, ivec2(tid), vec4(0.0));
        return;
    }

    vec3 posWS      = ReconstructWorldPos(uv, depth);
    vec3 V          = normalize(eyePos - posWS);

    vec4 normalSample = textureLod(sampler2D(normalTex, pointClamp), uv, 0);
    vec3 normalWS     = normalize(normalSample.rgb * 2.0 - 1.0);

    vec3 R        = reflect(-V, normalWS);
    vec3 rayDirVS = normalize((vec4(R, 0.0) * view).xyz);
    vec3 rayOriVS = WorldToViewPos(posWS);
    rayOriVS     += rayDirVS * stepSize;

    float hitWeight = 0.0;
    vec2  hitUV     = uv;

    for (uint i = 0u; i < maxSteps; ++i)
    {
        vec3 sampleVS = rayOriVS + rayDirVS * (stepSize * float(i));
        if (sampleVS.z > -0.01) break;

        vec2 sampleUV = ProjectToUV(sampleVS);
        if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0)))) break;

        float gbufDepthVS = SampleLinearDepthVS(sampleUV);
        float rayDepthVS  = sampleVS.z;

        if (rayDepthVS < gbufDepthVS && rayDepthVS > gbufDepthVS - thickness)
        {
            float stepFade  = 1.0 - (float(i) / float(maxSteps));
            float edgeFadeX = min(sampleUV.x, 1.0 - sampleUV.x) * 10.0;
            float edgeFadeY = min(sampleUV.y, 1.0 - sampleUV.y) * 10.0;
            float edgeFade  = clamp(edgeFadeX, 0.0, 1.0) * clamp(edgeFadeY, 0.0, 1.0);
            float roughFade = 1.0 - clamp(roughness / maxRoughness, 0.0, 1.0);

            hitWeight = stepFade * edgeFade * roughFade * metallic;
            hitUV     = sampleUV;
            break;
        }
    }

    vec3 reflColor = textureLod(sampler2D(sceneHDR, linearClamp), hitUV, 0).rgb;
    imageStore(ssrOut, ivec2(tid), vec4(reflColor * hitWeight, hitWeight));
}
