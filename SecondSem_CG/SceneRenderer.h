#pragma once


#include <d3d12.h>
#include <DirectXMath.h>
#include <filesystem>
#include <vector>
#include <wrl/client.h>

#include "ObjLoader.h"

class SceneRenderer
{
public:
    void BeginFrame(UINT frameIndex) { m_frameIndex = frameIndex; m_nextFrameConstantSlot = 0; }
    bool Load(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        ID3D12CommandAllocator* uploadAllocator,
        ID3D12GraphicsCommandList* uploadCommands,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvDescriptorSize,
        const std::filesystem::path& objPath,
        UINT materialSrvLimit);

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& cameraPosition,
        float timeSeconds,
        bool tessellationEnabled) const;

    void DrawShadow(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& lightViewProjection,
        UINT cascade) const;

    void DrawPointShadow(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const DirectX::XMMATRIX& lightViewProjection,
        UINT face,
        const DirectX::XMFLOAT4& lightPositionRange) const;

    bool GetShadowCasterBounds(DirectX::XMFLOAT3& minimum, DirectX::XMFLOAT3& maximum) const;

    bool IsReady() const { return m_ready; }

private:
    UINT m_frameIndex = 0;
    bool m_ready = false;
    bool m_isPbrModel = false;
    DirectX::XMFLOAT4X4 m_world{};
    Obj::LoadedMesh m_mesh;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_INDEX_BUFFER_VIEW m_indexView{};
    std::vector<UINT> m_materialSrvBase;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_textures;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialConstants;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_frameConstants;
    uint8_t* m_frameConstantsMapped = nullptr;
    mutable UINT m_nextFrameConstantSlot = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowConstants;
    uint8_t* m_shadowConstantsMapped = nullptr;
    DirectX::XMFLOAT3 m_shadowCasterBoundsMin{};
    DirectX::XMFLOAT3 m_shadowCasterBoundsMax{};
    bool m_hasShadowCasterBounds = false;
    UINT m_srvDescriptorSize = 0;
};
