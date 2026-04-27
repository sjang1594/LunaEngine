#version 460
// cluster_assign_vk.comp.glsl — Phase 24: Clustered Lighting
// Assigns point lights to 16×9×24 view-space clusters (logarithmic depth slicing).
// Dispatch: (16, 9, 24) — one thread per cluster.
//
// set=0, binding=0: ClusterParams (UBO)
// set=0, binding=1: GPUPointLight[] (SSBO, read)
// set=0, binding=2: clusterLightCounts (SSBO, read/write — atomics)
// set=0, binding=3: clusterLightIndices (SSBO, write)


layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

const uint CLUSTER_X = 16u;
const uint CLUSTER_Y = 9u;
const uint CLUSTER_Z = 24u;
const uint MAX_LIGHTS_PER_CLUSTER = 128u;

layout(set = 0, binding = 0, std140) uniform ClusterParams {
    mat4  invProj;         // inverse projection matrix (row-major)
    float nearZ;
    float farZ;
    float screenW;
    float screenH;
    uint  numLights;
    uint  _pad[3];
};

struct GPUPointLight {
    vec3  position;   // view-space position
    float radius;
    vec3  color;
    float intensity;
};

layout(std430, set = 0, binding = 1) readonly buffer LightBuffer {
    GPUPointLight lights[];
};

layout(std430, set = 0, binding = 2) buffer ClusterCounts {
    uint clusterLightCount[];  // [CLUSTER_X * CLUSTER_Y * CLUSTER_Z]
};

layout(std430, set = 0, binding = 3) writeonly buffer ClusterIndices {
    uint clusterLightIndex[];  // [CLUSTER_X * CLUSTER_Y * CLUSTER_Z * MAX_LIGHTS_PER_CLUSTER]
};

// Reconstruct view-space position from screen UV + linear depth via inverse projection
vec3 screenToView(vec2 uv, float z) {
    vec4 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - uv.y * 2.0;  // Vulkan Y-flip
    ndc.z = 0.5;  // arbitrary — we only need XY direction
    ndc.w = 1.0;
    vec4 vs = ndc * invProj;
    vs /= vs.w;
    // Scale XY by z/vs.z to get the actual view-space position at depth z
    return vec3(vs.xy * (z / vs.z), z);
}

// Sphere-AABB intersection test
bool sphereAABBIntersect(vec3 center, float radius, vec3 aabbMin, vec3 aabbMax) {
    // Closest point on AABB to sphere center
    vec3 closest = clamp(center, aabbMin, aabbMax);
    vec3 d = center - closest;
    return dot(d, d) <= radius * radius;
}

void main() {
    uvec3 gid = gl_GlobalInvocationID;
    if (gid.x >= CLUSTER_X || gid.y >= CLUSTER_Y || gid.z >= CLUSTER_Z)
        return;

    uint clusterIdx = gid.x + gid.y * CLUSTER_X + gid.z * CLUSTER_X * CLUSTER_Y;

    // Logarithmic depth slicing
    float logRatio = log(farZ / nearZ);
    float sliceNear = nearZ * exp(logRatio * float(gid.z) / float(CLUSTER_Z));
    float sliceFar  = nearZ * exp(logRatio * float(gid.z + 1u) / float(CLUSTER_Z));

    // Screen UV bounds for this cluster tile
    vec2 uvMin = vec2(float(gid.x) / float(CLUSTER_X), float(gid.y) / float(CLUSTER_Y));
    vec2 uvMax = vec2(float(gid.x + 1u) / float(CLUSTER_X), float(gid.y + 1u) / float(CLUSTER_Y));

    // Reconstruct 4 corners at near and far depth → view-space AABB
    vec3 c0 = screenToView(uvMin, sliceNear);
    vec3 c1 = screenToView(vec2(uvMax.x, uvMin.y), sliceNear);
    vec3 c2 = screenToView(vec2(uvMin.x, uvMax.y), sliceNear);
    vec3 c3 = screenToView(uvMax, sliceNear);
    vec3 c4 = screenToView(uvMin, sliceFar);
    vec3 c5 = screenToView(vec2(uvMax.x, uvMin.y), sliceFar);
    vec3 c6 = screenToView(vec2(uvMin.x, uvMax.y), sliceFar);
    vec3 c7 = screenToView(uvMax, sliceFar);

    vec3 aabbMin = min(min(min(c0, c1), min(c2, c3)), min(min(c4, c5), min(c6, c7)));
    vec3 aabbMax = max(max(max(c0, c1), max(c2, c3)), max(max(c4, c5), max(c6, c7)));

    // Test each light against this cluster's AABB
    uint count = 0u;
    uint baseIdx = clusterIdx * MAX_LIGHTS_PER_CLUSTER;

    for (uint i = 0u; i < numLights && count < MAX_LIGHTS_PER_CLUSTER; ++i) {
        if (sphereAABBIntersect(lights[i].position, lights[i].radius, aabbMin, aabbMax)) {
            clusterLightIndex[baseIdx + count] = i;
            count++;
        }
    }

    clusterLightCount[clusterIdx] = count;
}

