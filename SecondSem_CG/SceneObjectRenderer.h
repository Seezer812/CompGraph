#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "SpatialCulling.h"

class SceneObjectRenderer
{
public:
    void BeginFrame(UINT frameIndex) { m_frameIndex = frameIndex; }
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

    void DrawShadowCaster(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        float timeSeconds);

    void DrawMotionProbe(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        float timeSeconds,
        const DirectX::XMMATRIX& world,
        const DirectX::XMMATRIX& previousWorld);

    void DrawLightMarker(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        const DirectX::XMFLOAT3& lightPosition,
        const DirectX::XMFLOAT3& lightColor,
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
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    HRESULT EnsureDebugCubeGeometry();
    UINT m_frameIndex = 0;
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
    UINT m_objectCapacity = 0;
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
