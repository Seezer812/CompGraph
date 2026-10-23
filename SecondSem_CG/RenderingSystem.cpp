#include "RenderingSystem.h"
#include "D3D12Context.h"
#include "D3DHelpers.h"
#include "TextureUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{

enum LightType : UINT
{
    LIGHT_DIR = 0,
};

constexpr UINT kLightingCbStride = 512;
constexpr UINT kPostProcessCbStride = 256;

struct LightGpu
{
    XMFLOAT4 position_range{};
    XMFLOAT4 direction_cosOuter{};
    XMFLOAT4 color_intensity{};
    UINT type = LIGHT_DIR;
    float spotCosInner = 0.f;
    UINT pad[2]{};
};

static_assert(sizeof(LightGpu) == 64);

struct LightingCBGPU
{
    XMFLOAT4 cameraPos_pad{};
    XMFLOAT4 invScreen_pad{};
    XMFLOAT4X4 inverseViewProjection{};
    XMFLOAT4 shadowSettings{}; // x: environment max LOD, w: shadows enabled
    XMFLOAT4 cascadeSplits{};
    XMFLOAT4X4 cascadeMatrices[4]{};
    UINT lightCount = 0;
    UINT tileCountX = 0;
    UINT shadowMapDebugIndex = 0;
    UINT cascadeColorDebug = 0;
    LightGpu sun{};
};

static_assert(sizeof(LightingCBGPU) == 464);
static_assert(kLightingCbStride >= sizeof(LightingCBGPU) && kLightingCbStride % 256 == 0);

struct PostProcessCBGPU
{
    XMFLOAT4 invScreen_pad{};
    UINT vignetteEnabled = 0;
    UINT flyEyeEnabled = 0;
    UINT toneMappingEnabled = 1;
    UINT pad = 0;
};

static_assert(sizeof(PostProcessCBGPU) == 32);
static_assert(kPostProcessCbStride >= sizeof(PostProcessCBGPU) && kPostProcessCbStride % 256 == 0);

void RSCompile(const wchar_t* path, const char* entry, const char* target, ComPtr<ID3DBlob>& out)
{
    ComPtr<ID3DBlob> err;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &out, &err);
    if (FAILED(hr))
    {
        if (err)
            OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
        wchar_t b[96];
        swprintf_s(b, L"Deferred shader compile failed 0x%08X", static_cast<unsigned>(hr));
        MessageBoxW(nullptr, b, L"SecondSem CG", MB_OK | MB_ICONERROR);
        std::exit(static_cast<int>(hr));
    }
}

static ComPtr<ID3D12Resource> CreateUploadCb(ID3D12Device* device, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> buf;
    HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"Lighting CB", L"SecondSem CG", MB_OK | MB_ICONERROR);
        std::exit(static_cast<int>(hr));
    }
    return buf;
}

} // namespace

void RenderingSystem::WriteDefaultLights()
{
    LightingCBGPU cb{};
    XMVECTOR sunDir = XMVector3Normalize(XMVectorSet(0.18f, -0.96f, -0.22f, 0.f));
    cb.sun.type = LIGHT_DIR;
    XMStoreFloat4(&cb.sun.direction_cosOuter, sunDir);
    cb.sun.direction_cosOuter.w = 0.f;
    cb.sun.color_intensity = XMFLOAT4(1.0f, 0.93f, 0.80f, 7.0f);

    for (UINT frame = 0; frame < D3D12Context::FrameCount; ++frame)
        std::memcpy(m_lightingCBMapped + frame * kLightingCbStride, &cb, sizeof(cb));
}

void RenderingSystem::CreateLightingPipeline(ID3D12Device* device, const wchar_t* hlslPath)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 15; // G-buffer, CSM, IBL, point shadow and four cascade debug textures.
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[5]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Root SRVs avoid rewriting descriptors that an in-flight frame uses.
    for (UINT i = 2; i < 5; ++i)
    {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor.ShaderRegister = 15 + i - 2;
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSamp = samp;
    shadowSamp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSamp.AddressU = shadowSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSamp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSamp.ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = _countof(params);
    rs.pParameters = params;
    D3D12_STATIC_SAMPLER_DESC samplers[] = {samp, shadowSamp};
    rs.NumStaticSamplers = 2;
    rs.pStaticSamplers = samplers;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &rsErr);
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));
    hr = device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSigLight));
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));

    ComPtr<ID3DBlob> vs, ps;
    RSCompile(hlslPath, "LightingFullscreenVS", "vs_5_0", vs);
    RSCompile(hlslPath, "LightingPS", "ps_5_0", ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSigLight.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.InputLayout.NumElements = 0;
    pso.InputLayout.pInputElementDescs = nullptr;

    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoLight));
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));
}

void RenderingSystem::CreatePostProcessPipeline(ID3D12Device* device, const wchar_t* hlslPath)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = _countof(params);
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &sampler;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) std::exit(static_cast<int>(hr));
    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSigPostProcess));
    if (FAILED(hr)) std::exit(static_cast<int>(hr));

    ComPtr<ID3DBlob> vs, ps;
    RSCompile(hlslPath, "PostProcessFullscreenVS", "vs_5_0", vs);
    RSCompile(hlslPath, "PostProcessPS", "ps_5_0", ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSigPostProcess.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoPostProcess));
    if (FAILED(hr)) std::exit(static_cast<int>(hr));
}

void RenderingSystem::CreateLightingTarget(ID3D12Device* device, UINT width, UINT height)
{
    m_lightingWidth = width;
    m_lightingHeight = height;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    clear.Color[3] = 1.0f;
    HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clear, IID_PPV_ARGS(&m_lightingTarget));
    if (FAILED(hr)) std::exit(static_cast<int>(hr));

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = desc.Format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_lightingTarget.Get(), &rtv, m_lightingRtvHeap->GetCPUDescriptorHandleForHeapStart());
}

void RenderingSystem::CreateLightingTargetSrv(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_postProcessSrvIndex) * m_srvDescriptorIncrement;
    device->CreateShaderResourceView(m_lightingTarget.Get(), &srv, handle);
}

void RenderingSystem::Init(
    ID3D12Device* device,
    UINT width,
    UINT height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    UINT gbufferSrvStartIndex,
    UINT srvDescriptorIncrement,
    const wchar_t* deferredHlslPath,
    const wchar_t* postProcessHlslPath)
{
    m_device = device;
    m_gbufferSrvBase = gbufferSrvStartIndex;
    m_iblSrvBase = gbufferSrvStartIndex + 7;
    m_cascadeTextureSrvBase = gbufferSrvStartIndex + 11;
    m_postProcessSrvIndex = gbufferSrvStartIndex + 15;
    m_srvDescriptorIncrement = srvDescriptorIncrement;

    m_gbuffer.Init(device, width, height);
    m_gbuffer.CreateShaderResourceViews(
        device, shaderVisibleSrvHeap, gbufferSrvStartIndex, srvDescriptorIncrement);

    CreateLightingPipeline(device, deferredHlslPath);
    CreatePostProcessPipeline(device, postProcessHlslPath);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
    rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeap.NumDescriptors = 1;
    HRESULT hr = device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(&m_lightingRtvHeap));
    if (FAILED(hr)) std::exit(static_cast<int>(hr));
    CreateLightingTarget(device, width, height);
    CreateLightingTargetSrv(device, shaderVisibleSrvHeap);

    m_lightingCB = CreateUploadCb(device, D3D12Context::FrameCount * kLightingCbStride);
    D3D12_RANGE rr{0, 0};
    hr = m_lightingCB->Map(0, &rr, reinterpret_cast<void**>(&m_lightingCBMapped));
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));

    m_postProcessCB = CreateUploadCb(device, D3D12Context::FrameCount * kPostProcessCbStride);
    hr = m_postProcessCB->Map(0, &rr, reinterpret_cast<void**>(&m_postProcessCBMapped));
    if (FAILED(hr)) std::exit(static_cast<int>(hr));

    WriteDefaultLights();
}

void RenderingSystem::Resize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    UINT srvDescriptorIncrement)
{
    m_srvDescriptorIncrement = srvDescriptorIncrement;
    m_gbuffer.Resize(device, width, height);
    m_gbuffer.CreateShaderResourceViews(
        device, shaderVisibleSrvHeap, m_gbufferSrvBase, srvDescriptorIncrement);
    if (width != m_lightingWidth || height != m_lightingHeight)
        CreateLightingTarget(device, width, height);
    CreateLightingTargetSrv(device, shaderVisibleSrvHeap);
}

void RenderingSystem::UploadFrameConstants(
    const XMFLOAT3& cameraPos,
    const XMMATRIX& viewProjection,
    UINT screenW,
    UINT screenH,
    const std::array<XMMATRIX, 4>& cascadeMatrices,
    const std::array<float, 4>& cascadeSplits,
    bool shadowsEnabled,
    bool shadowDebugView,
    bool cascadeColorDebug,
    bool vignetteEnabled,
    bool flyEyeEnabled,
    UINT shadowMapDebugIndex)
{
    auto* cb = reinterpret_cast<LightingCBGPU*>(m_lightingCBMapped + m_frameIndex * kLightingCbStride);
    cb->cameraPos_pad = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
    const float iw = screenW > 0 ? 1.f / static_cast<float>(screenW) : 1.f;
    const float ih = screenH > 0 ? 1.f / static_cast<float>(screenH) : 1.f;
    cb->invScreen_pad = XMFLOAT4(iw, ih, shadowDebugView ? 1.0f : 0.0f, 0.0f);
    XMStoreFloat4x4(&cb->inverseViewProjection, XMMatrixInverse(nullptr, viewProjection));
    cb->shadowSettings = XMFLOAT4(m_environmentMaxLod, 0, 0, shadowsEnabled ? 1.0f : 0.0f);
    cb->cascadeSplits = XMFLOAT4(cascadeSplits[0], cascadeSplits[1], cascadeSplits[2], cascadeSplits[3]);
    for (UINT i = 0; i < 4; ++i)
        XMStoreFloat4x4(&cb->cascadeMatrices[i], cascadeMatrices[i]);

    UploadLightTiles(viewProjection, screenW, screenH);
    cb->lightCount = static_cast<UINT>(m_pointLights.size());
    cb->tileCountX = (screenW + LightTiling::TileSize - 1) / LightTiling::TileSize;
    cb->shadowMapDebugIndex = shadowMapDebugIndex;
    cb->cascadeColorDebug = cascadeColorDebug ? 1u : 0u;
    auto* postProcessCb = reinterpret_cast<PostProcessCBGPU*>(m_postProcessCBMapped + m_frameIndex * kPostProcessCbStride);
    postProcessCb->invScreen_pad = XMFLOAT4(iw, ih, 0.0f, 0.0f);
    postProcessCb->vignetteEnabled = vignetteEnabled && !shadowDebugView &&
        !cascadeColorDebug && shadowMapDebugIndex == 0 ? 1u : 0u;
    postProcessCb->flyEyeEnabled = flyEyeEnabled ? 1u : 0u;
    postProcessCb->toneMappingEnabled = !shadowDebugView && !cascadeColorDebug && shadowMapDebugIndex == 0 ? 1u : 0u;

}

void RenderingSystem::SetPointLights(std::vector<PointLight> lights)
{
    if (lights.size() > LightTiling::MaxPointLights)
        throw std::length_error("At most 4096 point lights are supported");
    for (const auto& light : lights)
    {
        const auto& p = light.positionRadius;
        const auto& c = light.colorIntensity;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !std::isfinite(p.w) || p.w <= 0.0f ||
            !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) ||
            !std::isfinite(c.w) || c.x < 0 || c.y < 0 || c.z < 0 || c.w < 0)
            throw std::invalid_argument("Point lights require finite position, positive radius and nonnegative linear colour/intensity");
    }
    m_pointLights = std::move(lights);
}

void RenderingSystem::UploadLightTiles(const XMMATRIX& viewProjection, UINT width, UINT height)
{
    const auto grid = LightTiling::Build(m_pointLights, viewProjection, width, height);
    auto& frame = m_lightFrames[m_frameIndex];
    // BeginFrame's fence wait protects these allocations and mapped writes.
    // Allocate only as much as the current list needs; retain capacity thereafter.
    auto upload = [&](ComPtr<ID3D12Resource>& buffer, uint8_t*& mapped, UINT64& capacity,
        const void* source, UINT64 bytes)
    {
        const UINT64 required = (std::max)(256ull, (bytes + 255) & ~255ull);
        if (capacity < required)
        {
            if (buffer) buffer->Unmap(0, nullptr);
            buffer = CreateUploadCb(m_device.Get(), required);
            D3D12_RANGE readRange{0, 0};
            const HRESULT hr = buffer->Map(0, &readRange, reinterpret_cast<void**>(&mapped));
            if (FAILED(hr)) std::exit(static_cast<int>(hr));
            capacity = required;
        }
        if (bytes) std::memcpy(mapped, source, static_cast<size_t>(bytes));
    };
    upload(frame.lights, frame.lightsMapped, frame.lightsCapacity,
        m_pointLights.data(), m_pointLights.size() * sizeof(PointLight));
    upload(frame.ranges, frame.rangesMapped, frame.rangesCapacity,
        grid.ranges.data(), grid.ranges.size() * sizeof(LightTiling::TileRange));
    upload(frame.indices, frame.indicesMapped, frame.indicesCapacity,
        grid.indices.data(), grid.indices.size() * sizeof(uint32_t));
}

bool RenderingSystem::LoadIbl(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* uploadAllocator, ID3D12GraphicsCommandList* uploadCommands,
    ID3D12DescriptorHeap* srvHeap, const std::filesystem::path& assetDirectory,
    const std::filesystem::path& cascadeTextureDirectory)
{
    m_iblTextures.clear(); m_iblUploads.clear();
    if (FAILED(uploadAllocator->Reset()) || FAILED(uploadCommands->Reset(uploadAllocator, nullptr))) return false;
    std::wstring error; ComPtr<ID3D12Resource> irradiance, prefiltered, integration;
    if (!Tex::CreateTextureFromDds(device, uploadCommands, srvHeap, m_iblSrvBase, m_srvDescriptorIncrement,
            assetDirectory / L"IrradianceMap_BC6U.dds", true, irradiance, m_iblUploads, error) ||
        !Tex::CreateTextureFromDds(device, uploadCommands, srvHeap, m_iblSrvBase + 1, m_srvDescriptorIncrement,
            assetDirectory / L"PreFilteredEnvMap_BC6U.dds", true, prefiltered, m_iblUploads, error) ||
        !Tex::CreateTextureFromDds(device, uploadCommands, srvHeap, m_iblSrvBase + 2, m_srvDescriptorIncrement,
            assetDirectory / L"IntegrationMap.dds", false, integration, m_iblUploads, error)) {
        MessageBoxW(nullptr, error.c_str(), L"IBL asset load", MB_OK | MB_ICONERROR); return false;
    }
    for (UINT cascade = 0; cascade < m_cascadeTextures.size(); ++cascade)
    {
        const std::filesystem::path path =
            cascadeTextureDirectory / (L"cascade_" + std::to_wstring(cascade) + L".jpg");
        if (!Tex::CreateTexture2DFromFile(
                device, uploadCommands, srvHeap, m_cascadeTextureSrvBase + cascade,
                m_srvDescriptorIncrement, path, m_cascadeTextures[cascade], m_iblUploads, error))
        {
            const std::wstring message = L"Failed to load cascade texture:\n" + path.wstring() + L"\n" + error;
            MessageBoxW(nullptr, message.c_str(), L"Cascade texture load", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    if (FAILED(uploadCommands->Close())) return false;
    ID3D12CommandList* lists[] = {uploadCommands}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!eventHandle) return false;
    if (FAILED(queue->Signal(fence.Get(), 1)) || FAILED(fence->SetEventOnCompletion(1, eventHandle)) ||
        WaitForSingleObject(eventHandle, INFINITE) != WAIT_OBJECT_0)
    {
        CloseHandle(eventHandle);
        return false;
    }
    CloseHandle(eventHandle);
    m_environmentMaxLod = static_cast<float>(prefiltered->GetDesc().MipLevels - 1);
    m_iblUploads.clear();
    m_iblTextures = {irradiance, prefiltered, integration};
    return true;
}

void RenderingSystem::DrawLightingPass(
    ID3D12GraphicsCommandList* cmd,
    ID3D12DescriptorHeap* srvHeapShaderVisible,
    UINT screenW,
    UINT screenH)
{
    ID3D12DescriptorHeap* heaps[] = {srvHeapShaderVisible};
    cmd->SetDescriptorHeaps(1, heaps);

    cmd->SetGraphicsRootSignature(m_rootSigLight.Get());
    cmd->SetPipelineState(m_psoLight.Get());

    cmd->SetGraphicsRootConstantBufferView(0, m_lightingCB->GetGPUVirtualAddress() + m_frameIndex * kLightingCbStride);

    D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeapShaderVisible->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<SIZE_T>(m_gbufferSrvBase) * static_cast<SIZE_T>(m_srvDescriptorIncrement);
    cmd->SetGraphicsRootDescriptorTable(1, table);

    const auto& frame = m_lightFrames[m_frameIndex];
    cmd->SetGraphicsRootShaderResourceView(2, frame.lights->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(3, frame.ranges->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(4, frame.indices->GetGPUVirtualAddress());

    const auto toRenderTarget = D3DHelpers::Transition(
        m_lightingTarget.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &toRenderTarget);
    const D3D12_CPU_DESCRIPTOR_HANDLE lightingRtv = m_lightingRtvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->OMSetRenderTargets(1, &lightingRtv, FALSE, nullptr);

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(screenW);
    vp.Height = static_cast<float>(screenH);
    vp.MaxDepth = 1.0f;
    D3D12_RECT sr{0, 0, static_cast<LONG>(screenW), static_cast<LONG>(screenH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    const auto toShaderResource = D3DHelpers::Transition(
        m_lightingTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &toShaderResource);
}

void RenderingSystem::DrawPostProcessPass(
    ID3D12GraphicsCommandList* cmd,
    ID3D12DescriptorHeap* srvHeapShaderVisible,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    UINT screenW,
    UINT screenH)
{
    ID3D12DescriptorHeap* heaps[] = {srvHeapShaderVisible};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_rootSigPostProcess.Get());
    cmd->SetPipelineState(m_psoPostProcess.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_postProcessCB->GetGPUVirtualAddress() + m_frameIndex * kPostProcessCbStride);

    D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeapShaderVisible->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<SIZE_T>(m_postProcessSrvIndex) * m_srvDescriptorIncrement;
    cmd->SetGraphicsRootDescriptorTable(1, table);
    cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(screenW);
    viewport.Height = static_cast<float>(screenH);
    viewport.MaxDepth = 1.0f;
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(screenW), static_cast<LONG>(screenH)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
