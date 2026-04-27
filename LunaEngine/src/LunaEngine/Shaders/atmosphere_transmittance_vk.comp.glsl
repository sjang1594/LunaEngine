// atmosphere_transmittance_vk.comp.glsl — Transmittance LUT generation (Hillaire 2020)
// Phase 28: Precomputed once at startup. Maps (height, cosZenith) → RGB transmittance.
// Dispatch: (256/16, 64/16, 1) = (16, 4, 1) workgroups
#version 460
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, rgba16f) writeonly uniform image2D outTransmittance;

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

#include "atmosphere_common.glsl"

void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size  = imageSize(outTransmittance);
    if (texel.x >= size.x || texel.y >= size.y) return;

    vec2 uv = (vec2(texel) + 0.5) / vec2(size);

    // Decode parameterization
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

    // Ray from (0, groundRadius + height, 0) in direction (sinZenith, cosZenith, 0)
    float r = atm.groundRadius + height;
    vec3 origin = vec3(0.0, r, 0.0);
    float sinZenith = sqrt(max(1.0 - cosZenith * cosZenith, 0.0));
    vec3 dir = vec3(sinZenith, cosZenith, 0.0);

    // Intersect with atmosphere top
    vec2 tAtm = RaySphereIntersect(origin, dir, atm.atmosphereRadius);
    float tMax = tAtm.y;  // far intersection

    // Check ground intersection
    vec2 tGnd = RaySphereIntersect(origin, dir, atm.groundRadius);
    if (tGnd.x > 0.0) tMax = min(tMax, tGnd.x);

    if (tMax <= 0.0) {
        imageStore(outTransmittance, texel, vec4(1.0));
        return;
    }

    // Ray-march and accumulate optical depth
    const int STEPS = 64;
    vec3 opticalDepth = ComputeOpticalDepth(origin, dir, tMax, STEPS, ap);

    vec3 transmittance = exp(-opticalDepth);
    imageStore(outTransmittance, texel, vec4(transmittance, 1.0));
}

