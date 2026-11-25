#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "TerrainData.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

bool TerrainData::Load(const std::filesystem::path& heightMap, std::wstring& error, bool sponzaSite)
{
    m_sponzaSite = false; m_siteHeight = 0;
    m_samples.clear(); m_vertices.clear(); m_indices.clear(); m_nodes.clear();
    m_width = m_height = 0;
    error.clear();
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename(heightMap.c_str(), nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) { error = L"Cannot read height map: " + heightMap.wstring(); return false; }
    WICPixelFormatGUID format{};
    hr = frame->GetPixelFormat(&format);
    if (FAILED(hr) || !IsEqualGUID(format, GUID_WICPixelFormat16bppGray)) {
        error = L"HeightMap.png must be a single-channel 16-bit grayscale image."; return false;
    }
    hr = frame->GetSize(&m_width, &m_height);
    if (FAILED(hr) || m_width != 1024 || m_height != 1024) {
        error = L"HeightMap.png must be 1024 x 1024."; return false;
    }
    m_samples.resize(size_t(m_width) * m_height);
    hr = frame->CopyPixels(nullptr, m_width * sizeof(uint16_t),
        UINT(m_samples.size() * sizeof(uint16_t)), reinterpret_cast<BYTE*>(m_samples.data()));
    if (FAILED(hr)) { error = L"Cannot copy 16-bit height pixels."; return false; }
    const auto [lowest, highest] = std::minmax_element(m_samples.begin(), m_samples.end());
    m_skirtDepth = float(*highest - *lowest) * HeightScale / 65535.0f + 1.0f;
    m_siteHeight = Height(WorldSize / 2, WorldSize / 2);
    m_sponzaSite = sponzaSite;
    m_nodes.reserve(1365);
    m_vertices.reserve(1365 * (Side * Side + 4 * Side));
    BuildNode(0, 0, uint32_t(WorldSize), 0);
    for (uint32_t z = 0; z < Grid; ++z)
        for (uint32_t x = 0; x < Grid; ++x) {
            const uint32_t a = z * Side + x, b = a + 1, c = a + Side, d = c + 1;
            m_indices.insert(m_indices.end(), {a, c, b, b, c, d});
        }
    for (uint32_t edge = 0; edge < 4; ++edge)
        for (uint32_t t = 0; t < Grid; ++t) {
            const uint32_t a = edge == 0 ? t : edge == 1 ? Grid * Side + t
                : edge == 2 ? t * Side : t * Side + Grid;
            const uint32_t b = a + (edge < 2 ? 1 : Side);
            const uint32_t c = Side * Side + edge * Side + t, d = c + 1;
            m_indices.insert(m_indices.end(), {a, b, c, b, d, c});
        }
    ResetSelection();
    return true;
}

float TerrainData::Height(float x, float z) const
{
    const float sx = std::clamp(x / WorldSize, 0.0f, 1.0f) * (m_width - 1);
    const float sz = std::clamp(z / WorldSize, 0.0f, 1.0f) * (m_height - 1);
    const uint32_t x0 = uint32_t(sx), z0 = uint32_t(sz);
    const uint32_t x1 = std::min(x0 + 1, m_width - 1), z1 = std::min(z0 + 1, m_height - 1);
    const float u = sx - x0, v = sz - z0;
    const float a = float(m_samples[z0 * m_width + x0]), b = float(m_samples[z0 * m_width + x1]);
    const float c = float(m_samples[z1 * m_width + x0]), d = float(m_samples[z1 * m_width + x1]);
    const float raw = ((a + u * (b - a)) * (1 - v) + (c + u * (d - c)) * v) * (HeightScale / 65535.0f);
    if (!m_sponzaSite) return raw;
    const float dx = x - WorldSize / 2, dz = z - WorldSize / 2;
    float t = std::clamp((std::sqrt(dx * dx + dz * dz) - 24.0f) / 24.0f, 0.0f, 1.0f);
    t = t * t * (3 - 2 * t);
    return (raw - m_siteHeight) * t - 0.10f;
}

int TerrainData::BuildNode(uint32_t x, uint32_t z, uint32_t span, uint32_t level)
{
    Node node;
    node.x = x; node.z = z; node.span = span; node.level = level;
    node.baseVertex = uint32_t(m_vertices.size());
    const uint32_t step = span / Grid;
    float lowest = HeightScale, highest = 0;
    for (uint32_t dz = 0; dz <= span; ++dz)
        for (uint32_t dx = 0; dx <= span; ++dx) {
            const float h = Height(float(x + dx), float(z + dz));
            lowest = std::min(lowest, h); highest = std::max(highest, h);
            const uint32_t gx = std::min(dx / step, Grid - 1), gz = std::min(dz / step, Grid - 1);
            const float u = float(dx - gx * step) / step, v = float(dz - gz * step) / step;
            const float ax = float(x + gx * step), az = float(z + gz * step);
            const float a = Height(ax, az), b = Height(ax + step, az);
            const float c = Height(ax, az + step), d = Height(ax + step, az + step);
            const float approximation = u + v <= 1 ? a + u * (b - a) + v * (c - a)
                : d + (1 - u) * (c - d) + (1 - v) * (b - d);
            node.geometricError = std::max(node.geometricError, std::abs(h - approximation));
        }
    const uint32_t sx0 = uint32_t(std::floor(float(x) / WorldSize * (m_width - 1)));
    const uint32_t sz0 = uint32_t(std::floor(float(z) / WorldSize * (m_height - 1)));
    const uint32_t sx1 = std::min(m_width - 1, uint32_t(std::ceil(float(x + span) / WorldSize * (m_width - 1))));
    const uint32_t sz1 = std::min(m_height - 1, uint32_t(std::ceil(float(z + span) / WorldSize * (m_height - 1))));
    for (uint32_t sz = sz0; sz <= sz1; ++sz)
        for (uint32_t sx = sx0; sx <= sx1; ++sx) {
            const float h = Height(float(sx) * WorldSize / (m_width - 1), float(sz) * WorldSize / (m_height - 1));
            lowest = std::min(lowest, h); highest = std::max(highest, h);
        }
    node.minimum = {float(x) - WorldSize / 2, lowest - m_skirtDepth, float(z) - WorldSize / 2};
    node.maximum = {float(x + span) - WorldSize / 2, highest, float(z + span) - WorldSize / 2};
    for (uint32_t gz = 0; gz <= Grid; ++gz)
        for (uint32_t gx = 0; gx <= Grid; ++gx) {
            const float sx = float(x + gx * step), sz = float(z + gz * step);
            m_vertices.push_back({{sx - WorldSize / 2, sz - WorldSize / 2},
                {sx / WorldSize, sz / WorldSize}, 0});
        }
    for (uint32_t edge = 0; edge < 4; ++edge)
        for (uint32_t t = 0; t <= Grid; ++t) {
            const uint32_t source = edge == 0 ? t : edge == 1 ? Grid * Side + t
                : edge == 2 ? t * Side : t * Side + Grid;
            Vertex v = m_vertices[node.baseVertex + source];
            const bool outer = (edge == 0 && z == 0) || (edge == 1 && z + span == uint32_t(WorldSize))
                || (edge == 2 && x == 0) || (edge == 3 && x + span == uint32_t(WorldSize));
            if (!outer) v.skirtDrop = m_skirtDepth;
            m_vertices.push_back(v);
        }
    const int index = int(m_nodes.size());
    m_nodes.push_back(node);
    if (level < MaxLevel) {
        const uint32_t half = span / 2;
        for (uint32_t child = 0; child < 4; ++child) {
            const int childIndex = BuildNode(x + (child & 1) * half,
                z + ((child >> 1) & 1) * half, half, level + 1);
            m_nodes[index].children[child] = childIndex;
            m_nodes[index].geometricError = std::max(m_nodes[index].geometricError,
                m_nodes[childIndex].geometricError);
        }
    }
    return index;
}

bool TerrainData::Outside(const Node& node, const XMMATRIX& vp)
{
    uint32_t common = 63;
    for (float z : {node.minimum.z, node.maximum.z})
        for (float y : {node.minimum.y, node.maximum.y})
            for (float x : {node.minimum.x, node.maximum.x}) {
                XMFLOAT4 p;
                XMStoreFloat4(&p, XMVector4Transform(XMVectorSet(x, y, z, 1), vp));
                const uint32_t mask = (p.x < -p.w ? 1 : 0) | (p.x > p.w ? 2 : 0)
                    | (p.y < -p.w ? 4 : 0) | (p.y > p.w ? 8 : 0)
                    | (p.z < 0 ? 16 : 0) | (p.z > p.w ? 32 : 0);
                common &= mask;
            }
    return common != 0;
}

void TerrainData::Visit(int index, const XMMATRIX& vp, const XMFLOAT3& eye,
    const XMFLOAT3& forward, float projectionScale, bool culling)
{
    Node& node = m_nodes[index];
    ++m_stats.visited;
    if (culling && Outside(node, vp)) { ++m_stats.culled; return; }
    const XMFLOAT3 center{(node.minimum.x + node.maximum.x) / 2,
        (node.minimum.y + node.maximum.y) / 2, (node.minimum.z + node.maximum.z) / 2};
    const float depth = (center.x - eye.x) * forward.x + (center.y - eye.y) * forward.y
        + (center.z - eye.z) * forward.z
        - std::abs(forward.x) * (node.maximum.x - node.minimum.x) / 2
        - std::abs(forward.y) * (node.maximum.y - node.minimum.y) / 2
        - std::abs(forward.z) * (node.maximum.z - node.minimum.z) / 2;
    const float screenError = node.geometricError * projectionScale / std::max(1.0f, depth);
    const float threshold = PixelError * (node.split ? 0.75f : 1.0f);
    node.split = node.children[0] >= 0 && screenError > threshold;
    if (node.split) {
        for (int child : node.children) Visit(child, vp, eye, forward, projectionScale, culling);
    } else {
        m_selected.push_back(index);
        ++m_stats.patches; ++m_stats.levels[node.level];
        m_stats.triangles += uint32_t(m_indices.size() / 3);
    }
}

void TerrainData::ResetSelection()
{
    for (auto& node : m_nodes) node.split = false;
    m_selected.clear(); m_stats = {};
}

void TerrainData::Select(const XMMATRIX& vp, const XMFLOAT3& eye, uint32_t screenHeight, bool culling)
{
    m_stats = {}; m_selected.clear();
    if (m_nodes.empty()) return;
    XMFLOAT4X4 matrix;
    XMStoreFloat4x4(&matrix, vp);
    XMFLOAT3 forward;
    XMStoreFloat3(&forward, XMVector3Normalize(XMVectorSet(matrix._14, matrix._24, matrix._34, 0)));
    const float projectionScale = float(screenHeight) / (2 * std::tan(XM_PIDIV4 / 2));
    Visit(0, vp, eye, forward, projectionScale, culling);
}

bool TerrainData::Validate(std::wstring& error)
{
    auto fail = [&](const wchar_t* message) { error = message; ResetSelection(); return false; };
    if (m_nodes.size() != 1365 || m_samples.size() != 1024 * 1024)
        return fail(L"Invalid source size or quadtree node count");
    if (std::none_of(m_samples.begin(), m_samples.end(), [](uint16_t v) { return v % 257 != 0; }))
        return fail(L"Height map has only 8-bit precision");
    for (const auto& node : m_nodes) {
        for (uint32_t z = 0; z <= Grid; ++z)
            for (uint32_t x = 0; x <= Grid; ++x) {
                const auto& v = m_vertices[node.baseVertex + z * Side + x];
                const float height = Height(v.uv.x * WorldSize, v.uv.y * WorldSize);
                if (height < node.minimum.y || height > node.maximum.y)
                    return fail(L"Vertex outside its culling bounds");
            }
        if (node.level == MaxLevel) continue;
        for (uint32_t child = 0; child < 4; ++child)
            for (uint32_t t = 0; t <= Grid; ++t) {
                const auto& a = m_nodes[node.children[child]];
                if ((child & 1) == 0) {
                    const auto& b = m_nodes[node.children[child + 1]];
                    const auto& va = m_vertices[a.baseVertex + t * Side + Grid];
                    const auto& vb = m_vertices[b.baseVertex + t * Side];
                    if (va.positionXZ.x != vb.positionXZ.x || va.positionXZ.y != vb.positionXZ.y
                        || va.uv.x != vb.uv.x || va.uv.y != vb.uv.y)
                        return fail(L"Horizontal tile edge mismatch");
                }
                if (child < 2) {
                    const auto& b = m_nodes[node.children[child + 2]];
                    const auto& va = m_vertices[a.baseVertex + Grid * Side + t];
                    const auto& vb = m_vertices[b.baseVertex + t];
                    if (va.positionXZ.x != vb.positionXZ.x || va.positionXZ.y != vb.positionXZ.y
                        || va.uv.x != vb.uv.x || va.uv.y != vb.uv.y)
                        return fail(L"Vertical tile edge mismatch");
                }
            }
    }
    uint32_t nearCount = 0;
    for (XMFLOAT3 eye : {XMFLOAT3{0, 220, -400}, XMFLOAT3{0, 220, -20000}}) {
        ResetSelection();
        const auto view = XMMatrixLookToLH(XMLoadFloat3(&eye), XMVectorSet(0, 0, 1, 0), XMVectorSet(0, 1, 0, 0));
        const auto vp = view * XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9, 1, 30000);
        Select(vp, eye, 720, false);
        std::array<uint32_t, 1024> coverage{};
        for (int index : m_selected) {
            const auto& n = m_nodes[index];
            for (uint32_t z = n.z / 32; z < (n.z + n.span) / 32; ++z)
                for (uint32_t x = n.x / 32; x < (n.x + n.span) / 32; ++x) ++coverage[z * 32 + x];
        }
        if (std::any_of(coverage.begin(), coverage.end(), [](uint32_t n) { return n != 1; }))
            return fail(L"LOD selection leaves gaps or overlapping parents");
        if (!nearCount) nearCount = m_stats.patches;
        else if (m_stats.patches >= nearCount) return fail(L"LOD does not simplify at distance");
    }
    Node box; box.minimum = {-0.5f, -0.5f, 0.1f}; box.maximum = {0.5f, 0.5f, 0.9f};
    const auto identity = XMMatrixIdentity();
    if (Outside(box, identity)) return fail(L"Visible box culled");
    box.minimum.z = -1;
    if (Outside(box, identity)) return fail(L"Near-plane intersection culled");
    box.maximum.z = -0.1f;
    if (!Outside(box, identity)) return fail(L"Box behind camera retained");
    box.minimum = {2, 0, 0.1f}; box.maximum = {3, 1, 0.9f};
    if (!Outside(box, identity)) return fail(L"Offscreen box retained");
    error.clear(); ResetSelection(); return true;
}
