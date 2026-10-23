#include "TerrainChecks.h"
#include "TerrainData.h"
#include "D3DHelpers.h"
#include <d3d12sdklayers.h>
#include <wincodec.h>
#include <fstream>
#include <cstring>

using Microsoft::WRL::ComPtr;

bool TerrainChecks::Assets(const std::filesystem::path& directory, std::wstring& error)
{
    TerrainData data;
    if (!data.Load(directory / L"HeightMap.png", error) || !data.Validate(error)) return false;
    TerrainData rejected;
    std::wstring expectedError;
    if (rejected.Load(directory / L"Color.png", expectedError)) {
        error = L"Height loader accepted an 8-bit color image"; return false;
    }
    if (rejected.Load(directory / L"__missing_terrain_check__.png", expectedError)) {
        error = L"Height loader accepted a missing file"; return false;
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromFilename((directory / L"Color.png").c_str(),
        nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width != data.Width() || height != data.HeightPixels()) {
        error = L"Color and height dimensions do not match"; return false;
    }
    return true;
}

bool TerrainChecks::Capture(D3D12Context& context, UINT frame, UINT width, UINT height,
    std::vector<uint8_t>& rgba, std::wstring& error)
{
    context.WaitForGpu();
    auto* device = context.device.Get();
    auto* buffer = context.renderTargets[frame].Get();
    const auto description = buffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; resource.Width = bytes;
    resource.Height = 1; resource.DepthOrArraySize = 1; resource.MipLevels = 1;
    resource.SampleDesc.Count = 1; resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&commands));
    if (FAILED(hr)) { error = L"Cannot allocate frame readback"; return false; }
    auto barrier = D3DHelpers::Transition(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = buffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource = readback.Get();
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; target.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    barrier = D3DHelpers::Transition(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    commands->ResourceBarrier(1, &barrier);
    if (FAILED(commands->Close())) { error = L"Cannot close frame readback"; return false; }
    ID3D12CommandList* lists[] = {commands.Get()};
    context.queue->ExecuteCommandLists(1, lists);
    context.WaitForGpu();
    uint8_t* pixels = nullptr;
    const D3D12_RANGE readRange{0, SIZE_T(bytes)};
    hr = readback->Map(0, &readRange, reinterpret_cast<void**>(&pixels));
    if (FAILED(hr)) { error = L"Cannot map captured frame"; return false; }
    rgba.resize(size_t(width) * height * 4);
    for (UINT y = 0; y < height; ++y)
        std::memcpy(rgba.data() + size_t(y) * width * 4,
            pixels + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, size_t(width) * 4);
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    return true;
}

void TerrainChecks::SavePpm(const std::filesystem::path& path, UINT width, UINT height, const std::vector<uint8_t>& rgba)
{
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << " " << height << "\n255\n";
    for (size_t i = 0; i < rgba.size(); i += 4) file.write(reinterpret_cast<const char*>(rgba.data() + i), 3);
}

UINT TerrainChecks::GpuErrors(ID3D12Device* device)
{
    UINT errors = 0;
    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info)))) {
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(info->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                ++errors;
                OutputDebugStringA(message->pDescription);
            }
        }
    }
    return errors;
}
