#version 450
// ssgi_vk.comp.glsl — Phase 30: Screen-Space GI compute (Vulkan)
// Half-resolution, cosine-weighted rays, Hi-Z march, temporal accumulation.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform SSGIConstants {
    mat4  invViewProj;
    mat4  prevViewProj;
    mat4  view;
    vec2  screenSize;
    vec2  halfResSize;
    uint  frameCount;
    uint  numRays;
    float maxRayDist;
    float temporalAlpha;
    vec4  _pad;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D depthTex;
layout(set = 0, binding = 2) uniform sampler2D gbuffer0;
layout(set = 0, binding = 3) uniform sampler2D gbuffer1;
layout(set = 0, binding = 4) uniform sampler2D hdrTex;
layout(set = 0, binding = 5) uniform sampler2D hiZTex;
layout(set = 0, binding = 6) uniform sampler2D ssgiHistory;
layout(set = 0, binding = 7, rgba16f) uniform writeonly image2D ssgiOutput;

const float PI = 3.14159265359;

float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint N) {
    return vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

vec3 CosineSampleHemisphere(vec2 xi) {
    float cosTheta = sqrt(max(1.0 - xi.x, 0.0));
    float sinTheta = sqrt(xi.x);
    float phi = 2.0 * PI * xi.y;
    return vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

mat3 BuildTBN(vec3 N) {
    vec3 up = (abs(N.y) < 0.999) ? vec3(0,1,0) : vec3(1,0,0);
    vec3 T  = normalize(cross(up, N));
    vec3 B  = cross(N, T);
    return mat3(T, B, N);
}

vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
    vec4 wp  = ubo.invViewProj * ndc;
    return wp.xyz / wp.w;
}

vec2 ReprojectUV(vec3 posWS) {
    vec4 prevClip = ubo.prevViewProj * vec4(posWS, 1.0);
    prevClip.xyz /= prevClip.w;
    return vec2(prevClip.x * 0.5 + 0.5, 1.0 - (prevClip.y * 0.5 + 0.5));
}

void main() {
    uvec2 halfPx = gl_GlobalInvocationID.xy;
    vec2  halfSz = ubo.halfResSize;
    if (float(halfPx.x) >= halfSz.x || float(halfPx.y) >= halfSz.y) return;

    vec2 uv = (vec2(halfPx) + 0.5) / halfSz;

    float depth = texture(depthTex, uv).r;
    if (depth >= 1.0) {
        imageStore(ssgiOutput, ivec2(halfPx), vec4(0,0,0,1));
        return;
    }

    vec3 albedo    = texture(gbuffer0, uv).rgb;
    vec3 normalEnc = texture(gbuffer1, uv).rgb;
    vec3 N         = normalize(normalEnc * 2.0 - 1.0);
    vec3 posWS     = ReconstructWorldPos(uv, depth);

    mat3 TBN = BuildTBN(N);

    float stepScale  = ubo.maxRayDist / max(halfSz.x, halfSz.y);
    vec2  uvPerStep  = vec2(stepScale * 0.12);
    float depStep    = 0.003;

    vec3 gi  = vec3(0);
    uint Nrays = clamp(ubo.numRays, 1u, 16u);

    for (uint r = 0u; r < Nrays; ++r) {
        uint   sid = (ubo.frameCount * 17u + r * 7u + halfPx.x * 3u + halfPx.y * 11u) & 63u;
        vec2   xi  = Hammersley(sid, 64u);
        xi.x = fract(xi.x + float(r) / float(Nrays));

        vec3 rayTS  = CosineSampleHemisphere(xi);
        vec3 rayDir = TBN * rayTS;

        float NdotR = dot(N, rayDir);
        if (NdotR <= 0.0) { rayDir = -rayDir; NdotR = -NdotR; }

        vec3 endWS   = posWS + rayDir * stepScale * 16.0;
        vec4 vsStart = ubo.view * vec4(posWS, 1.0);
        vec4 vsEnd   = ubo.view * vec4(endWS, 1.0);

        vec2 ssStart = (vsStart.xy / max(vsStart.z, 0.001)) * vec2(0.5, -0.5) + 0.5;
        vec2 ssEnd   = (vsEnd.xy   / max(vsEnd.z,   0.001)) * vec2(0.5, -0.5) + 0.5;
        vec2 sDir    = ssEnd - ssStart;
        float sLen   = length(sDir);
        if (sLen < 1e-5) continue;
        sDir /= sLen;

        vec2  stepUV = sDir * uvPerStep;
        float stepD  = depStep * NdotR;

        vec2  marchUV  = uv + stepUV;
        float marchDep = depth + stepD;
        bool  hit      = false;

        for (int step = 0; step < 16; ++step) {
            if (any(lessThan(marchUV, vec2(0))) || any(greaterThan(marchUV, vec2(1)))) break;
            float hiZSmp = texture(hiZTex, marchUV).r;
            if (marchDep > hiZSmp + 1e-4 && marchDep < hiZSmp + 0.05) {
                hit = true;
                break;
            }
            marchUV  += stepUV;
            marchDep += stepD;
        }

        if (hit) {
            vec3 hitAlbedo   = texture(gbuffer0, marchUV).rgb;
            hitAlbedo        = pow(max(hitAlbedo, vec3(1e-4)), vec3(2.2));
            vec3 hitRadiance = texture(hdrTex, marchUV).rgb;
            gi += hitAlbedo * hitRadiance * NdotR;
        }
    }
    gi /= float(Nrays);

    vec2  histUV = ReprojectUV(posWS);
    bool  valid  = all(greaterThanEqual(histUV, vec2(0))) && all(lessThanEqual(histUV, vec2(1)));
    vec3  history = valid ? texture(ssgiHistory, histUV).rgb : gi;
    float alpha   = valid ? ubo.temporalAlpha : 1.0;
    vec3  blended = mix(history, gi, alpha);

    imageStore(ssgiOutput, ivec2(halfPx), vec4(blended, 1.0));
}
