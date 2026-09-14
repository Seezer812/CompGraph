#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "RenderingSystem.h"

class RainSphereRenderer
{
public:
    HRESULT Initialize(ID3D12Device* device);

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const std::vector<RenderingSystem::RainLight>& drops,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        float timeSeconds);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_INDEX_BUFFER_VIEW m_indexView{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_frameConstants;
    uint8_t* m_frameConstantsMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialConstants;
};
