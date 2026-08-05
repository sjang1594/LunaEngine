// point_cloud.frag.hlsl — S3: LiDAR point cloud viewport overlay

struct PSIn
{
    float4 pos   : SV_Position;
    float4 color : COLOR;
};

float4 main(PSIn p) : SV_Target
{
    return p.color;
}
