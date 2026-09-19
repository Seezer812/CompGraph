#pragma once

// G-buffer (Geometry buffer) — набор вспомогательных рендер-таргетов для отложенного освещения:
// в геопроходе сюда пишутся альбедо, нормали и глубина; позиция мира восстанавливается в lighting pass.

#include <d3d12.h>
#include <wrl/client.h>

class GBuffer
{
public:
    void Init(ID3D12Device* device, UINT width, UINT height);
    void Resize(ID3D12Device* device, UINT width, UINT height);

    void CreateShaderResourceViews(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvStartIndex,
        UINT srvDescriptorSize);

    void TransitionToRenderTargets(ID3D12GraphicsCommandList* cmd);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmd);

    void ClearAndSetAsRenderTarget(
        ID3D12GraphicsCommandList* cmd,
        const float clearRgb[4]);

private:
    void DestroySizeDependent();
    void CreateTargets(ID3D12Device* device, UINT width, UINT height);

    UINT m_w = 0;
    UINT m_h = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_albedo;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normal;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_rtvInc = 0;

    enum : int
    {
        kRtvCount = 3
    };
};
