#include "RenderingSystem.h"
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

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{

enum LightType : UINT
{
    LIGHT_DIR = 0,
    LIGHT_POINT = 1,
    LIGHT_SPOT = 2,
};

constexpr UINT kStaticLightCount = 3;

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
    XMFLOAT4 cameraForward_shadowEnabled{};
    XMFLOAT4 cascadeSplits{};
    XMFLOAT4X4 cascadeMatrices[4]{};
    UINT lightCount = 0;
    UINT vignetteEnabled = 0;
    UINT shadowMapDebugIndex = 0;
    UINT cascadeColorDebug = 0;
    LightGpu lights[kStaticLightCount]{};
};

static_assert(sizeof(LightingCBGPU) == 592);

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
    cb.lightCount = kStaticLightCount;

    cb.lights[1].type = LIGHT_POINT;
    // Negative Y is upward in the transformed Sponza coordinate system.
    cb.lights[1].position_range = XMFLOAT4(0.f, -4.5f, 2.f, 22.f);
    cb.lights[1].color_intensity = XMFLOAT4(0.12f, 1.f, 0.82f, 5.5f);
  

    // Sponza is rotated by 180 degrees around X, so its local "up" points
    // toward negative world Y.  Positive ray Y places the source above its hall.
    // Equivalent to the working reference scene after Sponza's X-axis flip:
    // mostly top-down light with a small lateral component.
    XMVECTOR sunDir = XMVector3Normalize(XMVectorSet(0.18f, 0.96f, -0.22f, 0.f));
    cb.lights[2].type = LIGHT_DIR;
    XMStoreFloat4(&cb.lights[2].direction_cosOuter, sunDir);
    cb.lights[2].direction_cosOuter.w = 0.f;
    cb.lights[2].color_intensity = XMFLOAT4(1.0f, 0.93f, 0.80f, 7.0f);

    std::memcpy(m_lightingCBMapped, &cb, sizeof(cb));
}

void RenderingSystem::CreateLightingPipeline(ID3D12Device* device, const wchar_t* hlslPath)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 11; // G-buffer, four CSM maps, three IBL maps and point-light cubemap.
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

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSamp = samp;
    shadowSamp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSamp.AddressU = shadowSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSamp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSamp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSamp.ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
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
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    pso.InputLayout.NumElements = 0;
    pso.InputLayout.pInputElementDescs = nullptr;

    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoLight));
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));
}

void RenderingSystem::Init(
    ID3D12Device* device,
    UINT width,
    UINT height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    UINT gbufferSrvStartIndex,
    UINT srvDescriptorIncrement,
    const wchar_t* deferredHlslPath)
{
    m_gbufferSrvBase = gbufferSrvStartIndex;
    m_iblSrvBase = gbufferSrvStartIndex + 7;
    m_srvDescriptorIncrement = srvDescriptorIncrement;

    m_gbuffer.Init(device, width, height);
    m_gbuffer.CreateShaderResourceViews(
        device, shaderVisibleSrvHeap, gbufferSrvStartIndex, srvDescriptorIncrement);

    CreateLightingPipeline(device, deferredHlslPath);

    m_lightingCB = CreateUploadCb(device, sizeof(LightingCBGPU));
    D3D12_RANGE rr{0, 0};
    HRESULT hr = m_lightingCB->Map(0, &rr, reinterpret_cast<void**>(&m_lightingCBMapped));
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));

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
}

void RenderingSystem::UploadFrameConstants(
    const XMFLOAT3& cameraPos,
    const XMFLOAT3& cameraForward,
    const XMMATRIX& viewProjection,
    UINT screenW,
    UINT screenH,
    float deltaTime,
    const std::array<XMMATRIX, 4>& cascadeMatrices,
    const std::array<float, 4>& cascadeSplits,
    bool shadowsEnabled,
    bool shadowDebugView,
    bool cascadeColorDebug,
    bool vignetteEnabled,
    UINT shadowMapDebugIndex)
{
    auto* cb = reinterpret_cast<LightingCBGPU*>(m_lightingCBMapped);
    cb->cameraPos_pad = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
    const float iw = screenW > 0 ? 1.f / static_cast<float>(screenW) : 1.f;
    const float ih = screenH > 0 ? 1.f / static_cast<float>(screenH) : 1.f;
    cb->invScreen_pad = XMFLOAT4(iw, ih, shadowDebugView ? 1.0f : 0.0f, 0.0f);
    XMStoreFloat4x4(&cb->inverseViewProjection, XMMatrixInverse(nullptr, viewProjection));
    cb->cameraForward_shadowEnabled = XMFLOAT4(cameraForward.x, cameraForward.y, cameraForward.z, shadowsEnabled ? 1.0f : 0.0f);
    cb->cascadeSplits = XMFLOAT4(cascadeSplits[0], cascadeSplits[1], cascadeSplits[2], cascadeSplits[3]);
    for (UINT i = 0; i < 4; ++i)
        XMStoreFloat4x4(&cb->cascadeMatrices[i], cascadeMatrices[i]);

    cb->lightCount = kStaticLightCount;
    cb->vignetteEnabled = vignetteEnabled ? 1u : 0u;
    cb->shadowMapDebugIndex = shadowMapDebugIndex;
    cb->cascadeColorDebug = cascadeColorDebug ? 1u : 0u;

    const XMVECTOR axis = XMVector3Normalize(XMLoadFloat3(&cameraForward));
    const XMVECTOR eye = XMLoadFloat3(&cameraPos);

    LightGpu& spot = cb->lights[0];
    spot.type = LIGHT_SPOT;
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&spot.position_range), eye);
    spot.position_range.w = 0.f; // Camera flashlight is disabled: it flattened CSM shadows.
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&spot.direction_cosOuter), axis);
    spot.direction_cosOuter.w = cosf(XM_PI / 7.f);
    spot.spotCosInner = cosf(XM_PI / 10.f);
    spot.color_intensity = XMFLOAT4(1.f, 0.97f, 0.9f, 0.0f);

}

bool RenderingSystem::LoadIbl(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12CommandAllocator* uploadAllocator, ID3D12GraphicsCommandList* uploadCommands,
    ID3D12DescriptorHeap* srvHeap, const std::filesystem::path& assetDirectory)
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
    if (FAILED(uploadCommands->Close())) return false;
    ID3D12CommandList* lists[] = {uploadCommands}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!eventHandle) return false;
    queue->Signal(fence.Get(), 1); fence->SetEventOnCompletion(1, eventHandle); WaitForSingleObject(eventHandle, INFINITE); CloseHandle(eventHandle);
    m_iblTextures = {irradiance, prefiltered, integration};
    return true;
}

void RenderingSystem::DrawLightingPass(
    ID3D12GraphicsCommandList* cmd,
    ID3D12DescriptorHeap* srvHeapShaderVisible,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    UINT screenW,
    UINT screenH)
{
    ID3D12DescriptorHeap* heaps[] = {srvHeapShaderVisible};
    cmd->SetDescriptorHeaps(1, heaps);

    cmd->SetGraphicsRootSignature(m_rootSigLight.Get());
    cmd->SetPipelineState(m_psoLight.Get());

    cmd->SetGraphicsRootConstantBufferView(0, m_lightingCB->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeapShaderVisible->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<SIZE_T>(m_gbufferSrvBase) * static_cast<SIZE_T>(m_srvDescriptorIncrement);
    cmd->SetGraphicsRootDescriptorTable(1, table);

    cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(screenW);
    vp.Height = static_cast<float>(screenH);
    vp.MaxDepth = 1.0f;
    D3D12_RECT sr{0, 0, static_cast<LONG>(screenW), static_cast<LONG>(screenH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
