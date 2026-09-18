#include "WaveWallRenderer.h"

#include "D3DHelpers.h"

#include <cstring>
#include <vector>

using namespace DirectX;

namespace
{
constexpr UINT kCbAlignment = 256;
struct Vertex { XMFLOAT3 position; XMFLOAT3 normal; XMFLOAT2 uv; };
struct alignas(256) FrameConstants { XMFLOAT4X4 world; XMFLOAT4X4 viewProjection; XMFLOAT4 timeCamera; XMFLOAT4 uvAnimation; float padding[24]; };
struct alignas(256) MaterialConstants
{
    XMFLOAT4 kd; XMFLOAT2 uvScale; XMFLOAT2 uvOffset; XMFLOAT3 ks; float ns;
    UINT useUvAnimation, hasSpecularMap, isEmissive, hasNormalMap, hasDisplacementMap, enableTessellation;
    float padding[46];
};
static_assert(sizeof(FrameConstants) == kCbAlignment);
static_assert(sizeof(MaterialConstants) == kCbAlignment);
} // namespace

bool WaveWallRenderer::Initialize(ID3D12Device* device)
{
    // Every cell is a quad patch, in the order bottom-left, bottom-right, top-left, top-right.
    // Sponza's large brick wall is at world Z = 2.5 after the scene transform.
    // The 0.24 offset leaves enough clearance for the 0.22 wave amplitude.
    constexpr float left = -8.5f, right = -3.5f, bottom = -6.8f, top = -2.4f, z = 1.74f;
    constexpr UINT columns = 8, rows = 8;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve((columns + 1) * (rows + 1));
    indices.reserve(columns * rows * 4);
    for (UINT y = 0; y <= rows; ++y)
    {
        const float v = static_cast<float>(y) / rows;
        for (UINT x = 0; x <= columns; ++x)
        {
            const float u = static_cast<float>(x) / columns;
            vertices.push_back({{left + (right - left) * u, bottom + (top - bottom) * v, z}, {0, 0, 1}, {u, v}});
        }
    }
    for (UINT y = 0; y < rows; ++y)
        for (UINT x = 0; x < columns; ++x)
        {
            const uint32_t bottomLeft = y * (columns + 1) + x;
            indices.insert(indices.end(), {bottomLeft, bottomLeft + 1, bottomLeft + columns + 1, bottomLeft + columns + 2});
        }
    m_vertexBuffer = D3DHelpers::CreateUploadBuffer(device, vertices.data(), sizeof(Vertex) * vertices.size());
    m_indexBuffer = D3DHelpers::CreateUploadBuffer(device, indices.data(), sizeof(uint32_t) * indices.size());
    if (!m_vertexBuffer || !m_indexBuffer) return false;
    m_vertexView = {m_vertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(sizeof(Vertex) * vertices.size()), sizeof(Vertex)};
    m_indexView = {m_indexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(sizeof(uint32_t) * indices.size()), DXGI_FORMAT_R32_UINT};
    m_indexCount = static_cast<UINT>(indices.size());

    m_frameConstants = D3DHelpers::CreateUploadBuffer(device, nullptr, kCbAlignment);
    m_materialConstants = D3DHelpers::CreateUploadBuffer(device, nullptr, kCbAlignment);
    if (!m_frameConstants || !m_materialConstants) return false;
    D3D12_RANGE range{0, 0};
    if (FAILED(m_frameConstants->Map(0, &range, &m_frameConstantsMapped))) return false;
    void* materialMapped = nullptr;
    if (FAILED(m_materialConstants->Map(0, &range, &materialMapped))) return false;
    MaterialConstants material{};
    material.kd = XMFLOAT4(0.08f, 0.38f, 0.82f, 1.0f);
    material.ks = XMFLOAT3(0.55f, 0.65f, 0.9f);
    material.ns = 64.0f;
    std::memcpy(materialMapped, &material, sizeof(material));
    m_materialConstants->Unmap(0, nullptr);
    return true;
}

void WaveWallRenderer::Draw(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap,
    ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState,
    const XMMATRIX& viewProjection, float timeSeconds) const
{
    if (!m_frameConstantsMapped || !pipelineState) return;
    FrameConstants frame{};
    XMStoreFloat4x4(&frame.world, XMMatrixIdentity());
    XMStoreFloat4x4(&frame.viewProjection, viewProjection);
    frame.timeCamera = XMFLOAT4(timeSeconds, 0, 0, 0);
    std::memcpy(m_frameConstantsMapped, &frame, sizeof(frame));
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetGraphicsRootConstantBufferView(0, m_frameConstants->GetGPUVirtualAddress());
    commandList->SetGraphicsRootConstantBufferView(1, m_materialConstants->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexView);
    commandList->IASetIndexBuffer(&m_indexView);
    commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}
