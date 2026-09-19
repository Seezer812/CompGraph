#pragma once

#include <array>
#include <d3d12.h>
#include <wrl/client.h>

// Four depth maps for cascaded directional-light shadows.
class ShadowMap
{
public:
    static constexpr UINT CascadeCount = 4;
    static constexpr UINT Resolution = 2048;

    void Init(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT srvStart, UINT srvIncrement);
    void TransitionToDepthWrite(ID3D12GraphicsCommandList* cmd);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmd);
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT cascade) const;

private:
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, CascadeCount> m_maps;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_dsvIncrement = 0;
};
