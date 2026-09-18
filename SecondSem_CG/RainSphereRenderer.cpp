#include "RainSphereRenderer.h"

#include "ObjLoader.h"

#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
constexpr UINT kCbAlignment = 256;
constexpr UINT kRainSphereCount = 125;
constexpr UINT kSpatialObjectCount = 2000;
constexpr UINT kMaxSpheres = kRainSphereCount + kSpatialObjectCount;

struct alignas(256) FrameConstants
{
    XMFLOAT4X4 world;
    XMFLOAT4X4 viewProjection;
    XMFLOAT4 timeCamera;
    XMFLOAT4 uvAnimation;
    float padding[24];
};

struct alignas(256) EmissiveMaterialConstants
{
    XMFLOAT4 kd{0.25f, 0.65f, 1.0f, 1.0f};
    XMFLOAT2 uvScale{1.0f, 1.0f};
    XMFLOAT2 uvOffset{};
    XMFLOAT3 ks{};
    float ns = 0.0f;
    UINT useUvAnimation = 0;
    UINT hasSpecularMap = 0;
    UINT isEmissive = 1;
    UINT hasNormalMap = 0;
    UINT hasDisplacementMap = 0;
    UINT enableTessellation = 0;
    float padding[46]{};
};

static_assert(sizeof(FrameConstants) == kCbAlignment);
static_assert(sizeof(EmissiveMaterialConstants) == kCbAlignment);

HRESULT CreateUploadBuffer(ID3D12Device* device, const void* source, UINT64 size, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr))
        return hr;
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    if (FAILED(hr = out->Map(0, &readRange, &mapped)))
        return hr;
    if (source)
        std::memcpy(mapped, source, static_cast<size_t>(size));
    else
        std::memset(mapped, 0, static_cast<size_t>(size));
    out->Unmap(0, nullptr);
    return S_OK;
}

} // namespace

HRESULT RainSphereRenderer::Initialize(ID3D12Device* device)
{
    const Obj::MeshVertex vertices[] = {
        {0, 1, 0, 0, 1, 0, 0, 0}, {1, 0, 0, 1, 0, 0, 0, 0},
        {0, 0, 1, 0, 0, 1, 0, 0}, {-1, 0, 0, -1, 0, 0, 0, 0},
        {0, 0, -1, 0, 0, -1, 0, 0}, {0, -1, 0, 0, -1, 0, 0, 0},
    };
    const uint32_t indices[] = {0,1,2, 0,2,3, 0,3,4, 0,4,1, 5,2,1, 5,3,2, 5,4,3, 5,1,4};
    HRESULT hr = CreateUploadBuffer(device, vertices, sizeof(vertices), m_vertexBuffer);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = CreateUploadBuffer(device, indices, sizeof(indices), m_indexBuffer))) return hr;
    if (FAILED(hr = CreateUploadBuffer(device, nullptr, kMaxSpheres * kCbAlignment, m_frameConstants))) return hr;
    if (FAILED(hr = CreateUploadBuffer(device, nullptr, kCbAlignment, m_materialConstants))) return hr;

    m_vertexView = {m_vertexBuffer->GetGPUVirtualAddress(), sizeof(vertices), sizeof(Obj::MeshVertex)};
    m_indexView = {m_indexBuffer->GetGPUVirtualAddress(), sizeof(indices), DXGI_FORMAT_R32_UINT};
    D3D12_RANGE readRange{0, 0};
    if (FAILED(hr = m_frameConstants->Map(0, &readRange, reinterpret_cast<void**>(&m_frameConstantsMapped)))) return hr;
    EmissiveMaterialConstants material{};
    void* mapped = nullptr;
    if (FAILED(hr = m_materialConstants->Map(0, &readRange, &mapped))) return hr;
    std::memcpy(mapped, &material, sizeof(material));
    m_materialConstants->Unmap(0, nullptr);
    m_spatialObjects.Generate(kSpatialObjectCount);
    return S_OK;
}

void RainSphereRenderer::Draw(
    ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap,
    ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState,
    const std::vector<RenderingSystem::RainLight>& drops, const XMMATRIX& viewProjection,
    const XMFLOAT3& cameraPosition, float timeSeconds, bool frustumCulling,
    bool octreeCulling, SpatialCulling::Stats& cullingStats)
{
    const std::vector<uint32_t> visible = m_spatialObjects.FindVisible(
        viewProjection, frustumCulling, octreeCulling, cullingStats);
    if (drops.empty() && visible.empty()) return;
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexView);
    commandList->IASetIndexBuffer(&m_indexView);
    commandList->SetGraphicsRootConstantBufferView(1, m_materialConstants->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, srvHeap->GetGPUDescriptorHandleForHeapStart());

    constexpr float radius = 0.045f;
    constexpr float hover = 0.075f;
    UINT constantIndex = 0;
    for (UINT i = 0; i < drops.size() && i < kRainSphereCount; ++i, ++constantIndex)
    {
        FrameConstants frame{};
        XMStoreFloat4x4(&frame.world, XMMatrixScaling(radius, radius, radius) *
            XMMatrixTranslation(drops[i].position.x, drops[i].position.y - hover, drops[i].position.z));
        XMStoreFloat4x4(&frame.viewProjection, viewProjection);
        frame.timeCamera = XMFLOAT4(timeSeconds, cameraPosition.x, cameraPosition.y, cameraPosition.z);
        std::memcpy(m_frameConstantsMapped + static_cast<size_t>(constantIndex) * kCbAlignment, &frame, sizeof(frame));
        commandList->SetGraphicsRootConstantBufferView(
            0, m_frameConstants->GetGPUVirtualAddress() + static_cast<UINT64>(constantIndex) * kCbAlignment);
        commandList->DrawIndexedInstanced(24, 1, 0, 0, 0);
    }
    for (uint32_t objectIndex : visible)
    {
        const auto& object = m_spatialObjects.Objects()[objectIndex];
        FrameConstants frame{};
        XMStoreFloat4x4(&frame.world, XMMatrixScaling(object.radius, object.radius, object.radius) *
            XMMatrixTranslation(object.position.x, object.position.y, object.position.z));
        XMStoreFloat4x4(&frame.viewProjection, viewProjection);
        frame.timeCamera = XMFLOAT4(timeSeconds, cameraPosition.x, cameraPosition.y, cameraPosition.z);
        std::memcpy(m_frameConstantsMapped + static_cast<size_t>(constantIndex) * kCbAlignment, &frame, sizeof(frame));
        commandList->SetGraphicsRootConstantBufferView(
            0, m_frameConstants->GetGPUVirtualAddress() + static_cast<UINT64>(constantIndex) * kCbAlignment);
        commandList->DrawIndexedInstanced(24, 1, 0, 0, 0);
        ++constantIndex;
    }
}
