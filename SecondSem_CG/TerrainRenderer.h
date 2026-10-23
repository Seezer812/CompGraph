#pragma once
#include "TerrainData.h"
#include <d3d12.h>
#include <wrl/client.h>

class TerrainRenderer
{
public:
    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
        const std::filesystem::path& directory, const std::filesystem::path& shader, std::wstring& error);
    void Draw(ID3D12GraphicsCommandList* cmd, const DirectX::XMMATRIX& vp,
        const DirectX::XMMATRIX& selectionVp, const DirectX::XMFLOAT3& selectionEye,
        UINT height, bool wireframe, bool culling, bool lodColors);
    bool Ready() const { return m_ready; }
    const TerrainData::Stats& Statistics() const { return m_data.Statistics(); }
private:
    TerrainData m_data;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertices, m_indices, m_color;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_solid, m_wire;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_INDEX_BUFFER_VIEW m_indexView{};
    bool m_ready = false;
};
