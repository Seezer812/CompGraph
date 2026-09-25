cbuffer ShadowCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 LightViewProjection;
    float4 LightPositionRange;
};
struct In { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
float4 ShadowVS(In input) : SV_POSITION
{
    return mul(mul(float4(input.pos, 1.0f), World), LightViewProjection);
}

struct PointShadowOut
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
};

PointShadowOut PointShadowVS(In input)
{
    PointShadowOut output;
    float4 world = mul(float4(input.pos, 1.0f), World);
    output.position = mul(world, LightViewProjection);
    output.worldPosition = world.xyz;
    return output;
}

float PointShadowPS(PointShadowOut input) : SV_TARGET0
{
    return saturate(length(input.worldPosition - LightPositionRange.xyz) / max(LightPositionRange.w, 1e-4f));
}
