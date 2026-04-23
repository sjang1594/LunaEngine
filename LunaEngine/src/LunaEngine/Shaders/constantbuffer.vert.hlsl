cbuffer TransformBuffer : register(b0)
{
    row_major float4x4 modelMatrix;
    row_major float4x4 viewMatrix;
    row_major float4x4 projectionMatrix;
}

struct VSInput
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float4 worldPos  = mul(float4(input.position, 1.0f), modelMatrix);
    float4 viewPos   = mul(worldPos,  viewMatrix);
    output.position  = mul(viewPos,   projectionMatrix);
    output.color     = input.color;
    return output;
}
