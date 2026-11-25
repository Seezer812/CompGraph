#pragma once

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class TerrainData
{
public:
    static constexpr uint32_t Grid = 32, Side = Grid + 1, MaxLevel = 5;
    static constexpr float WorldSize = 1024.0f, HeightScale = 512.0f, PixelError = 3.0f;
    struct Vertex { DirectX::XMFLOAT2 positionXZ, uv; float skirtDrop = 0; };
    struct Node {
        DirectX::XMFLOAT3 minimum{}, maximum{};
        std::array<int, 4> children{-1, -1, -1, -1};
        uint32_t x = 0, z = 0, span = 0, level = 0, baseVertex = 0;
        float geometricError = 0;
        bool split = false;
    };
    struct Stats {
        uint32_t visited = 0, culled = 0, patches = 0, triangles = 0;
        std::array<uint32_t, MaxLevel + 1> levels{};
    };

    bool Load(const std::filesystem::path& heightMap, std::wstring& error, bool sponzaSite = false);
    void Select(const DirectX::XMMATRIX& viewProjection, const DirectX::XMFLOAT3& eye,
        uint32_t screenHeight, bool culling);
    bool Validate(std::wstring& error);
    float Height(float x, float z) const;
    const std::vector<Vertex>& Vertices() const { return m_vertices; }
    const std::vector<uint32_t>& Indices() const { return m_indices; }
    const std::vector<Node>& Nodes() const { return m_nodes; }
    const std::vector<int>& Selected() const { return m_selected; }
    const Stats& Statistics() const { return m_stats; }
    uint32_t Width() const { return m_width; }
    uint32_t HeightPixels() const { return m_height; }
    const std::vector<uint16_t>& HeightSamples() const { return m_samples; }
    bool HasSponzaSite() const { return m_sponzaSite; }
    static bool Outside(const Node& node, const DirectX::XMMATRIX& vp);

private:
    int BuildNode(uint32_t x, uint32_t z, uint32_t span, uint32_t level);
    void Visit(int index, const DirectX::XMMATRIX& vp, const DirectX::XMFLOAT3& eye,
        const DirectX::XMFLOAT3& forward, float projectionScale, bool culling);
    void ResetSelection();
    uint32_t m_width = 0, m_height = 0;
    float m_skirtDepth = 0;
    bool m_sponzaSite = false;
    float m_siteHeight = 0;
    std::vector<uint16_t> m_samples;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<Node> m_nodes;
    std::vector<int> m_selected;
    Stats m_stats{};
};

