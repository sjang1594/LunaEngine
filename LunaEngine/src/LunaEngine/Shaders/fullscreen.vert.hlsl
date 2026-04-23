// fullscreen.vert.hlsl — Fullscreen triangle vertex shader (SM 6.0)
// Phase 7: Uses SV_VertexID trick to generate a covering triangle without a vertex buffer.
//
//  id=0: pos(-1, 3, 0, 1)  uv(0, -1)
//  id=1: pos(-1,-1, 0, 1)  uv(0,  1)
//  id=2: pos( 3,-1, 0, 1)  uv(2,  1)
//
// The large triangle covers the entire [-1,1]x[-1,1] NDC viewport with one draw call.

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    // Generate clip-space position for each vertex of the covering triangle
    float2 positions[3] = { float2(-1.0,  3.0),
                             float2(-1.0, -1.0),
                             float2( 3.0, -1.0) };
    float2 uvs[3]       = { float2(0.0, -1.0),
                             float2(0.0,  1.0),
                             float2(2.0,  1.0) };
    o.pos = float4(positions[id], 0.0, 1.0);
    o.uv  = uvs[id];
    return o;
}
