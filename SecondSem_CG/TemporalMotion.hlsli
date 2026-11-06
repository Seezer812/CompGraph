// Geometry paths share one camera contract. Jitter changes coverage only.
cbuffer TemporalCamera : register(b2)
{
    row_major float4x4 PreviousViewProjection;
    float2 JitterNdc;
};

float4 ApplyTemporalJitter(float4 clip)
{
    clip.xy += JitterNdc * clip.w;
    return clip;
}

float2 TemporalMotionUV(float4 currentClip, float4 previousClip)
{
    // A point behind the previous camera has no usable history.
    if (previousClip.w <= 1e-5f)
        return float2(4, 4);

    const float2 currentNdc = currentClip.xy / currentClip.w;
    const float2 previousNdc = previousClip.xy / previousClip.w;
    return (currentNdc - previousNdc) * float2(0.5f, -0.5f);
}
