// atmosphere_common.glsl — Shared atmosphere physics (Hillaire 2020)
// Phase 28: #include'd by all atmosphere compute/fragment shaders.
// NOT a standalone shader — no #version or entry point.

// ---------------------------------------------------------------------------
// Atmosphere parameters UBO (std140, ~256 bytes)
// ---------------------------------------------------------------------------
struct AtmosphereParams {
    vec3  sunDirection;        float sunIntensity;       // 16B
    vec3  rayleighScattering;  float rayleighDensityH;   // 16B
    float mieScattering;       float mieAbsorption;
    float mieDensityH;         float miePhaseG;          // 16B
    vec3  ozoneAbsorption;     float ozoneCenterH;       // 16B
    float ozoneWidth;          float groundRadius;
    float atmosphereRadius;    float cameraHeight;       // 16B
    vec3  groundAlbedo;        float sunAngularRadius;   // 16B
    mat4  invViewProj;                                    // 64B
    vec2  screenResolution;    float nearPlane;
    float farPlane;                                       // 16B
};                                                        // total: 176B

// ---------------------------------------------------------------------------
// Physics helpers
// ---------------------------------------------------------------------------

// Rayleigh phase function
float RayleighPhase(float cosTheta) {
    return (3.0 / (16.0 * 3.14159265)) * (1.0 + cosTheta * cosTheta);
}

// Henyey-Greenstein (Mie) phase function
float MiePhase(float cosTheta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * 3.14159265 * denom * sqrt(denom));
}

// Density at height h above ground for exponential profile
float DensityRayleigh(float h, float scaleH) {
    return exp(-h / scaleH);
}

float DensityMie(float h, float scaleH) {
    return exp(-h / scaleH);
}

// Ozone density: tent function centered at ozoneCenterH
float DensityOzone(float h, float centerH, float width) {
    return max(0.0, 1.0 - abs(h - centerH) / width);
}

// Ray-sphere intersection: returns distances t0, t1 (or -1 if no hit)
// origin at (0, originY, 0), direction normalized
vec2 RaySphereIntersect(vec3 origin, vec3 dir, float radius) {
    float a = dot(dir, dir);
    float b = 2.0 * dot(origin, dir);
    float c = dot(origin, origin) - radius * radius;
    float disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return vec2(-1.0);
    float sq = sqrt(disc);
    return vec2((-b - sq) / (2.0 * a), (-b + sq) / (2.0 * a));
}

// Get extinction coefficients at height h
void GetExtinction(float h, AtmosphereParams atm,
                   out vec3 rayleighExt, out float mieExt, out vec3 ozoneExt) {
    float dR = DensityRayleigh(h, atm.rayleighDensityH);
    float dM = DensityMie(h, atm.mieDensityH);
    float dO = DensityOzone(h, atm.ozoneCenterH, atm.ozoneWidth);
    rayleighExt = atm.rayleighScattering * dR;
    mieExt      = (atm.mieScattering + atm.mieAbsorption) * dM;
    ozoneExt    = atm.ozoneAbsorption * dO;
}

// Total extinction at height h
vec3 TotalExtinction(float h, AtmosphereParams atm) {
    vec3 rE; float mE; vec3 oE;
    GetExtinction(h, atm, rE, mE, oE);
    return rE + vec3(mE) + oE;
}

// Compute optical depth along a ray from origin to atmosphere top
vec3 ComputeOpticalDepth(vec3 origin, vec3 dir, float tMax, int steps, AtmosphereParams atm) {
    float dt = tMax / float(steps);
    vec3 opticalDepth = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + 0.5) * dt;
        vec3 pos = origin + dir * t;
        float h = length(pos) - atm.groundRadius;
        opticalDepth += TotalExtinction(max(h, 0.0), atm) * dt;
    }
    return opticalDepth;
}

// Transmittance LUT parameterization: (cosZenith, height) → UV
vec2 TransmittanceUVFromParams(float cosZenith, float height, AtmosphereParams atm) {
    float H = sqrt(max(atm.atmosphereRadius * atm.atmosphereRadius -
                       atm.groundRadius * atm.groundRadius, 0.0));
    float rho = sqrt(max(height * height + 2.0 * atm.groundRadius * height, 0.0));
    // Use a safe parameterization
    float u = rho / H;
    float v = 0.5 + 0.5 * cosZenith;  // simple linear mapping
    return vec2(u, v);
}

// Inverse: UV → (cosZenith, height)
void TransmittanceParamsFromUV(vec2 uv, AtmosphereParams atm,
                                out float cosZenith, out float height) {
    float H = sqrt(max(atm.atmosphereRadius * atm.atmosphereRadius -
                       atm.groundRadius * atm.groundRadius, 0.0));
    float rho = uv.x * H;
    height = sqrt(rho * rho + atm.groundRadius * atm.groundRadius) - atm.groundRadius;
    cosZenith = uv.y * 2.0 - 1.0;
}

