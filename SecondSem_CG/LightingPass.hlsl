// Lighting pass: reads G-buffer and reconstructs world position from depth.

Texture2D GAlbedo : register(t0);
Texture2D GNormal : register(t1);
Texture2D GDepth : register(t2);
Texture2D ShadowMaps[4] : register(t3);
Texture2D AmbientOcclusion : register(t7);
TextureCube IrradianceMap : register(t8);
TextureCube PreFilteredEnvMap : register(t9);
Texture2D IntegrationMap : register(t10);
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

// SSAO is a separate full-screen post-process. It reconstructs world-space
// positions from the G-buffer and writes one accessibility value per pixel.
// A deterministic per-pixel rotation avoids visible radial banding without a
// separate noise texture.
FsOut SsaoFullscreenVS(uint vertexId : SV_VertexID)
{
    return LightingFullscreenVS(vertexId);
}

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 world = mul(float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f), InverseViewProjection);
    return world.xyz / max(world.w, 1e-6f);
}

float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

float4 SsaoPS(FsOut input) : SV_Target0
{
    float3 normal = GNormal.Sample(GSamp, input.uv).xyz;
    if (dot(normal, normal) < 1e-6f)
        return 1.0f;

    const float centerDepth = GDepth.Sample(GSamp, input.uv).r;
    const float3 center = ReconstructWorldPosition(input.uv, centerDepth);
    normal = normalize(normal);
    const float angle = Hash12(input.uv * float2(InvScreen_pad.x > 0 ? 1.0f / InvScreen_pad.x : 1.0f,
                                                  InvScreen_pad.y > 0 ? 1.0f / InvScreen_pad.y : 1.0f)) * 6.2831853f;
    const float2 rotation = float2(cos(angle), sin(angle));
    const float2 texel = InvScreen_pad.xy;
    const float radiusPixels = 18.0f;
    const float radiusWorld = 0.75f;
    float occlusion = 0.0f;

    [unroll] for (int i = 0; i < 12; ++i)
    {
        const float a = (6.2831853f * i) / 12.0f;
        const float2 direction = float2(cos(a) * rotation.x - sin(a) * rotation.y,
                                        cos(a) * rotation.y + sin(a) * rotation.x);
        // Alternating rings cover both tiny cracks and larger corners.
        const float ring = 0.35f + 0.65f * frac(i * 0.6180339f + 0.25f);
        const float2 sampleUv = input.uv + direction * texel * radiusPixels * ring;
        const float sampleDepth = GDepth.SampleLevel(GSamp, sampleUv, 0).r;
        const float3 samplePosition = ReconstructWorldPosition(sampleUv, sampleDepth);
        const float3 delta = samplePosition - center;
        const float distanceToSample = length(delta);
        const float facing = dot(normal, delta / max(distanceToSample, 1e-4f));
        // Only a nearby surface lying in the outward normal hemisphere can
        // block ambient light at the current surface point.
        const float nearby = 1.0f - smoothstep(radiusWorld * 0.45f, radiusWorld, distanceToSample);
        occlusion += step(0.055f, facing) * nearby;
    }
    const float accessibility = 1.0f - (occlusion / 12.0f) * 0.78f;
    return float4(saturate(accessibility).xxx, 1.0f);
}

static const float PI = 3.14159265359f;

float DistributionGGX(float3 n, float3 h, float roughness)
{
    float a = roughness * roughness, a2 = a * a;
    float nDotH = saturate(dot(n, h));
    float denom = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denom * denom, 1e-5f);
}

float GeometrySchlickGGX(float nDotX, float roughness)
{
    float r = roughness + 1.0f, k = (r * r) / 8.0f;
    return nDotX / max(nDotX * (1.0f - k) + k, 1e-5f);
}

float GeometrySmith(float3 n, float3 v, float3 l, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(n, v)), roughness) * GeometrySchlickGGX(saturate(dot(n, l)), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max(1.0f - roughness, f0) - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 EvaluateLight(GpuLight light, float3 albedo, float metallic, float roughness, float3 normal, float3 position, float3 viewDirection)
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
    float3 halfVector = normalize(lightDirection + viewDirection);
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotL <= 0.0f) return 0.0f;
    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 F = FresnelSchlick(saturate(dot(halfVector, viewDirection)), f0);
    float D = DistributionGGX(normal, halfVector, roughness);
    float G = GeometrySmith(normal, viewDirection, lightDirection, roughness);
    float3 specular = (D * G * F) / max(4.0f * saturate(dot(normal, viewDirection)) * nDotL, 1e-4f);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 radiance = light.color_intensity.xyz * light.color_intensity.w * attenuation;
    return (kD * albedo / PI + specular) * radiance * nDotL;
}

float4 LightingPS(FsOut input) : SV_Target0
{
    float4 packedAlbedo = GAlbedo.Sample(GSamp, input.uv);
    float3 albedo = packedAlbedo.rgb;
    float metallic = packedAlbedo.a;
    float4 packedNormal = GNormal.Sample(GSamp, input.uv);
    float3 normal = packedNormal.xyz;
    if (dot(normal, normal) < 1e-6f)
        return float4(albedo, 1.0f);

    // SSAO affects only the indirect/ambient term. Direct point, spot and sun
    // lighting remains bright even where a nearby corner blocks skylight.
    // Full-screen target coordinates are vertically opposite to the G-buffer
    // convention used by the existing lighting pass, so compensate here.
    const float rawAo = AmbientOcclusion.Sample(GSamp, float2(input.uv.x, 1.0f - input.uv.y)).r;
    if (InvScreen_pad.w > 0.5f)
        return float4(rawAo.xxx, 1.0f);
    const float ao = CameraPos_pad.w > 0.5f ? rawAo : 1.0f;
    const float roughness = clamp(packedNormal.w, 0.06f, 0.95f);

    float depth = GDepth.Sample(GSamp, input.uv).r;
    float3 position = ReconstructWorldPosition(input.uv, depth);
    normal = normalize(normal);
    float3 viewDirection = normalize(CameraPos_pad.xyz - position);
    float nDotV = saturate(dot(normal, viewDirection));
    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 F = FresnelSchlickRoughness(nDotV, f0, roughness);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 irradiance = IrradianceMap.Sample(GSamp, normal).rgb;
    float3 diffuseIbl = irradiance * albedo;
    float3 reflection = reflect(-viewDirection, normal);
    float3 prefiltered = PreFilteredEnvMap.SampleLevel(GSamp, reflection, roughness * 11.0f).rgb;
    float2 brdf = IntegrationMap.Sample(GSamp, float2(nDotV, roughness)).rg;
    float3 specularIbl = prefiltered * (F * brdf.x + brdf.y);
    float3 color = (kD * diffuseIbl + specularIbl) * ao;

    // Cascade is selected by camera-space distance. Splits are nonlinear: most
    // shadow-map precision stays near the viewer.
    float viewDepth = max(0.0f, dot(position - CameraPos_pad.xyz, CameraForward_shadowEnabled.xyz));
    uint cascade = viewDepth < CascadeSplits.x ? 0 : viewDepth < CascadeSplits.y ? 1 : viewDepth < CascadeSplits.z ? 2 : 3;
    float3 sunDirection = normalize(-Lights[2].direction_cosOuter.xyz);
    float rawShadow = ShadowPcf(cascade, position, normal, sunDirection);
    if (InvScreen_pad.z > 0.5f) return float4(rawShadow.xxx, 1.0f);
    float sunShadow = CameraForward_shadowEnabled.w > 0.5f ? lerp(0.25f, 1.0f, rawShadow) : 1.0f;
    color += EvaluateLight(Lights[0], albedo, metallic, roughness, normal, position, viewDirection);
    color += EvaluateLight(Lights[1], albedo, metallic, roughness, normal, position, viewDirection);
    color += EvaluateLight(Lights[2], albedo, metallic, roughness, normal, position, viewDirection) * sunShadow;

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
            color += EvaluateLight(Lights[lightIndex], albedo, metallic, roughness, normal, position, viewDirection);
        }
    }
    return float4(color, 1.0f);
}
