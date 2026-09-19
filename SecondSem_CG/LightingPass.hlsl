// Lighting pass: reads G-buffer and reconstructs world position from depth.

Texture2D GAlbedo : register(t0);
Texture2D GNormal : register(t1);
Texture2D GDepth : register(t2);
Texture2D ShadowMaps[4] : register(t3);
SamplerState GSamp : register(s0);
SamplerComparisonState ShadowSamp : register(s1);

#define LIGHT_DIR 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2
#define MAX_LIGHTS 128

struct GpuLight
{
    float4 position_range;
    float4 direction_cosOuter;
    float4 color_intensity;
    uint type;
    float spotCosInner;
    uint2 pad;
};

cbuffer LightingCB : register(b0)
{
    float4 CameraPos_pad;
    float4 InvScreen_pad;
    row_major float4x4 InverseViewProjection;
    float4 CameraForward_shadowEnabled;
    float4 CascadeSplits;
    row_major float4x4 CascadeMatrices[4];
    uint LightCount;
    uint3 padHdr;
    GpuLight Lights[MAX_LIGHTS];
    uint4 RainTileCounts[8];
    uint4 RainTileLightIndices[120];
};

float SampleCascade(uint cascade, float2 uv, float depth)
{
    // Shader Model 5 requires a literal texture-array index at SampleCmp.
    if (cascade == 0) return ShadowMaps[0].SampleCmpLevelZero(ShadowSamp, uv, depth);
    if (cascade == 1) return ShadowMaps[1].SampleCmpLevelZero(ShadowSamp, uv, depth);
    if (cascade == 2) return ShadowMaps[2].SampleCmpLevelZero(ShadowSamp, uv, depth);
    return ShadowMaps[3].SampleCmpLevelZero(ShadowSamp, uv, depth);
}

float ShadowPcf(uint cascade, float3 position, float3 normal, float3 lightDirection)
{
    float4 lightClip = mul(float4(position, 1.0f), CascadeMatrices[cascade]);
    float3 uvz = lightClip.xyz / max(lightClip.w, 1e-5f);
    float2 uv = uvz.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uvz.z <= 0.0f || uvz.z >= 1.0f || any(uv < 0.0f) || any(uv > 1.0f)) return 1.0f;
    // The Sponza walls contain very dense coplanar details; a larger slope bias
    // prevents the surface from shadowing itself (shadow acne).
    float bias = max(0.0040f * (1.0f - saturate(dot(normal, lightDirection))), 0.0015f);
    float2 texel = 1.0f / 2048.0f;
    float visibility = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        visibility += SampleCascade(cascade, uv + float2(x, y) * texel, uvz.z - bias);
    return visibility / 9.0f;
}

struct FsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

FsOut LightingFullscreenVS(uint vertexId : SV_VertexID)
{
    FsOut output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = float2(uv.x, 1.0f - uv.y);
    return output;
}

float3 EvaluateLight(GpuLight light, float3 albedo, float3 normal, float3 position, float3 viewDirection)
{
    float3 lightDirection;
    float attenuation = 1.0f;
    if (light.type == LIGHT_DIR)
        lightDirection = normalize(-light.direction_cosOuter.xyz);
    else
    {
        float3 toLight = light.position_range.xyz - position;
        float distanceToLight = length(toLight);
        if (distanceToLight > light.position_range.w) return 0.0f;
        lightDirection = toLight / max(distanceToLight, 1e-5f);
        float falloff = 1.0f - saturate(distanceToLight / light.position_range.w);
        attenuation = falloff * falloff;
        if (light.type == LIGHT_SPOT)
        {
            float rho = dot(-lightDirection, normalize(light.direction_cosOuter.xyz));
            float spot = saturate((rho - light.direction_cosOuter.w) / max(light.spotCosInner - light.direction_cosOuter.w, 1e-4f));
            attenuation *= spot * spot;
        }
    }
    float diffuse = saturate(dot(normal, lightDirection));
    float specular = pow(saturate(dot(normal, normalize(lightDirection + viewDirection))), 48.0f) * 0.28f;
    return (albedo * diffuse + specular) * light.color_intensity.xyz * light.color_intensity.w * attenuation;
}

float4 LightingPS(FsOut input) : SV_Target0
{
    float3 albedo = GAlbedo.Sample(GSamp, input.uv).rgb;
    float4 packedNormal = GNormal.Sample(GSamp, input.uv);
    float3 normal = packedNormal.xyz;
    float3 color = albedo * (0.035f + packedNormal.w);
    if (dot(normal, normal) < 1e-6f) return float4(color, 1.0f);

    float depth = GDepth.Sample(GSamp, input.uv).r;
    float4 world = mul(float4(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f, depth, 1.0f), InverseViewProjection);
    float3 position = world.xyz / max(world.w, 1e-6f);
    normal = normalize(normal);
    float3 viewDirection = normalize(CameraPos_pad.xyz - position);

    // Cascade is selected by camera-space distance. Splits are nonlinear: most
    // shadow-map precision stays near the viewer.
    float viewDepth = max(0.0f, dot(position - CameraPos_pad.xyz, CameraForward_shadowEnabled.xyz));
    uint cascade = viewDepth < CascadeSplits.x ? 0 : viewDepth < CascadeSplits.y ? 1 : viewDepth < CascadeSplits.z ? 2 : 3;
    float3 sunDirection = normalize(-Lights[2].direction_cosOuter.xyz);
    float rawShadow = ShadowPcf(cascade, position, normal, sunDirection);
    if (InvScreen_pad.z > 0.5f) return float4(rawShadow.xxx, 1.0f);
    float sunShadow = CameraForward_shadowEnabled.w > 0.5f ? lerp(0.25f, 1.0f, rawShadow) : 1.0f;
    color += EvaluateLight(Lights[0], albedo, normal, position, viewDirection);
    color += EvaluateLight(Lights[1], albedo, normal, position, viewDirection);
    color += EvaluateLight(Lights[2], albedo, normal, position, viewDirection) * sunShadow;

    int centerX = clamp((int)floor((position.x + 6.0f) / 2.0f), 0, 5);
    int centerZ = clamp((int)floor((position.z + 5.0f) / 2.0f), 0, 4);
    [unroll] for (int zOffset = -1; zOffset <= 1; ++zOffset)
    [unroll] for (int xOffset = -1; xOffset <= 1; ++xOffset)
    {
        int x = centerX + xOffset, z = centerZ + zOffset;
        if (x < 0 || x >= 6 || z < 0 || z >= 5) continue;
        uint tile = z * 6 + x;
        uint count = RainTileCounts[tile / 4][tile % 4];
        [loop] for (uint index = 0; index < count; ++index)
        {
            uint lightIndex = RainTileLightIndices[(tile * 16 + index) / 4][(tile * 16 + index) % 4];
            color += EvaluateLight(Lights[lightIndex], albedo, normal, position, viewDirection);
        }
    }
    return float4(color, 1.0f);
}
