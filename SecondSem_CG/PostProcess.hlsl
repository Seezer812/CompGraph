// Dedicated post-processing pass. It samples the completed lighting result and
// writes the final image to the swap-chain backbuffer.

Texture2D SceneColor : register(t0);
SamplerState LinearClampSampler : register(s0);

cbuffer PostProcessCB : register(b0)
{
    float4 InvScreen_pad;
    uint VignetteEnabled;
    float3 Padding;
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
    float3 color = SceneColor.SampleLevel(LinearClampSampler, input.uv, 0).rgb;

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
