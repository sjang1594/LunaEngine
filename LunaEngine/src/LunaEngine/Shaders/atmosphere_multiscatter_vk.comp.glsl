// atmosphere_multiscatter_vk.comp.glsl — Multi-scattering LUT (Hillaire 2020)
// Phase 28: Precomputed once. Approximates infinite-order scattering.
// Dispatch: (32/8, 32/8, 1) = (4, 4, 1) workgroups
#version 460
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) writeonly uniform image2D outMultiScatter;
layout(set = 0, binding = 1, std140) uniform AtmosphereUBO {
    vec3  sunDirection;        float sunIntensity;
    vec3  rayleighScattering;  float rayleighDensityH;
    float mieScattering;       float mieAbsorption;
    float mieDensityH;         float miePhaseG;
    vec3  ozoneAbsorption;     float ozoneCenterH;
    float ozoneWidth;          float groundRadius;
    float atmosphereRadius;    float cameraHeight;
    vec3  groundAlbedo;        float sunAngularRadius;
    mat4  invViewProj;
    vec2  screenResolution;    float nearPlane;  float farPlane;
} atm;
layout(set = 0, binding = 2) uniform sampler2D transmittanceLUT;

#include "atmosphere_common.glsl"

vec3 SampleTransmittance(float cosZenith, float height) {
    AtmosphereParams ap;
    ap.groundRadius     = atm.groundRadius;
    ap.atmosphereRadius = atm.atmosphereRadius;
    vec2 uv = TransmittanceUVFromParams(cosZenith, height, ap);
    return texture(transmittanceLUT, uv).rgb;
}

void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size  = imageSize(outMultiScatter);
    if (texel.x >= size.x || texel.y >= size.y) return;

    vec2 uv = (vec2(texel) + 0.5) / vec2(size);

    float cosZenith, height;
    AtmosphereParams ap;
    ap.rayleighScattering = atm.rayleighScattering;
    ap.rayleighDensityH   = atm.rayleighDensityH;
    ap.mieScattering      = atm.mieScattering;
    ap.mieAbsorption      = atm.mieAbsorption;
    ap.mieDensityH        = atm.mieDensityH;
    ap.ozoneAbsorption    = atm.ozoneAbsorption;
    ap.ozoneCenterH       = atm.ozoneCenterH;
    ap.ozoneWidth         = atm.ozoneWidth;
    ap.groundRadius       = atm.groundRadius;
    ap.atmosphereRadius   = atm.atmosphereRadius;
    TransmittanceParamsFromUV(uv, ap, cosZenith, height);

    float r = atm.groundRadius + height;
    vec3 origin = vec3(0.0, r, 0.0);
    float sinZ = sqrt(max(1.0 - cosZenith * cosZenith, 0.0));
    vec3 sunDir = vec3(sinZ, cosZenith, 0.0);

    // Integrate over 64 uniform directions on sphere
    const int DIR_SAMPLES = 64;
    vec3 luminance2nd = vec3(0.0);  // 2nd order scattered luminance
    float fms = 0.0;               // fraction that continues scattering

    for (int d = 0; d < DIR_SAMPLES; ++d) {
        // Uniform sphere sampling via Fibonacci spiral
        float theta = acos(1.0 - 2.0 * (float(d) + 0.5) / float(DIR_SAMPLES));
        float phi   = 2.0 * 3.14159265 * float(d) * 0.6180339887;
        vec3 dir = vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));

        // Ray-march this direction
        vec2 tAtm = RaySphereIntersect(origin, dir, atm.atmosphereRadius);
        float tMax = max(tAtm.y, 0.0);

        vec2 tGnd = RaySphereIntersect(origin, dir, atm.groundRadius);
        bool hitGround = tGnd.x > 0.0;
        if (hitGround) tMax = tGnd.x;

        const int STEPS = 20;
        float dt = tMax / float(STEPS);
        vec3 throughput = vec3(1.0);
        vec3 Lscat = vec3(0.0);
        float scatFrac = 0.0;

        for (int s = 0; s < STEPS; ++s) {
            float t = (float(s) + 0.5) * dt;
            vec3 pos = origin + dir * t;
            float h = length(pos) - atm.groundRadius;

            vec3 rE; float mE; vec3 oE;
            GetExtinction(max(h, 0.0), ap, rE, mE, oE);
            vec3 extinction = rE + vec3(mE) + oE;
            vec3 scattering = rE + vec3(atm.mieScattering * DensityMie(max(h, 0.0), atm.mieDensityH));

            // Transmittance to sun from this point
            float cosS = dot(normalize(pos), sunDir);
            vec3 tSun = SampleTransmittance(cosS, max(h, 0.0));

            // Isotropic phase (uniform scattering from all directions)
            float isotropicPhase = 1.0 / (4.0 * 3.14159265);

            vec3 scat = scattering * isotropicPhase;
            Lscat += throughput * scat * tSun * dt;
            scatFrac += dot(throughput * scat, vec3(1.0/3.0)) * dt;

            throughput *= exp(-extinction * dt);
        }

        // Ground bounce contribution
        if (hitGround) {
            vec3 groundPos = origin + dir * tMax;
            float cosS = dot(normalize(groundPos), sunDir);
            vec3 tSun = SampleTransmittance(max(cosS, 0.0), 0.0);
            Lscat += throughput * atm.groundAlbedo * tSun * max(cosS, 0.0) / 3.14159265;
        }

        luminance2nd += Lscat / float(DIR_SAMPLES);
        fms += scatFrac / float(DIR_SAMPLES);
    }

    // Infinite-order approximation: L_ms = L_2nd / (1 - f_ms)
    vec3 multiScatter = luminance2nd / max(1.0 - fms, 0.001);
    imageStore(outMultiScatter, texel, vec4(multiScatter, 1.0));
}

