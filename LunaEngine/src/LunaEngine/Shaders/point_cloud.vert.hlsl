// point_cloud.vert.hlsl — S3: LiDAR point cloud viewport overlay
// Root sig: b0 = 16 root constants (row-major VP 4x4)

cbuffer VPConstants : register(b0)
{
    row_major float4x4 gVP;
};

struct VSIn
{
    float3 pos       : POSITION;
    float  intensity : TEXCOORD0;
};

struct VSOut
{
    float4 pos   : SV_Position;
    float4 color : COLOR;
};

VSOut main(VSIn v)
{
    VSOut o;
    o.pos = mul(float4(v.pos, 1.0f), gVP);

    // False-color by intensity: blue(0) → cyan → green → yellow → red(1)
    float t = saturate(v.intensity);
    float4 c;
    if      (t < 0.25f) c = lerp(float4(0,0,1,1), float4(0,1,1,1), t * 4.0f);
    else if (t < 0.50f) c = lerp(float4(0,1,1,1), float4(0,1,0,1), (t - 0.25f) * 4.0f);
    else if (t < 0.75f) c = lerp(float4(0,1,0,1), float4(1,1,0,1), (t - 0.50f) * 4.0f);
    else                c = lerp(float4(1,1,0,1), float4(1,0,0,1), (t - 0.75f) * 4.0f);
    o.color = c;
    return o;
}
