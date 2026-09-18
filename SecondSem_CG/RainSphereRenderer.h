#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "RenderingSystem.h"
#include "SpatialCulling.h"

class RainSphereRenderer
{
public:
    HRESULT Initialize(ID3D12Device* device);
    uint32_t SpatialNodeCount() const { return m_spatialObjects.NodeCount(); }

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const std::vector<RenderingSystem::RainLight>& drops,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        float timeSeconds,
        bool frustumCulling,
        bool octreeCulling,
        SpatialCulling::Stats& cullingStats);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_INDEX_BUFFER_VIEW m_indexView{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_frameConstants;
    uint8_t* m_frameConstantsMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialConstants;
    SpatialCulling m_spatialObjects;
};
