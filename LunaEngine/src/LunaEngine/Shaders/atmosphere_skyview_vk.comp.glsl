// atmosphere_skyview_vk.comp.glsl — Sky-view LUT generation (Hillaire 2020)
// Phase 28: Per-frame. Maps (azimuth, zenith) → sky radiance for current sun position.
// Dispatch: (192/16, 108/4, 1) = (12, 27, 1) workgroups
#version 460
layout(local_size_x = 16, local_size_y = 4, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) writeonly uniform image2D outSkyView;
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
layout(set = 0, binding = 3) uniform sampler2D multiScatterLUT;

#include "atmosphere_common.glsl"

vec3 SampleTransmittance(float cosZenith, float height) {
    AtmosphereParams ap;
    ap.groundRadius     = atm.groundRadius;
    ap.atmosphereRadius = atm.atmosphereRadius;
    vec2 uv = TransmittanceUVFromParams(cosZenith, height, ap);
    return texture(transmittanceLUT, uv).rgb;
}

vec3 SampleMultiScatter(float cosZenith, float height) {
    AtmosphereParams ap;
    ap.groundRadius     = atm.groundRadius;
    ap.atmosphereRadius = atm.atmosphereRadius;
    vec2 uv = TransmittanceUVFromParams(cosZenith, height, ap);
    return texture(multiScatterLUT, uv).rgb;
}

void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size  = imageSize(outSkyView);
    if (texel.x >= size.x || texel.y >= size.y) return;

    vec2 uv = (vec2(texel) + 0.5) / vec2(size);

    // Non-linear parameterization: u → azimuth [0, 2π], v → zenith [0, π]
    float azimuth = uv.x * 2.0 * 3.14159265;
    // Use non-linear V to concentrate resolution near horizon
    float vAdj = uv.y;
    float zenith;
    if (vAdj < 0.5) {
        float coord = 1.0 - 2.0 * vAdj;
        zenith = 0.5 * 3.14159265 - coord * coord * 0.5 * 3.14159265;
    } else {
        float coord = 2.0 * vAdj - 1.0;
        zenith = 0.5 * 3.14159265 + coord * coord * 0.5 * 3.14159265;
    }

    // View direction from (azimuth, zenith) — in world space with Y up
    float sinZ = sin(zenith);
    float cosZ = cos(zenith);
    vec3 viewDir = vec3(sinZ * cos(azimuth), cosZ, sinZ * sin(azimuth));

    // Camera position at (0, groundRadius + height, 0)
    float r = atm.groundRadius + atm.cameraHeight;
    vec3 origin = vec3(0.0, r, 0.0);

    // Intersect view ray with atmosphere
    vec2 tAtm = RaySphereIntersect(origin, viewDir, atm.atmosphereRadius);
    float tMax = max(tAtm.y, 0.0);

    vec2 tGnd = RaySphereIntersect(origin, viewDir, atm.groundRadius);
    bool hitGround = tGnd.x > 0.0;
    if (hitGround) tMax = tGnd.x;

    if (tMax <= 0.0) {
        imageStore(outSkyView, texel, vec4(0.0));
        return;
    }

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

    // Sun direction
    vec3 sunDir = normalize(atm.sunDirection);
    float cosTheta = dot(viewDir, sunDir);  // view-sun angle for phase functions

    // Ray march from camera through atmosphere
    const int STEPS = 32;
    float dt = tMax / float(STEPS);
    vec3 throughput = vec3(1.0);
    vec3 luminance  = vec3(0.0);

    for (int i = 0; i < STEPS; ++i) {
        float t = (float(i) + 0.5) * dt;
        vec3 pos = origin + viewDir * t;
        float h = length(pos) - atm.groundRadius;
        h = max(h, 0.0);

        vec3 rE; float mE; vec3 oE;
        GetExtinction(h, ap, rE, mE, oE);
        vec3 extinction = rE + vec3(mE) + oE;

        // Scattering coefficients at this point
        float dR = DensityRayleigh(h, atm.rayleighDensityH);
        float dM = DensityMie(h, atm.mieDensityH);
        vec3 rayleighScat = atm.rayleighScattering * dR;
        float mieScat     = atm.mieScattering * dM;

        // Transmittance from this point to sun
        float cosSun = dot(normalize(pos), sunDir);
        vec3 tSun = SampleTransmittance(cosSun, h);

        // Single scattering: Rayleigh + Mie phase weighted
        vec3 singleScat = tSun * atm.sunIntensity * (
            rayleighScat * RayleighPhase(cosTheta) +
            vec3(mieScat) * MiePhase(cosTheta, atm.miePhaseG)
        );

        // Multi-scattering contribution (isotropic)
        vec3 ms = SampleMultiScatter(cosSun, h);
        vec3 multiScat = (rayleighScat + vec3(mieScat)) * ms;

        // Integrate
        vec3 scatStep = (singleScat + multiScat);

        // Analytical integration for exponential extinction over dt
        vec3 expExt = exp(-extinction * dt);
        vec3 integScatter = (scatStep - scatStep * expExt) / max(extinction, vec3(1e-10));
        luminance += throughput * integScatter;
        throughput *= expExt;
    }

    // Ground contribution
    if (hitGround) {
        vec3 groundPos = origin + viewDir * tMax;
        float cosS = dot(normalize(groundPos), sunDir);
        vec3 tSun = SampleTransmittance(max(cosS, 0.0), 0.0);
        vec3 groundColor = atm.groundAlbedo * tSun * atm.sunIntensity * max(cosS, 0.0) / 3.14159265;
        luminance += throughput * groundColor;
    }

    imageStore(outSkyView, texel, vec4(luminance, 1.0));
}

