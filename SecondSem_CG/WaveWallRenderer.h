#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

// A small procedural wall whose cells are submitted as four-control-point patches.
class WaveWallRenderer
{
public:
    bool Initialize(ID3D12Device* device);
    void Draw(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature, ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection, float timeSeconds) const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_frameConstants;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialConstants;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_INDEX_BUFFER_VIEW m_indexView{};
    void* m_frameConstantsMapped = nullptr;
    UINT m_indexCount = 0;
};
