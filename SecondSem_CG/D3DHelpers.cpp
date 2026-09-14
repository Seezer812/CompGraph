#include "D3DHelpers.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace D3DHelpers
{

void CompileShader(const wchar_t* path, const char* entryPoint, const char* target, ComPtr<ID3DBlob>& output)
{
    ComPtr<ID3DBlob> error;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, target, flags, 0, &output, &error);
    if (SUCCEEDED(hr)) return;
    if (error) OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
    MessageBoxW(nullptr, L"Не удалось скомпилировать HLSL-шейдер.", L"SecondSem CG", MB_OK | MB_ICONERROR);
    std::exit(static_cast<int>(hr));
}

ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, const void* data, UINT64 size)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer));
    if (FAILED(hr)) std::exit(static_cast<int>(hr));
    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    hr = buffer->Map(0, &readRange, &mapped);
    if (FAILED(hr)) std::exit(static_cast<int>(hr));
    if (data) std::memcpy(mapped, data, static_cast<size_t>(size));
    else std::memset(mapped, 0, static_cast<size_t>(size));
    buffer->Unmap(0, nullptr);
    return buffer;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

} // namespace D3DHelpers
