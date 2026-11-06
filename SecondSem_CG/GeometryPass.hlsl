// Geometry pass: fills albedo, normal and depth G-buffer targets.

#include "TemporalMotion.hlsli"

cbuffer FrameCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProj;
    float4 TimeCamPos;
    float4 UvAnimAndPad;
    row_major float4x4 PreviousWorld;
};

cbuffer MatCB : register(b1)
{
    float4 Kd;
    float2 UvScale;
    float2 UvOffset;
    float3 Ks;
    float Ns;
    uint UseUvAnim;
    uint HasSpecularTex;
    uint IsEmissive;
    uint HasNormalMap;
    uint HasDisplacementMap;
    uint EnableTessellation;
    uint IsPbrMaterial;
};

Texture2D Albedo : register(t0);
Texture2D SpecMap : register(t1);
Texture2D NormalMap : register(t2);
Texture2D DisplacementMap : register(t3);
SamplerState Samp : register(s0);

struct GeoVsIn { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct GeoVsOut { float4 clipPos : SV_POSITION; float3 nrmW : NORMAL; float3 posW : POSITION0; float2 uv : TEXCOORD0; float4 currentClip : TEXCOORD1; float4 previousClip : TEXCOORD2; };

GeoVsOut GeometryVS(GeoVsIn input)
{
    GeoVsOut output;
    float4 worldPosition = mul(float4(input.pos, 1.0f), World);
    output.currentClip = mul(worldPosition, ViewProj);
    output.clipPos = ApplyTemporalJitter(output.currentClip);
    float4 previousWorldPosition = UvAnimAndPad.w != 0
        ? mul(float4(input.pos, 1.0f), PreviousWorld) : worldPosition;
    output.previousClip = mul(previousWorldPosition, PreviousViewProjection);
    output.nrmW = normalize(mul(input.normal, (float3x3)World));
    output.posW = worldPosition.xyz;
    output.uv = input.uv;
    return output;
}

struct TessFactors { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };

float EdgeTessellation(float3 a, float3 b)
{
    float distanceToCamera = length((a + b) * 0.5f - TimeCamPos.yzw);
    return lerp(5.0f, 1.0f, saturate(distanceToCamera / 14.0f));
}

TessFactors TessellationFactors(InputPatch<GeoVsOut, 3> patch)
{
    TessFactors factors;
    if (UvAnimAndPad.z == 0.0f || EnableTessellation == 0 || HasDisplacementMap == 0)
    {
        factors.edges[0] = factors.edges[1] = factors.edges[2] = factors.inside = 1.0f;
        return factors;
    }
    // Shared edges must receive the same factor from both adjacent triangles.
    factors.edges[0] = EdgeTessellation(patch[1].posW, patch[2].posW);
    factors.edges[1] = EdgeTessellation(patch[2].posW, patch[0].posW);
    factors.edges[2] = EdgeTessellation(patch[0].posW, patch[1].posW);
    factors.inside = (factors.edges[0] + factors.edges[1] + factors.edges[2]) / 3.0f;
    return factors;
}

[domain("tri")][partitioning("fractional_odd")][outputtopology("triangle_cw")]
[outputcontrolpoints(3)][patchconstantfunc("TessellationFactors")]
GeoVsOut TessellationHS(InputPatch<GeoVsOut, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
GeoVsOut TessellationDS(TessFactors factors, const OutputPatch<GeoVsOut, 3> patch, float3 bary : SV_DomainLocation)
{
    GeoVsOut output;
    output.nrmW = normalize(patch[0].nrmW * bary.x + patch[1].nrmW * bary.y + patch[2].nrmW * bary.z);
    output.uv = patch[0].uv * bary.x + patch[1].uv * bary.y + patch[2].uv * bary.z;
    output.posW = patch[0].posW * bary.x + patch[1].posW * bary.y + patch[2].posW * bary.z;
    if (EnableTessellation && HasDisplacementMap)
        output.posW += output.nrmW * ((DisplacementMap.SampleLevel(Samp, output.uv, 0).r - 0.5f) * 0.10f);
    output.currentClip = mul(float4(output.posW, 1.0f), ViewProj);
    output.clipPos = ApplyTemporalJitter(output.currentClip);
    // These domain paths currently render static transforms; their final
    // displaced position is used for camera motion.
    output.previousClip = mul(float4(output.posW, 1.0f), PreviousViewProjection);
    return output;
}

// Separate quad-domain path for the animated procedural wall.
struct QuadTessFactors { float edges[4] : SV_TessFactor; float inside[2] : SV_InsideTessFactor; };

QuadTessFactors WaveWallFactors(InputPatch<GeoVsOut, 4> patch)
{
    QuadTessFactors factors;
    factors.edges[0] = factors.edges[1] = factors.edges[2] = factors.edges[3] = 16.0f;
    factors.inside[0] = factors.inside[1] = 16.0f;
    return factors;
}

[domain("quad")][partitioning("fractional_even")][outputtopology("triangle_cw")]
[outputcontrolpoints(4)][patchconstantfunc("WaveWallFactors")]
GeoVsOut WaveWallHS(InputPatch<GeoVsOut, 4> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("quad")]
GeoVsOut WaveWallDS(QuadTessFactors factors, const OutputPatch<GeoVsOut, 4> patch, float2 domainUv : SV_DomainLocation)
{
    GeoVsOut output;
    output.posW = lerp(lerp(patch[0].posW, patch[1].posW, domainUv.x), lerp(patch[2].posW, patch[3].posW, domainUv.x), domainUv.y);
    output.uv = lerp(lerp(patch[0].uv, patch[1].uv, domainUv.x), lerp(patch[2].uv, patch[3].uv, domainUv.x), domainUv.y);

    // The sine wave travels over the wall and shifts it along the Z normal.
    const float phase = output.posW.x * 2.1f + output.posW.y * 1.35f + TimeCamPos.x * 2.4f;
    const float amplitude = 0.22f;
    output.posW.z += amplitude * sin(phase);
    output.nrmW = normalize(float3(-amplitude * 2.1f * cos(phase), -amplitude * 1.35f * cos(phase), 1.0f));
    output.currentClip = mul(float4(output.posW, 1.0f), ViewProj);
    output.clipPos = ApplyTemporalJitter(output.currentClip);
    // These domain paths currently render static transforms; their final
    // displaced position is used for camera motion.
    output.previousClip = mul(float4(output.posW, 1.0f), PreviousViewProjection);
    return output;
}

struct GeoRtOut { float4 albedo : SV_Target0; float4 normal : SV_Target1; float depth : SV_Target2; float2 motion : SV_Target3; };

GeoRtOut GeometryPS(GeoVsOut input)
{
    GeoRtOut output;
    output.motion = TemporalMotionUV(input.currentClip, input.previousClip);
    float2 uv = input.uv * UvScale + UvOffset;
    if (UseUvAnim)
        uv += UvAnimAndPad.xy * TimeCamPos.x;

    // Albedo has an sRGB SRV: hardware decodes before bilinear filtering.
    const float3 baseColor = Albedo.Sample(Samp, uv).rgb * Kd.rgb;
    if (IsEmissive != 0)
    {
        output.albedo = float4(baseColor, 0.0f);
        output.normal = 0.0f; // Lighting pass treats this as unlit colour.
        output.depth = input.clipPos.z;
        return output;
    }
    // OBJ/MTL is a specular-gloss workflow.  Convert its shininess to the
    // perceptually linear roughness needed by the metallic-roughness BRDF.
    // Sponza has no reliable metalness map, so coloured, very strong specular
    // materials are the only ones treated as metals.
    const float specularStrength = HasSpecularTex ? dot(SpecMap.Sample(Samp, uv).rgb * Ks, 1.0f / 3.0f) : dot(Ks, 1.0f / 3.0f);
    const float metallic = IsPbrMaterial ? SpecMap.Sample(Samp, uv).r : saturate((specularStrength - 0.55f) * 2.2f);
    const float roughness = IsPbrMaterial ? clamp(DisplacementMap.Sample(Samp, uv).r, 0.06f, 0.95f) :
        clamp(sqrt(2.0f / (Ns + 2.0f)), 0.06f, 0.95f);
    output.albedo = float4(baseColor, metallic);
    float3 normalW = normalize(input.nrmW);
    if (HasNormalMap)
    {
        float3 dpdx = ddx(input.posW), dpdy = ddy(input.posW);
        float2 duvdx = ddx(input.uv), duvdy = ddy(input.uv);
        float3 dpdyPerp = cross(dpdy, normalW), dpdxPerp = cross(normalW, dpdx);
        float3 tangent = dpdyPerp * duvdx.x + dpdxPerp * duvdy.x;
        float3 bitangent = dpdyPerp * duvdx.y + dpdxPerp * duvdy.y;
        float scale = rsqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-12f));
        float3 normalT = NormalMap.Sample(Samp, uv).xyz * 2.0f - 1.0f;
        float3 mapped = tangent * (normalT.x * scale) + bitangent * (normalT.y * scale) + normalW * normalT.z;
        if (dot(mapped, mapped) > 1e-12f) normalW = normalize(mapped);
    }
    output.normal = float4(normalW, roughness);
    // In a pixel shader SV_POSITION.z is already the post-projection depth
    // in the [0, 1] range. SV_POSITION.w is reciprocal clip-space W here;
    // dividing by it again corrupts the depth used to reconstruct world space.
    output.depth = input.clipPos.z;
    return output;
}

// Forward-шейдер второго viewport. Он выводит полноценную геометрию сцены
// прямо в back buffer, не изменяя основной G-buffer.
float4 TopCameraPS(GeoVsOut input) : SV_Target
{
    float2 uv = input.uv * UvScale + UvOffset;
    if (UseUvAnim)
        uv += UvAnimAndPad.xy * TimeCamPos.x;
    const float3 albedo = Albedo.Sample(Samp, uv).rgb * Kd.rgb;
    const float light = 0.30f + 0.70f * saturate(dot(normalize(input.nrmW), normalize(float3(0.35f, 0.8f, -0.25f))));
    return float4(albedo * light, 1.0f);
}
