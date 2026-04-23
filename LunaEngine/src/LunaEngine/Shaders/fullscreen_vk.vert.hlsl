// fullscreen_vk.vert.hlsl — Vulkan fullscreen triangle vertex shader (SM 6.0)
// Same as fullscreen.vert.hlsl but with UV.y flipped for Vulkan NDC conventions.
// In Vulkan, NDC Y=-1 is top of screen (opposite of DX12 Y=+1 at top).
// Without this flip, fullscreen passes would read G-buffer data from the
// vertically-mirrored position, producing incorrect shading.

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    float2 positions[3] = { float2(-1.0,  3.0),
                             float2(-1.0, -1.0),
                             float2( 3.0, -1.0) };
    float2 uvs[3]       = { float2(0.0, -1.0),
                             float2(0.0,  1.0),
                             float2(2.0,  1.0) };
    o.pos = float4(positions[id], 0.0, 1.0);
    o.uv  = uvs[id];
    // Flip UV.y for Vulkan's inverted NDC Y axis
    o.uv.y = 1.0 - o.uv.y;
    return o;
}

 