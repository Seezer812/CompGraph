#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace LightTiling
{
constexpr uint32_t TileSize = 32;
constexpr uint32_t MaxPointLights = 4096;

struct PointLight
{
    DirectX::XMFLOAT4 positionRadius{};
    DirectX::XMFLOAT4 colorIntensity{};
};
static_assert(sizeof(PointLight) == 32);

struct TileRange { uint32_t offset, count; };
static_assert(sizeof(TileRange) == 8);

struct Grid
{
    uint32_t columns = 0, rows = 0;
    std::vector<TileRange> ranges;
    std::vector<uint32_t> indices;
};

inline Grid Build(std::span<const PointLight> lights,
    const DirectX::XMMATRIX& viewProjection, uint32_t width, uint32_t height)
{
    using namespace DirectX;
    Grid grid;
    if (width == 0 || height == 0) return grid;
    grid.columns = (width + TileSize - 1) / TileSize;
    grid.rows = (height + TileSize - 1) / TileSize;
    std::vector<std::vector<uint32_t>> tiles(static_cast<size_t>(grid.columns) * grid.rows);
    const XMMATRIX columns = XMMatrixTranspose(viewProjection);
    std::array<XMVECTOR, 6> planes = {
        columns.r[3] + columns.r[0], columns.r[3] - columns.r[0],
        columns.r[3] + columns.r[1], columns.r[3] - columns.r[1],
        columns.r[2], columns.r[3] - columns.r[2]};
    for (auto& plane : planes) plane = XMPlaneNormalize(plane);

    for (uint32_t index = 0; index < lights.size(); ++index)
    {
        const auto& p = lights[index].positionRadius;
        const float radius = p.w;
        if (radius <= 0.0f || lights[index].colorIntensity.w <= 0.0f) continue;
        const XMVECTOR center = XMVectorSet(p.x, p.y, p.z, 1.0f);
        bool outside = false;
        for (const auto& plane : planes)
            outside |= XMVectorGetX(XMPlaneDotCoord(plane, center)) < -radius - 1e-4f;
        if (outside) continue;

        float minX = 1.0f, minY = 1.0f, maxX = -1.0f, maxY = -1.0f;
        bool nearIntersection = false;
        for (int z : {-1, 1}) for (int y : {-1, 1}) for (int x : {-1, 1})
        {
            const XMVECTOR corner = XMVector4Transform(
                center + XMVectorSet(x * radius, y * radius, z * radius, 0.0f), viewProjection);
            const float w = XMVectorGetW(corner);
            if (w <= 1e-5f || XMVectorGetZ(corner) <= 0.0f)
            {
                nearIntersection = true;
                continue;
            }
            const float ndcX = XMVectorGetX(corner) / w;
            const float ndcY = XMVectorGetY(corner) / w;
            minX = (std::min)(minX, ndcX); maxX = (std::max)(maxX, ndcX);
            minY = (std::min)(minY, ndcY); maxY = (std::max)(maxY, ndcY);
        }
        if (nearIntersection) { minX = minY = -1.0f; maxX = maxY = 1.0f; }
        auto tileX = [&](float ndc) {
            return (std::min)(grid.columns - 1, static_cast<uint32_t>(
                std::clamp(ndc * 0.5f + 0.5f, 0.0f, 1.0f) * width) / TileSize);
        };
        auto tileY = [&](float ndc) {
            return (std::min)(grid.rows - 1, static_cast<uint32_t>(
                std::clamp(0.5f - ndc * 0.5f, 0.0f, 1.0f) * height) / TileSize);
        };
        const uint32_t firstX = tileX(minX - 1e-5f), lastX = tileX(maxX + 1e-5f);
        const uint32_t firstY = tileY(maxY + 1e-5f), lastY = tileY(minY - 1e-5f);
        for (uint32_t y = firstY; y <= lastY; ++y)
            for (uint32_t x = firstX; x <= lastX; ++x)
                tiles[static_cast<size_t>(y) * grid.columns + x].push_back(index);
    }
    grid.ranges.reserve(tiles.size());
    for (const auto& tile : tiles)
    {
        grid.ranges.push_back({static_cast<uint32_t>(grid.indices.size()), static_cast<uint32_t>(tile.size())});
        grid.indices.insert(grid.indices.end(), tile.begin(), tile.end());
    }
    return grid;
}
}
