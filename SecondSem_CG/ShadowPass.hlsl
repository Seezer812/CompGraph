cbuffer ShadowCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 LightViewProjection;
};
struct In { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
float4 ShadowVS(In input) : SV_POSITION
{
    return mul(mul(float4(input.pos, 1.0f), World), LightViewProjection);
}
