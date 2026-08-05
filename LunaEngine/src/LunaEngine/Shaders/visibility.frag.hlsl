// Phase 32: Visibility buffer — pixel shader
// Writes packed uint: bits[31:23] = objectIndex (9 bits, max 511 objects)
//                     bits[22:0]  = primitiveID  (23 bits, max ~8M tris per draw)
// SV_PrimitiveID is the local triangle index within the current draw call.
// The shade compute reconstructs absolute triangle index via MeshDrawInfo.firstIndex/3 + primID.

struct PSIn
{
    float4              clipPos   : SV_Position;
    nointerpolation uint objectIdx : OBJECT_IDX;
};

uint main(PSIn input, uint primID : SV_PrimitiveID) : SV_Target
{
    // Pack (objectIdx+1) so 0 is an unambiguous sky/background sentinel.
    return ((input.objectIdx + 1u) << 23u) | (primID & 0x7FFFFFu);
}
