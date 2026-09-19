#include "TextureUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wincodec.h>

#include <algorithm>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace Tex
{
namespace
{

void CreateSrv(
    ID3D12Device* device,
    ID3D12Resource* tex,
    ID3D12DescriptorHeap* heap,
    UINT heapIndex,
    UINT descriptorIncrement)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(heapIndex) * descriptorIncrement;
    device->CreateShaderResourceView(tex, &srv, h);
}

void AppendTransition(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &b);
}

bool CopyBufferToTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* dstTex,
    const D3D12_RESOURCE_DESC& desc,
    const void* srcPixels,
    UINT srcRowPitch,
    std::vector<ComPtr<ID3D12Resource>>& uploadKeep)
{
    UINT64 uploadSize = 0;
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &numRows, &rowSizeBytes, &uploadSize);

    D3D12_HEAP_PROPERTIES uhp{};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub{};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = uploadSize;
    ub.Height = 1;
    ub.DepthOrArraySize = 1;
    ub.MipLevels = 1;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    HRESULT hr = device->CreateCommittedResource(
        &uhp, D3D12_HEAP_FLAG_NONE, &ub, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr))
        return false;

    BYTE* mapped = nullptr;
    hr = upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr))
        return false;

    const BYTE* src = static_cast<const BYTE*>(srcPixels);
    for (UINT row = 0; row < numRows; ++row)
    {
        BYTE* dstRow = mapped + layout.Offset + row * layout.Footprint.RowPitch;
        if (layout.Footprint.RowPitch > 0)
            memset(dstRow, 0, static_cast<size_t>(layout.Footprint.RowPitch));
        const BYTE* srcRow = src + row * srcRowPitch;
        memcpy(dstRow, srcRow, static_cast<size_t>(rowSizeBytes));
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = dstTex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = layout;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    uploadKeep.push_back(std::move(upload));
    return true;
}

static bool CiStartsWith(const std::wstring& s, const std::wstring& pref)
{
    if (s.size() < pref.size())
        return false;
    for (size_t i = 0; i < pref.size(); ++i)
    {
        if (towlower(static_cast<wint_t>(s[i])) != towlower(static_cast<wint_t>(pref[i])))
            return false;
    }
    return true;
}

static std::wstring InnerPathUnderTexturesFolder(const std::wstring& mapRel)
{
    namespace fs = std::filesystem;
    std::wstring s = mapRel;
    for (wchar_t& c : s)
    {
        if (c == L'/')
            c = L'\\';
    }

    static const std::wstring prefixes[] = {L"textures\\", L".\\textures\\"};
    for (const auto& pref : prefixes)
    {
        if (CiStartsWith(s, pref))
        {
            s.erase(0, pref.size());
            break;
        }
    }
    while (!s.empty() && s.front() == L'\\')
        s.erase(s.begin());

    if (s.empty())
        return fs::path(mapRel).filename().wstring();

    return s;
}

static HRESULT CreateWicDecoderForFile(
    IWICImagingFactory* factory,
    const wchar_t* path,
    ComPtr<IWICBitmapDecoder>& outDecoder)
{
    HRESULT hr = factory->CreateDecoderFromFilename(
        path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &outDecoder);
    if (SUCCEEDED(hr))
        return hr;

    outDecoder.Reset();

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr))
        return hr;

    hr = stream->InitializeFromFilename(path, GENERIC_READ);
    if (FAILED(hr))
        return hr;

    return factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &outDecoder);
}

#pragma pack(push, 1)
struct TgaHeader
{
    uint8_t idLength;
    uint8_t colorMapType;
    uint8_t imageType;
    uint16_t colorMapOrigin;
    uint16_t colorMapLength;
    uint8_t colorMapDepth;
    uint16_t xOrigin;
    uint16_t yOrigin;
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
    uint8_t imageDescriptor;
};
#pragma pack(pop)

static bool ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const auto end = f.tellg();
    if (end <= 0)
        return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(end));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(end)));
}

static void BgrPixelToRgba(uint8_t* dst, const uint8_t* src, unsigned bpp)
{
    if (bpp == 32)
    {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = src[3];
    }
    else if (bpp == 24)
    {
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = 255;
    }
}

static bool DecodeTgaTruecolor(
    const std::vector<uint8_t>& file,
    std::vector<uint8_t>& rgba,
    UINT& outW,
    UINT& outH,
    std::wstring& error)
{
    error.clear();
    if (file.size() < sizeof(TgaHeader))
        return false;

    TgaHeader h{};
    std::memcpy(&h, file.data(), sizeof(h));

    if (h.imageType != 2 && h.imageType != 10)
        return false;

    if (h.colorMapType != 0)
    {
        error = L"TGA: индексированные палитры не поддерживаются";
        return false;
    }

    const unsigned bpp = h.bpp;
    if (bpp != 24 && bpp != 32)
    {
        error = L"TGA: поддерживаются только 24/32 bpp";
        return false;
    }

    const UINT w = h.width;
    const UINT ht = h.height;
    if (w == 0 || ht == 0)
    {
        error = L"TGA: нулевой размер";
        return false;
    }

    const unsigned bytesPerPixel = bpp / 8;
    const size_t pixelCount = static_cast<size_t>(w) * ht;
    const bool topOrigin = (h.imageDescriptor & 0x20) != 0;

    const size_t dataOff = sizeof(TgaHeader) + h.idLength;

    std::vector<uint8_t> raw(pixelCount * bytesPerPixel);

    if (h.imageType == 2)
    {
        const size_t need = dataOff + raw.size();
        if (file.size() < need)
        {
            error = L"TGA: обрезанный файл";
            return false;
        }
        std::memcpy(raw.data(), file.data() + dataOff, raw.size());
    }
    else
    {
        size_t p = dataOff;
        size_t o = 0;
        const size_t target = raw.size();
        while (o < target)
        {
            if (p >= file.size())
            {
                error = L"TGA RLE: нехватает данных";
                return false;
            }
            const uint8_t pkt = file[p++];
            const uint32_t count = static_cast<uint32_t>(pkt & 0x7Fu) + 1u;
            const size_t chunk = static_cast<size_t>(count) * bytesPerPixel;
            if (pkt & 0x80u)
            {
                if (p + bytesPerPixel > file.size())
                {
                    error = L"TGA RLE: обрезанный пакет";
                    return false;
                }
                const uint8_t* px = file.data() + p;
                p += bytesPerPixel;
                if (o + chunk > target)
                {
                    error = L"TGA RLE: переполнение";
                    return false;
                }
                for (uint32_t i = 0; i < count; ++i)
                    std::memcpy(raw.data() + o + static_cast<size_t>(i) * bytesPerPixel, px, bytesPerPixel);
                o += chunk;
            }
            else
            {
                if (p + chunk > file.size())
                {
                    error = L"TGA RLE: сырые данные";
                    return false;
                }
                if (o + chunk > target)
                {
                    error = L"TGA RLE: переполнение";
                    return false;
                }
                std::memcpy(raw.data() + o, file.data() + p, chunk);
                o += chunk;
                p += chunk;
            }
        }
    }

    rgba.resize(pixelCount * 4);
    for (UINT y = 0; y < ht; ++y)
    {
        const UINT srcY = topOrigin ? y : (ht - 1u - y);
        for (UINT x = 0; x < w; ++x)
        {
            const size_t si = (static_cast<size_t>(srcY) * w + x) * bytesPerPixel;
            const size_t di = (static_cast<size_t>(y) * w + x) * 4u;
            BgrPixelToRgba(rgba.data() + di, raw.data() + si, bpp);
        }
    }

    outW = w;
    outH = ht;
    return true;
}

static bool TryDecodeTgaFile(
    const std::filesystem::path& filePath,
    std::vector<uint8_t>& outPixels,
    UINT& outW,
    UINT& outH,
    std::wstring& error)
{
    std::vector<uint8_t> file;
    if (!ReadWholeFile(filePath, file))
        return false;
    return DecodeTgaTruecolor(file, outPixels, outW, outH, error);
}

static bool DecodeImageFileToRgba32(
    IWICImagingFactory* factory,
    const std::filesystem::path& filePath,
    std::vector<uint8_t>& outPixels,
    UINT& outW,
    UINT& outH,
    std::wstring& error)
{
    outPixels.clear();
    outW = outH = 0;

    std::wstring tgaErr;
    if (TryDecodeTgaFile(filePath, outPixels, outW, outH, tgaErr))
        return true;

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = CreateWicDecoderForFile(factory, filePath.c_str(), decoder);
    if (FAILED(hr))
    {
        if (!tgaErr.empty())
            error = tgaErr;
        else
            error = L"Декодер (WIC): " + filePath.wstring();
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        error = L"Кадр текстуры";
        return false;
    }

    ComPtr<IWICFormatConverter> conv;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr))
    {
        error = L"FormatConverter";
        return false;
    }

    hr = conv->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr))
    {
        error = L"Initialize converter (формат файла?)";
        return false;
    }

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0)
    {
        error = L"Нулевой размер текстуры";
        return false;
    }

    const UINT rowPitch = w * 4;
    const UINT imageSize = rowPitch * h;
    outPixels.resize(imageSize);
    hr = conv->CopyPixels(nullptr, rowPitch, imageSize, outPixels.data());
    if (FAILED(hr))
    {
        error = L"CopyPixels";
        return false;
    }

    outW = w;
    outH = h;
    return true;
}

} // namespace

bool CreateTextureFromDds(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap, UINT heapIndex, UINT descriptorIncrement,
    const std::filesystem::path& filePath, bool expectCube,
    ComPtr<ID3D12Resource>& outTexture, std::vector<ComPtr<ID3D12Resource>>& uploadKeep,
    std::wstring& error)
{
    struct DdsHeader { uint32_t size, flags, height, width, pitch, depth, mipCount, reserved1[11];
        uint32_t pfSize, pfFlags, fourCC, rgbBits, rMask, gMask, bMask, aMask, caps, caps2, caps3, caps4, reserved2; };
    struct DdsDx10 { DXGI_FORMAT format; D3D12_RESOURCE_DIMENSION dimension; uint32_t miscFlag, arraySize, miscFlags2; };
    constexpr uint32_t kDds = 0x20534444u, kDx10 = 0x30315844u, kCube = 0x4u;
    std::ifstream in(filePath, std::ios::binary | std::ios::ate);
    if (!in) { error = L"DDS file: " + filePath.wstring(); return false; }
    const size_t size = static_cast<size_t>(in.tellg());
    std::vector<uint8_t> bytes(size); in.seekg(0); in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (size < 4 + sizeof(DdsHeader) + sizeof(DdsDx10) || *reinterpret_cast<const uint32_t*>(bytes.data()) != kDds)
    { error = L"Invalid DDS header"; return false; }
    const auto* h = reinterpret_cast<const DdsHeader*>(bytes.data() + 4);
    if (h->size != 124 || h->fourCC != kDx10) { error = L"DDS requires DX10 header"; return false; }
    const auto* dx10 = reinterpret_cast<const DdsDx10*>(bytes.data() + 4 + sizeof(DdsHeader));
    const bool isCube = (dx10->miscFlag & kCube) != 0;
    if (expectCube != isCube || h->width == 0 || h->height == 0 || dx10->arraySize == 0)
    { error = L"Unexpected DDS texture type"; return false; }
    const UINT mipCount = (std::max)(1u, h->mipCount);
    const UINT arraySize = dx10->arraySize * (isCube ? 6u : 1u);
    const bool blockCompressed = dx10->format == DXGI_FORMAT_BC6H_UF16 || dx10->format == DXGI_FORMAT_BC6H_SF16;
    const UINT bytesPerPixel = dx10->format == DXGI_FORMAT_R32G32_FLOAT ? 8u : 0u;
    if (!blockCompressed && bytesPerPixel == 0) { error = L"Unsupported DDS format"; return false; }

    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = h->width; rd.Height = h->height; rd.DepthOrArraySize = static_cast<UINT16>(arraySize);
    rd.MipLevels = static_cast<UINT16>(mipCount); rd.Format = dx10->format; rd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES defaultHeap{}; defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTexture)))) { error = L"DDS GPU resource"; return false; }

    const UINT subresources = mipCount * arraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources);
    std::vector<UINT> rows(subresources); std::vector<UINT64> rowSizes(subresources); UINT64 uploadBytes = 0;
    device->GetCopyableFootprints(&rd, 0, subresources, 0, layouts.data(), rows.data(), rowSizes.data(), &uploadBytes);
    D3D12_HEAP_PROPERTIES uploadHeap{}; uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = uploadBytes;
    buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) { error = L"DDS upload buffer"; return false; }
    uint8_t* mapped = nullptr; D3D12_RANGE noRead{0, 0}; upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    size_t sourceOffset = 4 + sizeof(DdsHeader) + sizeof(DdsDx10);
    for (UINT array = 0; array < arraySize; ++array) for (UINT mip = 0; mip < mipCount; ++mip) {
        const UINT sub = array * mipCount + mip; const UINT w = (std::max)(1u, h->width >> mip), ht = (std::max)(1u, h->height >> mip);
        const UINT srcPitch = blockCompressed ? ((w + 3) / 4) * 16u : w * bytesPerPixel;
        const UINT srcRows = blockCompressed ? (ht + 3) / 4 : ht;
        const size_t srcBytes = static_cast<size_t>(srcPitch) * srcRows;
        if (sourceOffset + srcBytes > bytes.size()) { upload->Unmap(0, nullptr); error = L"Truncated DDS data"; return false; }
        uint8_t* dstMapped = mapped + layouts[sub].Offset;
        for (UINT row = 0; row < srcRows; ++row) std::memcpy(dstMapped + row * layouts[sub].Footprint.RowPitch, bytes.data() + sourceOffset + row * srcPitch, srcPitch);
        sourceOffset += srcBytes;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = layouts[sub];
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = outTexture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = sub;
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    upload->Unmap(0, nullptr); uploadKeep.push_back(upload);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {outTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    cmdList->ResourceBarrier(1, &barrier);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format = dx10->format; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = isCube ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURE2D;
    if (isCube) srv.TextureCube.MipLevels = mipCount; else srv.Texture2D.MipLevels = mipCount;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += static_cast<SIZE_T>(heapIndex) * descriptorIncrement;
    device->CreateShaderResourceView(outTexture.Get(), &srv, handle);
    return true;
}

std::filesystem::path ResolveTexturePathInTexturesFolder(
    const std::filesystem::path& mtlDir,
    const std::wstring& mapRelFromMtl)
{
    namespace fs = std::filesystem;
    if (mapRelFromMtl.empty())
        return {};

    const std::wstring innerW = InnerPathUnderTexturesFolder(mapRelFromMtl);
    const fs::path inner(innerW);
    const fs::path fname = fs::path(mapRelFromMtl).filename();

    const std::wstring stemInner = inner.stem().wstring();
    const std::wstring stemFname = fname.stem().wstring();

    const fs::path texRoots[] = {
        mtlDir / L"textures",
        mtlDir / L"Textures",
    };

    std::vector<fs::path> relAttempts;
    relAttempts.reserve(8);

    auto pushAttempt = [&](const fs::path& rel) {
        if (rel.empty())
            return;
        const fs::path norm = rel.lexically_normal();
        if (std::find(relAttempts.begin(), relAttempts.end(), norm) != relAttempts.end())
            return;
        relAttempts.push_back(norm);
    };

    // 1) путь как в MTL (относительно textures/)
    pushAttempt(inner);
    // 2) та же подпапка, имя как stem + .tga (MTL мог указать .jpg/png)
    pushAttempt(inner.parent_path() / (stemInner + L".tga"));
    // 3) только stem.tga в корне textures/
    pushAttempt(fs::path(stemInner + L".tga"));
    pushAttempt(fname);
    pushAttempt(fname.parent_path() / (stemFname + L".tga"));
    pushAttempt(fs::path(stemFname + L".tga"));

    for (const fs::path& root : texRoots)
    {
        for (const fs::path& rel : relAttempts)
        {
            std::error_code ec;
            fs::path cand = (root / rel).lexically_normal();
            if (!cand.empty() && fs::exists(cand, ec))
                return cand;
        }
    }

    return (texRoots[0] / inner.parent_path() / (stemInner + L".tga")).lexically_normal();
}

void WriteTexture2DSrv(
    ID3D12Device* device,
    ID3D12Resource* texture,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(heapIndex) * descriptorIncrement;
    device->CreateShaderResourceView(texture, &srv, h);
}

bool CreateSolidTexture2D(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement,
    uint32_t rgba8Unorm,
    ComPtr<ID3D12Resource>& outTexture,
    std::vector<ComPtr<ID3D12Resource>>& uploadKeep)
{
    outTexture.Reset();

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 1;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;

    HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTexture));
    if (FAILED(hr))
        return false;

    if (!CopyBufferToTexture(device, cmdList, outTexture.Get(), rd, &rgba8Unorm, 4, uploadKeep))
        return false;

    AppendTransition(cmdList, outTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CreateSrv(device, outTexture.Get(), srvHeap, heapIndex, descriptorIncrement);
    return true;
}

bool CreateTexture2DFromFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement,
    const std::filesystem::path& filePath,
    ComPtr<ID3D12Resource>& outTexture,
    std::vector<ComPtr<ID3D12Resource>>& uploadKeep,
    std::wstring& error)
{
    outTexture.Reset();
    error.clear();

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        error = L"WIC factory";
        return false;
    }

    std::vector<uint8_t> pixels;
    UINT w = 0, h = 0;
    if (!DecodeImageFileToRgba32(factory.Get(), filePath, pixels, w, h, error))
        return false;

    const UINT rowPitch = w * 4;

    D3D12_HEAP_PROPERTIES thp{};
    thp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;

    hr = device->CreateCommittedResource(
        &thp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTexture));
    if (FAILED(hr))
        return false;

    if (!CopyBufferToTexture(device, cmdList, outTexture.Get(), rd, pixels.data(), rowPitch, uploadKeep))
        return false;

    AppendTransition(cmdList, outTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CreateSrv(device, outTexture.Get(), srvHeap, heapIndex, descriptorIncrement);
    return true;
}

} // namespace Tex
