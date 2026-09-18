#pragma once

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <vector>

class SpatialCulling
{
public:
    struct Object { DirectX::XMFLOAT3 position{}; float radius = 0.25f; };
    struct Stats { uint32_t visibleObjects = 0, objectTests = 0, nodeTests = 0; };

    void Generate(uint32_t count, uint32_t seed = 20260918);
    const std::vector<Object>& Objects() const { return m_objects; }
    uint32_t NodeCount() const { return static_cast<uint32_t>(m_nodes.size()); }
    // octree=false tests all objects directly; frustum=false returns all objects.
    std::vector<uint32_t> FindVisible(const DirectX::XMMATRIX& viewProjection, bool frustum, bool octree, Stats& stats) const;

private:
    struct Bounds { DirectX::XMFLOAT3 min{}, max{}; };
    struct Node { Bounds bounds{}; std::array<int, 8> children{}; std::vector<uint32_t> objects; Node() { children.fill(-1); } };
    static bool OutsideFrustum(const Bounds& bounds, const DirectX::XMMATRIX& viewProjection);
    static Bounds ObjectBounds(const Object& object);
    static bool Contains(const Bounds& outer, const Bounds& inner);
    void Build();
    void Insert(uint32_t objectIndex, int nodeIndex, uint32_t depth);
    void Split(int nodeIndex);
    void Visit(int nodeIndex, const DirectX::XMMATRIX& viewProjection, std::vector<uint32_t>& out, Stats& stats) const;
    std::vector<Object> m_objects;
    std::vector<Node> m_nodes;
};
