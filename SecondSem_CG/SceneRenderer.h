#pragma once

// SceneRenderer отвечает только за содержимое сцены: OBJ/MTL, текстуры,
// GPU-буферы материалов и отрисовку Sponza. Он не управляет окном,
// swap-chain или циклом приложения.

#include <d3d12.h>
#include <DirectXMath.h>
#include <filesystem>
#include <vector>
#include <wrl/client.h>

#include "ObjLoader.h"

class SceneRenderer
{
public:
    bool Load(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        ID3D12CommandAllocator* uploadAllocator,
        ID3D12GraphicsCommandList* uploadCommands,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvDescriptorSize,
        const std::filesystem::path& objPath);

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        float timeSeconds,
        bool tessellationEnabled) const;

    // Depth-only draw used by every CSM cascade. It intentionally uses the base mesh:
    // shadow silhouette remains stable even when optional tessellation is disabled.
    void DrawShadow(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& lightViewProjection,
        UINT cascade) const;

    bool IsReady() const { return m_ready; }

private:
    bool m_ready = false;
    Obj::LoadedMesh m_mesh;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_INDEX_BUFFER_VIEW m_indexView{};
    std::vector<UINT> m_materialSrvBase;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_textures;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_uploads;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialConstants;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_frameConstants;
    uint8_t* m_frameConstantsMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowConstants;
    uint8_t* m_shadowConstantsMapped = nullptr;
    UINT m_srvDescriptorSize = 0;
};
