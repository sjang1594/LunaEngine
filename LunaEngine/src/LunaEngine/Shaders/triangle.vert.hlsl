struct VSInput {
    float3 pos : POSITION;
    float3 col : COLOR;
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float3 col : COLOR;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.pos = float4(input.pos, 1.0f);
    output.col = input.col;
    return output;
}