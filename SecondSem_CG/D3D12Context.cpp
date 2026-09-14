#include "D3D12Context.h"

#include <cstdlib>

namespace
{
void Check(HRESULT result)
{
    if (FAILED(result))
        std::exit(static_cast<int>(result));
}
}

void D3D12Context::WaitForGpu()
{
    const UINT64 value = ++fenceValue;
    Check(queue->Signal(fence.Get(), value));
    if (fence->GetCompletedValue() < value)
    {
        Check(fence->SetEventOnCompletion(value, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

void D3D12Context::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < FrameCount; ++index)
    {
        Check(swapChain->GetBuffer(index, IID_PPV_ARGS(&renderTargets[index])));
        device->CreateRenderTargetView(renderTargets[index].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize;
    }
}

void D3D12Context::CreateDepthBuffer(UINT width, UINT height)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&depthStencil)));

    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_D32_FLOAT;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depthStencil.Get(), &view, dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void D3D12Context::Resize(UINT width, UINT height)
{
    if (!swapChain || width == 0 || height == 0)
        return;
    WaitForGpu();
    depthStencil.Reset();
    for (UINT index = 0; index < FrameCount; ++index)
    {
        renderTargets[index].Reset();
        swapSeenPresent[index] = false;
        frameFenceValues[index] = fence->GetCompletedValue();
    }
    DXGI_SWAP_CHAIN_DESC description{};
    Check(swapChain->GetDesc(&description));
    Check(swapChain->ResizeBuffers(FrameCount, width, height, description.BufferDesc.Format, description.Flags));
    CreateRenderTargets();
    CreateDepthBuffer(width, height);
}

void D3D12Context::Shutdown()
{
    if (fence)
        WaitForGpu();
    if (fenceEvent)
    {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
}
