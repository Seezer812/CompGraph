#include "SceneObjectRenderer.h"

#include "ObjLoader.h"

#include <array>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
constexpr UINT kCbAlignment = 256;
constexpr UINT kSpatialObjectCount = 2000;
constexpr UINT kMaxObjects = kSpatialObjectCount;
constexpr UINT kSphereSlices = 12;
constexpr UINT kSphereStacks = 8;

const XMMATRIX& DebugCubeWorld()
{
    // Sponza is rotated around X, therefore negative Y is visually upward.
    static const XMMATRIX world = XMMatrixScaling(1.15f, 1.15f, 1.15f) *
        XMMatrixTranslation(0.0f, -2.0f, 0.0f);
    return world;
}

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

struct ShadowConstants
{
    XMFLOAT4X4 world;
    XMFLOAT4X4 lightViewProjection;
    XMFLOAT4 lightPositionRange;
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
    if (FAILED(hr)) return hr;

    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    if (FAILED(hr = out->Map(0, &readRange, &mapped))) return hr;
    if (source) std::memcpy(mapped, source, static_cast<size_t>(size));
    else std::memset(mapped, 0, static_cast<size_t>(size));
    out->Unmap(0, nullptr);
    return S_OK;
}
} // namespace

HRESULT SceneObjectRenderer::Initialize(ID3D12Device* device)
{
    HRESULT hr = S_OK;
    std::vector<Obj::MeshVertex> sphereVertices;
    std::vector<uint32_t> sphereIndices;
    sphereVertices.reserve((kSphereStacks + 1) * (kSphereSlices + 1));
    sphereIndices.reserve(kSphereStacks * kSphereSlices * 6);
    for (UINT stack = 0; stack <= kSphereStacks; ++stack) {
        const float phi = XM_PI * static_cast<float>(stack) / kSphereStacks;
        const float y = cosf(phi);
        const float radial = sinf(phi);
        for (UINT slice = 0; slice <= kSphereSlices; ++slice) {
            const float theta = XM_2PI * static_cast<float>(slice) / kSphereSlices;
            const float x = radial * cosf(theta);
            const float z = radial * sinf(theta);
            sphereVertices.push_back({x, y, z, x, y, z,
                static_cast<float>(slice) / kSphereSlices, static_cast<float>(stack) / kSphereStacks});
        }
    }
    for (UINT stack = 0; stack < kSphereStacks; ++stack)
        for (UINT slice = 0; slice < kSphereSlices; ++slice) {
            const uint32_t first = stack * (kSphereSlices + 1) + slice;
            const uint32_t next = first + kSphereSlices + 1;
            sphereIndices.insert(sphereIndices.end(), {first, next, first + 1, first + 1, next, next + 1});
        }
    if (FAILED(hr = CreateUploadBuffer(device, sphereVertices.data(), sphereVertices.size() * sizeof(Obj::MeshVertex), m_sphereVertexBuffer))) return hr;
    if (FAILED(hr = CreateUploadBuffer(device, sphereIndices.data(), sphereIndices.size() * sizeof(uint32_t), m_sphereIndexBuffer))) return hr;
    const std::array<Obj::MeshVertex, 24> cubeVertices = {{
        {-1,-1, 1, 0, 0, 1, 0,1}, { 1,-1, 1, 0, 0, 1, 1,1}, { 1, 1, 1, 0, 0, 1, 1,0}, {-1, 1, 1, 0, 0, 1, 0,0},
        { 1,-1,-1, 0, 0,-1, 0,1}, {-1,-1,-1, 0, 0,-1, 1,1}, {-1, 1,-1, 0, 0,-1, 1,0}, { 1, 1,-1, 0, 0,-1, 0,0},
        {-1,-1,-1,-1, 0, 0, 0,1}, {-1,-1, 1,-1, 0, 0, 1,1}, {-1, 1, 1,-1, 0, 0, 1,0}, {-1, 1,-1,-1, 0, 0, 0,0},
        { 1,-1, 1, 1, 0, 0, 0,1}, { 1,-1,-1, 1, 0, 0, 1,1}, { 1, 1,-1, 1, 0, 0, 1,0}, { 1, 1, 1, 1, 0, 0, 0,0},
        {-1, 1, 1, 0, 1, 0, 0,1}, { 1, 1, 1, 0, 1, 0, 1,1}, { 1, 1,-1, 0, 1, 0, 1,0}, {-1, 1,-1, 0, 1, 0, 0,0},
        {-1,-1,-1, 0,-1, 0, 0,1}, { 1,-1,-1, 0,-1, 0, 1,1}, { 1,-1, 1, 0,-1, 0, 1,0}, {-1,-1, 1, 0,-1, 0, 0,0},
    }};
    const std::array<uint32_t, 36> cubeIndices = {{
        0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
        12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23,
    }};
    if (FAILED(hr = CreateUploadBuffer(device, cubeVertices.data(), sizeof(cubeVertices), m_cubeVertexBuffer))) return hr;
    if (FAILED(hr = CreateUploadBuffer(device, cubeIndices.data(), sizeof(cubeIndices), m_cubeIndexBuffer))) return hr;

    if (FAILED(hr = CreateUploadBuffer(device, nullptr, 2ull * kMaxObjects * kCbAlignment, m_frameConstants))) return hr;
    if (FAILED(hr = CreateUploadBuffer(device, nullptr, 11 * kCbAlignment, m_markerConstants))) return hr;
    if (FAILED(hr = CreateUploadBuffer(device, nullptr, kCbAlignment, m_materialConstants))) return hr;

    m_sphereVertexView = {m_sphereVertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(sphereVertices.size() * sizeof(Obj::MeshVertex)), sizeof(Obj::MeshVertex)};
    m_sphereIndexView = {m_sphereIndexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(sphereIndices.size() * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};
    m_sphereIndexCount = static_cast<UINT>(sphereIndices.size());
    m_cubeVertexView = {m_cubeVertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(sizeof(cubeVertices)), sizeof(Obj::MeshVertex)};
    m_cubeIndexView = {m_cubeIndexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(sizeof(cubeIndices)), DXGI_FORMAT_R32_UINT};
    m_cubeIndexCount = static_cast<UINT>(cubeIndices.size());
    D3D12_RANGE readRange{0, 0};
    if (FAILED(hr = m_frameConstants->Map(0, &readRange, reinterpret_cast<void**>(&m_frameConstantsMapped)))) return hr;
    if (FAILED(hr = m_markerConstants->Map(0, &readRange, reinterpret_cast<void**>(&m_markerConstantsMapped)))) return hr;
    EmissiveMaterialConstants material{};
    void* mapped = nullptr;
    if (FAILED(hr = m_materialConstants->Map(0, &readRange, &mapped))) return hr;
    std::memcpy(mapped, &material, sizeof(material));
    m_materialConstants->Unmap(0, nullptr);
    m_spatialObjects.Generate(kSpatialObjectCount);
    return S_OK;
}

void SceneObjectRenderer::Draw(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap,
    ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState, const XMMATRIX& viewProjection,
    const XMFLOAT3& cameraPosition, float timeSeconds, bool frustumCulling, bool octreeCulling,
    SpatialCulling::Stats& cullingStats)
{
    const std::vector<uint32_t> visible = m_spatialObjects.FindVisible(viewProjection, frustumCulling, octreeCulling, cullingStats);
    DrawVisible(commandList, srvHeap, rootSignature, pipelineState, viewProjection, cameraPosition, timeSeconds, visible, 0);
}

void SceneObjectRenderer::DrawCulledFromTopCamera(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap,
    ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState, const XMMATRIX& topViewProjection,
    const XMMATRIX& mainViewProjection, float timeSeconds, bool frustumCulling, bool octreeCulling)
{
    SpatialCulling::Stats ignoredStats{};
    const std::vector<uint32_t> visible = m_spatialObjects.FindVisible(
        mainViewProjection, frustumCulling, octreeCulling, ignoredStats);
    const XMFLOAT3 topCameraPosition{0.0f, 48.0f, 0.0f};
    DrawVisible(commandList, srvHeap, rootSignature, pipelineState, topViewProjection, topCameraPosition, timeSeconds, visible, kMaxObjects);
}

void SceneObjectRenderer::DrawVisible(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap,
    ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState, const XMMATRIX& viewProjection,
    const XMFLOAT3& cameraPosition, float timeSeconds, const std::vector<uint32_t>& visible, UINT constantBufferOffset)
{
    if (visible.empty()) return;
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_sphereVertexView);
    commandList->IASetIndexBuffer(&m_sphereIndexView);
    commandList->SetGraphicsRootConstantBufferView(1, m_materialConstants->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, srvHeap->GetGPUDescriptorHandleForHeapStart());
    UINT constantIndex = 0;
    for (uint32_t objectIndex : visible) {
        const auto& object = m_spatialObjects.Objects()[objectIndex];
        FrameConstants frame{};
        XMStoreFloat4x4(&frame.world, XMMatrixScaling(object.radius, object.radius, object.radius) * XMMatrixTranslation(object.position.x, object.position.y, object.position.z));
        XMStoreFloat4x4(&frame.viewProjection, viewProjection);
        frame.timeCamera = XMFLOAT4(timeSeconds, cameraPosition.x, cameraPosition.y, cameraPosition.z);
        const UINT frameSlot = constantBufferOffset + constantIndex;
        std::memcpy(m_frameConstantsMapped + static_cast<size_t>(frameSlot) * kCbAlignment, &frame, sizeof(frame));
        commandList->SetGraphicsRootConstantBufferView(0, m_frameConstants->GetGPUVirtualAddress() + static_cast<UINT64>(frameSlot) * kCbAlignment);
        commandList->DrawIndexedInstanced(m_sphereIndexCount, 1, 0, 0, 0);
        ++constantIndex;
    }
}

namespace
{
const std::array<XMMATRIX, 3>& ShadowCasterWorlds()
{
    static const std::array<XMMATRIX, 3> worlds = {
        XMMatrixScaling(0.48f, 0.70f, 0.48f) * XMMatrixTranslation(-2.7f, 0.56f, 4.5f),
        XMMatrixScaling(0.58f, 0.70f, 0.58f) * XMMatrixTranslation(-5.4f, 0.56f, 3.0f),
        XMMatrixScaling(0.52f, 0.70f, 0.52f) * XMMatrixTranslation(-7.8f, 0.56f, 6.3f),
    };
    return worlds;
}
} // namespace

void SceneObjectRenderer::DrawShadowCaster(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap,
    ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState, const XMMATRIX& viewProjection, float timeSeconds)
{
    FrameConstants frame{};
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetGraphicsRootConstantBufferView(1, m_materialConstants->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_sphereVertexView);
    commandList->IASetIndexBuffer(&m_sphereIndexView);
    UINT markerIndex = 0;
    for (const XMMATRIX& world : ShadowCasterWorlds()) {
        XMStoreFloat4x4(&frame.world, world);
        XMStoreFloat4x4(&frame.viewProjection, viewProjection);
        frame.timeCamera = XMFLOAT4(timeSeconds, 0, 0, 0);
        std::memcpy(m_markerConstantsMapped + static_cast<size_t>(markerIndex) * kCbAlignment, &frame, sizeof(frame));
        commandList->SetGraphicsRootConstantBufferView(0, m_markerConstants->GetGPUVirtualAddress() + static_cast<UINT64>(markerIndex) * kCbAlignment);
        commandList->DrawIndexedInstanced(m_sphereIndexCount, 1, 0, 0, 0);
        ++markerIndex;
    }
}

void SceneObjectRenderer::DrawDebugCube(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap,
    ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState, const XMMATRIX& viewProjection,
    const XMFLOAT3& cameraPosition, float timeSeconds)
{
    // Slot 10 is deliberately separate from slots 0..9 used by shadow passes
    // draws.  The GPU executes this command list after it has all been built.
    constexpr UINT kGeometrySlot = 10;
    FrameConstants frame{};
    XMStoreFloat4x4(&frame.world, DebugCubeWorld());
    XMStoreFloat4x4(&frame.viewProjection, viewProjection);
    frame.timeCamera = XMFLOAT4(timeSeconds, cameraPosition.x, cameraPosition.y, cameraPosition.z);
    std::memcpy(m_markerConstantsMapped + static_cast<size_t>(kGeometrySlot) * kCbAlignment, &frame, sizeof(frame));

    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetGraphicsRootConstantBufferView(0, m_markerConstants->GetGPUVirtualAddress() + static_cast<UINT64>(kGeometrySlot) * kCbAlignment);
    commandList->SetGraphicsRootConstantBufferView(1, m_materialConstants->GetGPUVirtualAddress());
    commandList->SetGraphicsRootDescriptorTable(2, srvHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_cubeVertexView);
    commandList->IASetIndexBuffer(&m_cubeIndexView);
    commandList->DrawIndexedInstanced(m_cubeIndexCount, 1, 0, 0, 0);
}

void SceneObjectRenderer::DrawDebugCubeShadow(ID3D12GraphicsCommandList* commandList,
    ID3D12RootSignature* shadowRootSignature, ID3D12PipelineState* shadowPipelineState,
    const XMMATRIX& lightViewProjection, UINT cascade)
{
    ShadowConstants constants{};
    XMStoreFloat4x4(&constants.world, DebugCubeWorld());
    XMStoreFloat4x4(&constants.lightViewProjection, lightViewProjection);
    std::memcpy(m_markerConstantsMapped + static_cast<size_t>(cascade) * kCbAlignment, &constants, sizeof(constants));

    commandList->SetGraphicsRootSignature(shadowRootSignature);
    commandList->SetPipelineState(shadowPipelineState);
    commandList->SetGraphicsRootConstantBufferView(0, m_markerConstants->GetGPUVirtualAddress() + static_cast<UINT64>(cascade) * kCbAlignment);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_cubeVertexView);
    commandList->IASetIndexBuffer(&m_cubeIndexView);
    commandList->DrawIndexedInstanced(m_cubeIndexCount, 1, 0, 0, 0);
}

void SceneObjectRenderer::DrawDebugCubePointShadow(ID3D12GraphicsCommandList* commandList,
    ID3D12RootSignature* shadowRootSignature, ID3D12PipelineState* shadowPipelineState,
    const XMMATRIX& lightViewProjection, UINT face, const XMFLOAT4& lightPositionRange)
{
    if (face >= 6) return;
    constexpr UINT slot = 4;
    const UINT constantSlot = slot + face;
    ShadowConstants constants{};
    XMStoreFloat4x4(&constants.world, DebugCubeWorld());
    XMStoreFloat4x4(&constants.lightViewProjection, lightViewProjection);
    constants.lightPositionRange = lightPositionRange;
    std::memcpy(m_markerConstantsMapped + static_cast<size_t>(constantSlot) * kCbAlignment, &constants, sizeof(constants));
    commandList->SetGraphicsRootSignature(shadowRootSignature);
    commandList->SetPipelineState(shadowPipelineState);
    commandList->SetGraphicsRootConstantBufferView(0, m_markerConstants->GetGPUVirtualAddress() + static_cast<UINT64>(constantSlot) * kCbAlignment);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_cubeVertexView);
    commandList->IASetIndexBuffer(&m_cubeIndexView);
    commandList->DrawIndexedInstanced(m_cubeIndexCount, 1, 0, 0, 0);
}
