// Shaders.hlsl

cbuffer PerObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldViewProj;

    float3 gLightPosW;
    float  pad0;

    float3 gEyePosW;
    float  pad1;

    float4 gDiffuseColor;
    float4 gSpecColorPower;

    float  gUVOffsetX;
    float  gUVOffsetY;
    float  gUVTileX;
    float  gUVTileY;

    int    gUseTexture;
    float3 pad2;
};

Texture2D    gDiffuseMap : register(t0);
SamplerState gSampler    : register(s0);

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
    float4 Color   : COLOR;
};

struct VertexOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
    float4 Color   : COLOR;
};

VertexOut VSMain(VertexIn vin)
{
    VertexOut vout;

    vout.PosH   = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.PosW   = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));

    float2 uv;
    uv.x = vin.TexC.x * gUVTileX + gUVOffsetX;
    uv.y = vin.TexC.y * gUVTileY + gUVOffsetY;
    vout.TexC = uv;

    vout.Color = vin.Color;

    return vout;
}

float4 PSMain(VertexOut pin) : SV_TARGET
{
    float3 normal   = normalize(pin.NormalW);
    float3 lightDir = normalize(gLightPosW - pin.PosW);
    float3 viewDir  = normalize(gEyePosW   - pin.PosW);
    float3 halfVec  = normalize(lightDir + viewDir);

    float4 baseColor;
    if (gUseTexture)
    {
        baseColor = gDiffuseMap.Sample(gSampler, pin.TexC);
    }
    else
    {
        baseColor = pin.Color * gDiffuseColor;
    }

    float  diff    = max(dot(normal, lightDir), 0.0f);
    float3 diffuse = diff * baseColor.rgb;

    float  spec    = pow(max(dot(normal, halfVec), 0.0f), gSpecColorPower.w);
    float3 specular = spec * gSpecColorPower.xyz;

    float3 ambient = 0.15f * baseColor.rgb;

    float3 finalColor = ambient + diffuse + specular;

    return float4(finalColor, baseColor.a);
}
