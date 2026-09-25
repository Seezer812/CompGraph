#pragma once

#include <array>
#include <d3d12.h>
#include <wrl/client.h>

// Radial-distance shadow cubemap for a point light.
class PointShadowMap
{
public:
    static constexpr UINT FaceCount = 6;
    static constexpr UINT Resolution = 1024;

    void Init(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT srvIndex, UINT srvIncrement);
    void TransitionToRenderTarget(ID3D12GraphicsCommandList* commandList);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* commandList);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT face) const;
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT face) const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_distanceCube;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthCube;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_rtvIncrement = 0;
    UINT m_dsvIncrement = 0;
};
