#pragma once

#include <d3d12.h>
#include <wrl/client.h>

namespace D3DHelpers
{

void CompileShader(const wchar_t* path, const char* entryPoint, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& output);

Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(
    ID3D12Device* device,
    const void* data,
    UINT64 size);

D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after);

} // namespace D3DHelpers
