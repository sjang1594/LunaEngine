// skybox.vert.hlsl — Phase 14: environment mapping
// Fullscreen-triangle trick; reconstructs world-space ray direction
// from inverse(ViewProj) and passes it to the pixel shader.
// No vertex buffer — driven by SV_VertexID (DrawInstanced(3,1,0,0)).

cbuffer SkyboxCB : register(b0)
{
    row_major float4x4 invViewProj; // inverse of (unjittered) view * proj
};

struct VSOut
{
    float4 pos    : SV_POSITION;
    float3 rayDir : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    // Generate a full-screen triangle in NDC; set z = 1.0 (far plane depth)
    float2 uv  = float2((vid << 1) & 2, vid & 2);
    float2 ndc = uv * 2.0f - 1.0f;

    // Unproject NDC (z=1) back to world space — removes translation via w=0 trick
    float4 worldPos = mul(float4(ndc, 1.0f, 1.0f), invViewProj);
    worldPos /= worldPos.w;

    // Camera position is (invViewProj * (0,0,0,1)).xyz — subtract to get ray dir
    float4 camPos  = mul(float4(0.0f, 0.0f, 0.0f, 1.0f), invViewProj);
    camPos        /= camPos.w;

    VSOut o;
    o.pos    = float4(ndc, 1.0f, 1.0f); // z=w=1 → hardware depth = 1.0 exactly
    o.rayDir = worldPos.xyz - camPos.xyz;
    return o;
}

