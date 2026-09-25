#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "SpatialCulling.h"

// Рисует тестовые объекты для проверки frustum/octree culling и CSM shadows.
class SceneObjectRenderer
{
public:
    HRESULT Initialize(ID3D12Device* device);

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        float timeSeconds,
        bool frustumCulling,
        bool octreeCulling,
        SpatialCulling::Stats& cullingStats);

    // Рисует с верхней камеры ровно те объекты, которые прошли culling основной камеры.
    void DrawCulledFromTopCamera(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& topViewProjection,
        const DirectX::XMMATRIX& mainViewProjection,
        float timeSeconds,
        bool frustumCulling,
        bool octreeCulling);

    // A fixed, clearly visible marker for demonstrating the directional shadow.
    void DrawShadowCaster(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        float timeSeconds);

    // Неподвижный куб в центре зала: на нём удобно проверять CSM без влияния
    // frustum- и octree-culling тестовых сфер.
    void DrawDebugCube(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        float timeSeconds);

    void DrawDebugCubeShadow(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature* shadowRootSignature,
        ID3D12PipelineState* shadowPipelineState,
        const DirectX::XMMATRIX& lightViewProjection,
        UINT cascade);

    void DrawDebugCubePointShadow(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature* shadowRootSignature,
        ID3D12PipelineState* shadowPipelineState,
        const DirectX::XMMATRIX& lightViewProjection,
        UINT face,
        const DirectX::XMFLOAT4& lightPositionRange);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sphereVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sphereIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_sphereVertexView{};
    D3D12_INDEX_BUFFER_VIEW m_sphereIndexView{};
    UINT m_sphereIndexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cubeVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cubeIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_cubeVertexView{};
    D3D12_INDEX_BUFFER_VIEW m_cubeIndexView{};
    UINT m_cubeIndexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_frameConstants;
    uint8_t* m_frameConstantsMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_markerConstants;
    uint8_t* m_markerConstantsMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialConstants;
    SpatialCulling m_spatialObjects;

    void DrawVisible(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        float timeSeconds,
        const std::vector<uint32_t>& visibleObjects,
        UINT constantBufferOffset);
};
