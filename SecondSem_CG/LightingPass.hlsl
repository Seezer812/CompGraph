// Lighting pass: reads G-buffer and reconstructs world position from depth.

Texture2D GAlbedo : register(t0);
Texture2D GNormal : register(t1);
Texture2D GDepth : register(t2);
SamplerState GSamp : register(s0);

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
    uint LightCount;
    uint3 padHdr;
    GpuLight Lights[MAX_LIGHTS];
    uint4 RainTileCounts[8];
    uint4 RainTileLightIndices[120];
};

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

    [unroll] for (uint i = 0; i < 3; ++i)
        color += EvaluateLight(Lights[i], albedo, normal, position, viewDirection);

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
