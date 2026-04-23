// mesh_preview.frag.hlsl — SM 6.0
// Simple hemisphere diffuse shading by world normal.
// Used as a lightweight preview for glTF meshes before full PBR is wired up.

struct PSInput
{
    float4 position    : SV_POSITION;
    float3 worldNormal : NORMAL;
};

float4 main(PSInput input) : SV_TARGET
{
    float3 N        = normalize(input.worldNormal);
    float3 lightDir = normalize(float3(0.4f, 1.0f, 0.6f));
    float  diffuse  = saturate(dot(N, lightDir)) * 0.85f + 0.15f;  // ambient 0.15
    float3 color    = float3(0.72f, 0.76f, 0.82f);                 // silver-gray base
    return float4(color * diffuse, 1.0f);
}
