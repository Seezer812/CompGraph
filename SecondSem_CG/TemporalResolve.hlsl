Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> HistoryColor : register(t1);
Texture2D<float2> Motion : register(t2);
SamplerState LinearClamp : register(s0);

cbuffer ResolveSettings : register(b0)
{
    uint HistoryValid;
    uint Reproject;
    uint ClampHistory;
    float CurrentWeight;
};

float4 TemporalVS(uint vertexId : SV_VertexID) : SV_POSITION
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 ClampToNeighborhood(float3 history, int2 pixel, int2 size)
{
    float3 lower = CurrentColor.Load(int3(pixel, 0)).rgb;
    float3 upper = lower;
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            int2 neighbor = clamp(pixel + int2(x, y), 0, size - 1);
            float3 color = CurrentColor.Load(int3(neighbor, 0)).rgb;
            lower = min(lower, color);
            upper = max(upper, color);
        }
    }
    return clamp(history, lower, upper);
}

float4 TemporalPS(float4 position : SV_POSITION) : SV_TARGET
{
    const int2 pixel = int2(position.xy);
    const float4 current = CurrentColor.Load(int3(pixel, 0));
    if (!HistoryValid)
        return current;

    uint width, height;
    CurrentColor.GetDimensions(width, height);
    const int2 size = int2(width, height);
    const float2 uv = position.xy / float2(size);
    const float2 motion = Reproject ? Motion.Load(int3(pixel, 0)) : 0;
    const float2 historyUv = uv - motion;
    if (any(historyUv < 0) || any(historyUv >= 1))
        return current;

    float3 history = HistoryColor.SampleLevel(LinearClamp, historyUv, 0).rgb;
    if (ClampHistory)
        history = ClampToNeighborhood(history, pixel, size);
    return float4(lerp(history, current.rgb, CurrentWeight), current.a);
}
