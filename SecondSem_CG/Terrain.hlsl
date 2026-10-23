cbuffer Patch : register(b0)
{
    row_major float4x4 ViewProjection;
    float4 LodColor;
};
Texture2D ColorMap : register(t0);
SamplerState LinearClamp : register(s0);

struct VertexOut {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VertexOut TerrainVS(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0)
{
    VertexOut result;
    result.position = mul(float4(position, 1), ViewProjection);
    result.normal = normal;
    result.uv = uv;
    return result;
}

struct GBufferOut {
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float depth : SV_Target2;
};
GBufferOut TerrainPS(VertexOut input)
{
    uint width, height;
    ColorMap.GetDimensions(width, height);
    float2 uv = (saturate(input.uv) * (float2(width, height) - 1) + 0.5) / float2(width, height);
    float3 albedo = ColorMap.Sample(LinearClamp, uv).rgb;
    GBufferOut result;
    result.albedo = float4(lerp(albedo, LodColor.rgb, LodColor.a), 0);
    result.normal = float4(normalize(input.normal), 0.9);
    result.depth = input.position.z;
    return result;
}
