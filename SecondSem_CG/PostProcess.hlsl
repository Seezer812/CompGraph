// Dedicated post-processing pass. It samples the completed lighting result and
// writes the final image to the swap-chain backbuffer.

Texture2D SceneColor : register(t0);
SamplerState LinearClampSampler : register(s0);

cbuffer PostProcessCB : register(b0)
{
    float4 InvScreen_pad;
    uint VignetteEnabled;
    uint FlyEyeEnabled;
    float2 Padding;
};

struct FsOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FsOut PostProcessFullscreenVS(uint vertexId : SV_VertexID)
{
    FsOut output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    // Unlike the deferred pass, this is a straight copy from the completed
    // render target, so texture coordinates retain the backbuffer orientation.
    output.uv = uv;
    return output;
}

float4 PostProcessPS(FsOut input) : SV_Target0
{
    float2 sceneUv = input.uv;
    float facetMask = 1.0f;

    if (FlyEyeEnabled != 0)
    {
        // Pointy-top hexagonal grid in pixel space.  Axial cube rounding finds
        // the nearest facet centre without discontinuities between rows.
        const float hexRadius = 82.0f;
        const float sqrt3 = 1.73205080757f;
        float2 pixel = input.uv / InvScreen_pad.xy;
        float axialR = (2.0f / 3.0f) * pixel.y / hexRadius;
        float axialQ = (sqrt3 / 3.0f * pixel.x - pixel.y / 3.0f) / hexRadius;

        float3 cube = float3(axialQ, -axialQ - axialR, axialR);
        float3 roundedCube = round(cube);
        float3 cubeError = abs(roundedCube - cube);
        if (cubeError.x > cubeError.y && cubeError.x > cubeError.z)
            roundedCube.x = -roundedCube.y - roundedCube.z;
        else if (cubeError.y > cubeError.z)
            roundedCube.y = -roundedCube.x - roundedCube.z;
        else
            roundedCube.z = -roundedCube.x - roundedCube.y;

        float2 center = float2(
            hexRadius * sqrt3 * (roundedCube.x + 0.5f * roundedCube.z),
            hexRadius * 1.5f * roundedCube.z);
        float2 local = pixel - center;

        // Every facet maps its complete bounding box back to the full scene.
        sceneUv = float2(
            local.x / (sqrt3 * hexRadius) + 0.5f,
            local.y / (2.0f * hexRadius) + 0.5f);
        sceneUv = saturate(sceneUv);

        // Exact pointy-top regular hexagon: one pair of vertical sides and
        // two pairs of diagonal sides.  Both terms equal one on the border.
        float2 absoluteLocal = abs(local);
        float verticalSides = absoluteLocal.x / (0.5f * sqrt3 * hexRadius);
        float diagonalSides = absoluteLocal.x / (sqrt3 * hexRadius)
            + absoluteLocal.y / hexRadius;
        float hexDistance = max(verticalSides, diagonalSides);

        // Keep a visible black seam, with derivative-based antialiasing on
        // every one of the six straight edges.
        float borderWidth = max(fwidth(hexDistance) * 1.5f, 0.012f);
        facetMask = 1.0f - smoothstep(0.92f - borderWidth, 0.92f, hexDistance);
    }

    float3 color = SceneColor.SampleLevel(LinearClampSampler, sceneUv, 0).rgb;
    color *= facetMask;

    if (VignetteEnabled != 0)
    {
        // Account for the aspect ratio so the vignette remains circular.
        float2 centeredUv = (input.uv - 0.5f) * 2.0f;
        centeredUv.x *= InvScreen_pad.y / max(InvScreen_pad.x, 1e-5f);
        const float distanceFromCenter = length(centeredUv);
        const float edgeDarkening = smoothstep(0.58f, 1.35f, distanceFromCenter);
        color *= 1.0f - edgeDarkening * 0.45f;
    }

    return float4(color, 1.0f);
}
