// Lighting pass: reads G-buffer and reconstructs world position from depth.

Texture2D GAlbedo : register(t0);
Texture2D GNormal : register(t1);
Texture2D GDepth : register(t2);
Texture2D ShadowMap0 : register(t3);
Texture2D ShadowMap1 : register(t4);
Texture2D ShadowMap2 : register(t5);
Texture2D ShadowMap3 : register(t6);
TextureCube IrradianceMap : register(t7);
TextureCube PreFilteredEnvMap : register(t8);
Texture2D IntegrationMap : register(t9);
TextureCube PointShadowMap : register(t10);
Texture2D CascadeDebug0 : register(t11);
Texture2D CascadeDebug1 : register(t12);
Texture2D CascadeDebug2 : register(t13);
Texture2D CascadeDebug3 : register(t14);
SamplerState GSamp : register(s0);
SamplerComparisonState ShadowSamp : register(s1);

#define LIGHT_DIR 0
#define LIGHT_POINT 1

struct GpuLight
{
    float4 position_range;
    float4 direction_cosOuter;
    float4 color_intensity;
    uint type;
    float spotCosInner;
    uint2 pad;
};

struct PointLight
{
    float4 positionRadius;
    float4 colorIntensity;
};
StructuredBuffer<PointLight> PointLights : register(t15);
StructuredBuffer<uint2> LightTileRanges : register(t16);
StructuredBuffer<uint> LightTileIndices : register(t17);
static const uint LightTileSize = 32;

cbuffer LightingCB : register(b0)
{
    float4 CameraPos_pad;
    float4 InvScreen_pad;
    row_major float4x4 InverseViewProjection;
    float4 ShadowSettings; // x: environment max LOD, w: shadows enabled
    float4 CascadeSplits;
    row_major float4x4 CascadeMatrices[4];
    uint LightCount;
    uint TileCountX;
    uint ShadowMapDebugIndex;
    uint CascadeColorDebug;
    GpuLight Sun;
};

float SampleCascade(uint cascade, float2 uv, float depth)
{
    float result = 1.0f;
    if (cascade == 0) result = ShadowMap0.SampleCmpLevelZero(ShadowSamp, uv, depth);
    else if (cascade == 1) result = ShadowMap1.SampleCmpLevelZero(ShadowSamp, uv, depth);
    else if (cascade == 2) result = ShadowMap2.SampleCmpLevelZero(ShadowSamp, uv, depth);
    else result = ShadowMap3.SampleCmpLevelZero(ShadowSamp, uv, depth);
    return result;
}

float SampleShadowMapDepth(uint mapIndex, float2 uv)
{
    float result = 1.0f;
    if (mapIndex == 1) result = ShadowMap0.SampleLevel(GSamp, uv, 0).r;
    else if (mapIndex == 2) result = ShadowMap1.SampleLevel(GSamp, uv, 0).r;
    else if (mapIndex == 3) result = ShadowMap2.SampleLevel(GSamp, uv, 0).r;
    else result = ShadowMap3.SampleLevel(GSamp, uv, 0).r;
    return result;
}

float4 SampleCascadeDebugTexture(uint cascade, float2 uv)
{
    float4 result = 0.0f;
    if (cascade == 0) result = CascadeDebug0.SampleLevel(GSamp, uv, 0);
    else if (cascade == 1) result = CascadeDebug1.SampleLevel(GSamp, uv, 0);
    else if (cascade == 2) result = CascadeDebug2.SampleLevel(GSamp, uv, 0);
    else result = CascadeDebug3.SampleLevel(GSamp, uv, 0);
    return result;
}

float ShadowPcf(uint cascade, float3 position, float3 normal, float3 lightDirection)
{
    float4 lightClip = mul(float4(position, 1.0f), CascadeMatrices[cascade]);
    float3 uvz = lightClip.xyz / max(lightClip.w, 1e-5f);
    float2 uv = uvz.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uvz.z <= 0.0f || uvz.z >= 1.0f || any(uv < 0.0f) || any(uv > 1.0f)) return 1.0f;
    // Increased receiver bias reduces self-shadowing (shadow acne) on surfaces.
    float bias = max(0.0030f * (1.0f - saturate(dot(normal, lightDirection))), 0.0007f);
    float2 texel = 1.0f / 2048.0f;
    float visibility = 0.0f;
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x)
        visibility += SampleCascade(cascade, uv + float2(x, y) * texel * 2.0f, uvz.z - bias);
    return visibility / 25.0f;
}

float PointShadowVisibility(float3 position, float3 normal, PointLight light)
{
    float3 fromLight = position - light.positionRadius.xyz;
    float distanceToLight = length(fromLight);
    float range = light.positionRadius.w;
    if (distanceToLight <= 1e-4f || distanceToLight >= range) return 1.0f;

    float3 direction = fromLight / distanceToLight;
    float3 helper = abs(direction.y) < 0.95f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(direction, helper));
    float3 bitangent = cross(direction, tangent);
    float bias = max(0.0012f, 0.003f * (1.0f - saturate(dot(normal, -direction))));
    float referenceDistance = distanceToLight / range - bias;
    const float filterRadius = 2.0f / 1024.0f;
    float visibility = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        float3 sampleDirection = normalize(direction + (tangent * x + bitangent * y) * filterRadius);
        float nearestDistance = PointShadowMap.SampleLevel(GSamp, sampleDirection, 0).r;
        visibility += referenceDistance <= nearestDistance ? 1.0f : 0.0f;
    }
    return visibility / 9.0f;
}

struct FsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

FsOut LightingFullscreenVS(uint vertexId : SV_VertexID)
{
    FsOut output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    // G-buffer sampling uses top-left texture coordinates; restore its vertical
    // UV flip and use the matching NDC convention in ReconstructWorldPosition.
    output.uv = float2(uv.x, 1.0f - uv.y);
    return output;
}

float3 ReconstructWorldPosition(float2 uv, float depth)
{
    float4 world = mul(float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth, 1.0f), InverseViewProjection);
    return world.xyz / max(world.w, 1e-6f);
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
        lightDirection = -light.direction_cosOuter.xyz / max(length(light.direction_cosOuter.xyz), 1e-5f);
    else
    {
        float3 toLight = light.position_range.xyz - position;
        float distanceToLight = length(toLight);
        if (distanceToLight > light.position_range.w) return 0.0f;
        lightDirection = toLight / max(distanceToLight, 1e-5f);
        float falloff = 1.0f - saturate(distanceToLight / max(light.position_range.w, 1e-5f));
        attenuation = falloff * falloff;
    }
    float3 halfSum = lightDirection + viewDirection;
    float3 halfVector = halfSum * rsqrt(max(dot(halfSum, halfSum), 1e-12f));
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
    if (ShadowMapDebugIndex != 0)
    {
        const float depth = SampleShadowMapDepth(ShadowMapDebugIndex, input.uv);
        // Reversed contrast makes nearby shadow casters immediately visible.
        const float visibleDepth = pow(saturate(1.0f - depth), 0.32f);
        return float4(visibleDepth.xxx, 1.0f);
    }

    // Read matching G-buffer texels directly. Bilinear filtering blends
    // foreground depth with the background at silhouettes and destabilizes
    // reconstructed positions and cascade selection while the camera moves.
    const int2 pixel = int2(input.pos.xy);
    float4 packedAlbedo = GAlbedo.Load(int3(pixel, 0));
    float3 albedo = packedAlbedo.rgb;
    float metallic = packedAlbedo.a;
    float4 packedNormal = GNormal.Load(int3(pixel, 0));
    float3 normal = packedNormal.xyz;
    if (dot(normal, normal) < 1e-6f)
        return float4(albedo, 1.0f);

    const float roughness = clamp(packedNormal.w, 0.06f, 0.95f);

    float depth = GDepth.Load(int3(pixel, 0)).r;
    float3 position = ReconstructWorldPosition(input.uv, depth);
    normal = normalize(normal);

    // Cascades are selected by radial camera distance. In visualization mode
    // each cascade supplies the material for horizontal floor surfaces only;
    // the rest of Sponza keeps its original materials.
    float cameraDistance = length(position - CameraPos_pad.xyz);
    uint cascade = cameraDistance < CascadeSplits.x ? 0 : cameraDistance < CascadeSplits.y ? 1 : cameraDistance < CascadeSplits.z ? 2 : 3;
    if (CascadeColorDebug != 0 && abs(normal.y) > 0.82f)
    {
        const float2 floorUv = frac(position.xz * 0.08f);
        albedo = SampleCascadeDebugTexture(cascade, floorUv).rgb;
        metallic = 0.0f;
    }

    float3 toCamera = CameraPos_pad.xyz - position;
    float3 viewDirection = toCamera * rsqrt(max(dot(toCamera, toCamera), 1e-12f));
    float nDotV = saturate(dot(normal, viewDirection));
    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    float3 F = FresnelSchlickRoughness(nDotV, f0, roughness);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 irradiance = IrradianceMap.Sample(GSamp, normal).rgb;
    float3 diffuseIbl = irradiance * albedo;
    float3 reflection = reflect(-viewDirection, normal);
    float3 prefiltered = PreFilteredEnvMap.SampleLevel(GSamp, reflection, roughness * ShadowSettings.x).rgb;
    float2 brdf = IntegrationMap.Sample(GSamp, float2(nDotV, roughness)).rg;
    float3 specularIbl = prefiltered * (F * brdf.x + brdf.y);
    float3 color = kD * diffuseIbl + specularIbl;

    float3 sunDirection = normalize(-Sun.direction_cosOuter.xyz);
    float rawShadow = ShadowSettings.w > 0.5f ? ShadowPcf(cascade, position, normal, sunDirection) : 1.0f;
    if (InvScreen_pad.z > 0.5f) return float4(rawShadow.xxx, 1.0f);
    // The shadow visibility is the actual directional-light multiplier.
    // Keeping a forced 25% contribution made fully occluded regions appear lit.
    float sunShadow = rawShadow;
    color += EvaluateLight(Sun, albedo, metallic, roughness, normal, position, viewDirection) * sunShadow;

    const uint2 tile = uint2(pixel) / LightTileSize;
    const uint2 range = LightTileRanges[tile.y * TileCountX + tile.x];
    [loop] for (uint i = 0; i < range.y; ++i)
    {
        const uint lightIndex = LightTileIndices[range.x + i];
        if (lightIndex >= LightCount) continue;
        const PointLight localLight = PointLights[lightIndex];
        const float3 toLight = localLight.positionRadius.xyz - position;
        if (dot(toLight, toLight) >= localLight.positionRadius.w * localLight.positionRadius.w) continue;
        GpuLight light = (GpuLight)0;
        light.type = LIGHT_POINT;
        light.position_range = localLight.positionRadius;
        light.color_intensity = localLight.colorIntensity;
        // Only point light zero owns a shadow cubemap.
        float visibility = 1.0f;
        if (lightIndex == 0 && ShadowSettings.w > 0.5f)
            visibility = PointShadowVisibility(position, normal, localLight);
        color += EvaluateLight(light, albedo, metallic, roughness, normal, position, viewDirection) * visibility;
    }

    return float4(color, 1.0f);
}
