#include "ShadowMap.h"
#include "D3DHelpers.h"

#include <cstdlib>

using Microsoft::WRL::ComPtr;

void ShadowMap::Init(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT srvStart, UINT srvIncrement)
{
    D3D12_DESCRIPTOR_HEAP_DESC dh{};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = CascadeCount;
    if (FAILED(device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_dsvHeap)))) std::exit(EXIT_FAILURE);
    m_dsvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = Resolution; rd.Height = Resolution; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_TYPELESS; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{}; clear.Format = DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth = 1.0f;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE srv = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(srvStart) * srvIncrement;
    for (UINT i = 0; i < CascadeCount; ++i)
    {
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_maps[i])))) std::exit(EXIT_FAILURE);
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(m_maps[i].Get(), &dv, dsv);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_maps[i].Get(), &sv, srv);
        dsv.ptr += m_dsvIncrement; srv.ptr += srvIncrement;
    }
}

void ShadowMap::TransitionToDepthWrite(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER bars[CascadeCount];
    for (UINT i = 0; i < CascadeCount; ++i) bars[i] = D3DHelpers::Transition(m_maps[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(CascadeCount, bars);
}
void ShadowMap::TransitionToShaderResource(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER bars[CascadeCount];
    for (UINT i = 0; i < CascadeCount; ++i) bars[i] = D3DHelpers::Transition(m_maps[i].Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(CascadeCount, bars);
}
D3D12_CPU_DESCRIPTOR_HANDLE ShadowMap::Dsv(UINT cascade) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_dsvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += static_cast<SIZE_T>(cascade) * m_dsvIncrement; return h;
}
