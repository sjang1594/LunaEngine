// atmosphere_composite_vk.frag.glsl — Sky composite pass (Hillaire 2020)
// Phase 28: Fullscreen fragment shader. Renders sky for background pixels,
// applies aerial perspective for geometry pixels, renders sun disk.
#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, std140) uniform AtmosphereUBO {
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

layout(set = 0, binding = 1) uniform sampler2D skyViewLUT;
layout(set = 0, binding = 2) uniform sampler2D depthTex;

const float PI = 3.14159265;

// Reconstruct world-space view direction from screen UV + depth
vec3 GetViewDirFromUV(vec2 uv) {
    // NDC: x [-1,1], y [-1,1] (Vulkan: y flipped)
    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clipFar  = vec4(ndc, 1.0, 1.0);
    vec4 clipNear = vec4(ndc, 0.0, 1.0);
    vec4 worldFar  = atm.invViewProj * clipFar;
    vec4 worldNear = atm.invViewProj * clipNear;
    worldFar  /= worldFar.w;
    worldNear /= worldNear.w;
    return normalize(worldFar.xyz - worldNear.xyz);
}

// Map world direction to sky-view LUT UV
vec2 SkyViewUV(vec3 viewDir) {
    float zenith = acos(clamp(viewDir.y, -1.0, 1.0));
    float azimuth = atan(viewDir.z, viewDir.x);
    if (azimuth < 0.0) azimuth += 2.0 * PI;

    // Inverse of non-linear V mapping
    float v;
    if (zenith < 0.5 * PI) {
        float coord = sqrt((0.5 * PI - zenith) / (0.5 * PI));
        v = 0.5 - 0.5 * coord;
    } else {
        float coord = sqrt((zenith - 0.5 * PI) / (0.5 * PI));
        v = 0.5 + 0.5 * coord;
    }

    float u = azimuth / (2.0 * PI);
    return vec2(u, v);
}

// Sun disk with limb darkening
vec3 SunDisk(vec3 viewDir, vec3 sunDir) {
    float cosAngle = dot(viewDir, sunDir);
    float angularRadius = atm.sunAngularRadius;
    float cosRadius = cos(angularRadius);

    if (cosAngle < cosRadius) return vec3(0.0);

    // Limb darkening (approx)
    float u = (cosAngle - cosRadius) / (1.0 - cosRadius);
    float limb = sqrt(clamp(u, 0.0, 1.0));

    // Sun luminance — extremely bright
    return vec3(atm.sunIntensity * 1000.0) * limb;
}

void main() {
    float depth = texture(depthTex, inUV).r;
    bool isSky = (depth >= 0.999);

    // Scene pixels: discard so the LOAD_OP_LOAD scene content is preserved in the framebuffer.
    if (!isSky) discard;

    vec3 viewDir = GetViewDirFromUV(inUV);
    vec3 sunDir = normalize(atm.sunDirection);

    vec2 skyUV = SkyViewUV(viewDir);
    vec3 skyColor = texture(skyViewLUT, skyUV).rgb;
    skyColor += SunDisk(viewDir, sunDir);

    outColor = vec4(skyColor, 1.0);
}

