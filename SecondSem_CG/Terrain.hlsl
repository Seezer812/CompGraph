cbuffer Patch : register(b0)
{
    row_major float4x4 ViewProjection;
    float4 LodColor;
    float4 TerrainSettings;
};
Texture2D ColorMap : register(t0);
Texture2D<float> HeightMap : register(t1);
SamplerState LinearClamp : register(s0);

float RawHeight(float2 uv)
{
    uint width, height;
    HeightMap.GetDimensions(width, height);
    float2 pixel = saturate(uv) * float2(width - 1, height - 1);
    int2 a = int2(pixel);
    int2 b = min(a + 1, int2(width - 1, height - 1));
    float2 t = frac(pixel);
    float top = lerp(HeightMap.Load(int3(a, 0)), HeightMap.Load(int3(b.x, a.y, 0)), t.x);
    float bottom = lerp(HeightMap.Load(int3(a.x, b.y, 0)), HeightMap.Load(int3(b, 0)), t.x);
    return lerp(top, bottom, t.y) * TerrainSettings.y;
}

float TerrainHeight(float2 uv)
{
    float raw = RawHeight(uv);
    if (TerrainSettings.z == 0) return raw;
    float radius = length((uv - 0.5) * TerrainSettings.x);
    float t = smoothstep(24.0, 48.0, radius);
    return (raw - RawHeight(float2(0.5, 0.5))) * t - 0.10;
}

struct VertexOut {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VertexOut TerrainVS(float2 positionXZ : POSITION, float2 uv : TEXCOORD0, float skirtDrop : TEXCOORD1)
{
    VertexOut result;
    float3 position = float3(positionXZ.x, TerrainHeight(uv) - skirtDrop, positionXZ.y);
    result.position = mul(float4(position, 1), ViewProjection);
    uint width, height;
    HeightMap.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width - 1, height - 1);
    float2 worldStep = texel * TerrainSettings.x;
    result.normal = normalize(float3(
        (TerrainHeight(uv - float2(texel.x, 0)) - TerrainHeight(uv + float2(texel.x, 0))) / (2 * worldStep.x),
        1,
        (TerrainHeight(uv - float2(0, texel.y)) - TerrainHeight(uv + float2(0, texel.y))) / (2 * worldStep.y)));
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
