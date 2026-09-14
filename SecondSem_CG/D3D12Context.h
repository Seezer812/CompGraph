#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// Все ресурсы, принадлежащие жизненному циклу Direct3D 12.
// Рендереры сцены и освещения не владеют этими объектами.
struct D3D12Context
{
    static constexpr UINT FrameCount = 2;

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvDescriptorSize = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> renderTargets[FrameCount];

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
    UINT64 frameFenceValues[FrameCount]{};
    bool swapSeenPresent[FrameCount]{};

    void WaitForGpu();
    void CreateRenderTargets();
    void CreateDepthBuffer(UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void Shutdown();
};
