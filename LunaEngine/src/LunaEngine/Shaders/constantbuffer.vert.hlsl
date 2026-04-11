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
    float4 worldPos  = mul(modelMatrix,      float4(input.position, 1.0f));
    float4 viewPos   = mul(viewMatrix,       worldPos);
    output.position  = mul(projectionMatrix, viewPos);
    output.color     = input.color;
    return output;
}
