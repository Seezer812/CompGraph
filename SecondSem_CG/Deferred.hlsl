// Этап геометрии отложенного рендера (заполнение G-buffer) и полноэкранный проход света.

cbuffer FrameCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProj;
    float4 TimeCamPos;
    float4 UvAnimAndPad;
    float _PadRest[24];
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
    float _PadMat[46];
};

Texture2D Albedo : register(t0);
Texture2D SpecMap : register(t1);
Texture2D NormalMap : register(t2);
Texture2D DisplacementMap : register(t3);
SamplerState Samp : register(s0);

struct GeoVsIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct GeoVsOut
{
    float4 clipPos : SV_POSITION;
    float3 nrmW : NORMAL;
    float3 posW : POSITION0;
    float2 uv : TEXCOORD0;
};

GeoVsOut GeometryVS(GeoVsIn input)
{
    GeoVsOut o;
    float4 wpos = mul(float4(input.pos, 1.0f), World);
    o.clipPos = mul(wpos, ViewProj);
    float3x3 W = (float3x3)World;
    o.nrmW = normalize(mul(input.normal, W));
    o.posW = wpos.xyz;
    o.uv = input.uv;
    return o;
}

struct TessFactors
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

TessFactors TessellationFactors(InputPatch<GeoVsOut, 3> patch)
{
    TessFactors f;
    if (UvAnimAndPad.z == 0.0f || EnableTessellation == 0 || HasDisplacementMap == 0)
    {
        f.edges[0] = f.edges[1] = f.edges[2] = f.inside = 1.0f;
        return f;
    }
    float3 center = (patch[0].posW + patch[1].posW + patch[2].posW) / 3.0f;
    float distanceToCamera = length(center - TimeCamPos.yzw);
    float level = lerp(6.0f, 1.0f, saturate(distanceToCamera / 20.0f));
    f.edges[0] = f.edges[1] = f.edges[2] = f.inside = level;
    return f;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("TessellationFactors")]
GeoVsOut TessellationHS(InputPatch<GeoVsOut, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
GeoVsOut TessellationDS(TessFactors f, const OutputPatch<GeoVsOut, 3> patch, float3 bary : SV_DomainLocation)
{
    GeoVsOut o;
    o.nrmW = normalize(patch[0].nrmW * bary.x + patch[1].nrmW * bary.y + patch[2].nrmW * bary.z);
    o.uv = patch[0].uv * bary.x + patch[1].uv * bary.y + patch[2].uv * bary.z;
    o.posW = patch[0].posW * bary.x + patch[1].posW * bary.y + patch[2].posW * bary.z;
    if (EnableTessellation != 0 && HasDisplacementMap != 0)
        o.posW += o.nrmW * ((DisplacementMap.SampleLevel(Samp, o.uv, 0).r - 0.5f) * 0.10f);
    o.clipPos = mul(float4(o.posW, 1.0f), ViewProj);
    return o;
}

struct GeoRtOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
};

GeoRtOut GeometryPS(GeoVsOut input)
{
    GeoRtOut o;
    float time = TimeCamPos.x;
    float2 uv = input.uv * UvScale + UvOffset;
    if (UseUvAnim != 0)
        uv += UvAnimAndPad.xy * time;
    float3 a = Albedo.Sample(Samp, uv).rgb * Kd.rgb;
    o.albedo = float4(a, 1);
    float3 normalW = normalize(input.nrmW);
    if (HasNormalMap != 0)
    {
        float3 dp1 = ddx(input.posW);
        float3 dp2 = ddy(input.posW);
        float2 duv1 = ddx(input.uv);
        float2 duv2 = ddy(input.uv);
        float3 tangent = normalize(dp1 * duv2.y - dp2 * duv1.y);
        float3 bitangent = normalize(cross(normalW, tangent));
        float3 normalT = NormalMap.Sample(Samp, uv).xyz * 2.0f - 1.0f;
        normalW = normalize(tangent * normalT.x + bitangent * normalT.y + normalW * normalT.z);
    }
    o.normal = float4(normalW, IsEmissive);
    o.position = float4(input.posW, 1);
    return o;
}

// --- Lighting pass ---

Texture2D GAlbedo : register(t0);
Texture2D GNormal : register(t1);
Texture2D GPos : register(t2);
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
    uint LightCount;
    uint3 padHdr;
    GpuLight Lights[MAX_LIGHTS];
    uint4 RainTileCounts[8];
    uint4 RainTileLightIndices[120];
};

struct FsOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FsOut LightingFullscreenVS(uint vid : SV_VertexID)
{
    FsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
    o.uv = float2(uv.x, 1.f - uv.y);
    return o;
}

float3 EvaluateLight(GpuLight Lg, float3 alb, float3 N, float3 P, float3 V)
{
    float3 Lc = Lg.color_intensity.xyz;
    float I = Lg.color_intensity.w;
    float3 Ldir = float3(0, 1, 0);
    float att = 1.f;

    if (Lg.type == LIGHT_DIR)
    {
        Ldir = normalize(-Lg.direction_cosOuter.xyz);
    }
    else if (Lg.type == LIGHT_POINT)
    {
        float3 toL = Lg.position_range.xyz - P;
        float dist = length(toL);
        if (dist > Lg.position_range.w)
            return 0.f;
        Ldir = toL / max(dist, 1e-5);
        float t = 1.f - saturate(dist / Lg.position_range.w);
        att = t * t;
    }
    else
    {
        float3 toL = Lg.position_range.xyz - P;
        float dist = length(toL);
        if (dist > Lg.position_range.w)
            return 0.f;
        Ldir = toL / max(dist, 1e-5);
        float t = 1.f - saturate(dist / Lg.position_range.w);
        att = t * t;
        float3 axis = normalize(Lg.direction_cosOuter.xyz);
        float rho = dot(-Ldir, axis);
        float spot = saturate((rho - Lg.direction_cosOuter.w) / max(Lg.spotCosInner - Lg.direction_cosOuter.w, 1e-4));
        att *= spot * spot;
    }

    float diff = saturate(dot(N, Ldir));
    float3 H = normalize(Ldir + V);
    float spec = pow(saturate(dot(N, H)), 48.f) * 0.28f;
    return (alb * diff + spec) * Lc * I * att;
}

float4 LightingPS(FsOut pin) : SV_Target0
{
    float3 alb = GAlbedo.Sample(GSamp, pin.uv).rgb;
    float4 packedNormal = GNormal.Sample(GSamp, pin.uv);
    float3 N = packedNormal.xyz;
    float3 P = GPos.Sample(GSamp, pin.uv).xyz;

    float3 color = alb * (0.035f + packedNormal.w * 2.5f);

    if (dot(N, N) < 1e-6f)
        return float4(color, 1.f);

    N = normalize(N);
    float3 V = normalize(CameraPos_pad.xyz - P);

    // Постоянные: фонарь камеры, одиночный точечный и направленный свет.
    [unroll]
    for (uint i = 0; i < 3; ++i)
        color += EvaluateLight(Lights[i], alb, N, P, V);

    // Пространственная сетка дождя: текущая ячейка мира и восемь соседних.
    int centerX = clamp((int)floor((P.x + 6.f) / 2.f), 0, 5);
    int centerZ = clamp((int)floor((P.z + 5.f) / 2.f), 0, 4);
    [unroll]
    for (int dz = -1; dz <= 1; ++dz)
    {
        const int z = centerZ + dz;
        if (z < 0 || z >= 5) continue;
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int x = centerX + dx;
            if (x < 0 || x >= 6) continue;
            const uint tile = z * 6 + x;
            const uint count = RainTileCounts[tile / 4][tile % 4];
            [loop]
            for (uint j = 0; j < count; ++j)
            {
                const uint index = tile * 16 + j;
                color += EvaluateLight(Lights[RainTileLightIndices[index / 4][index % 4]], alb, N, P, V);
            }
        }
    }
    return float4(color, 1.f);
}
