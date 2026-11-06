// DirectX 12: окно, FPS-камера, сцена Sponza (OBJ/MTL).

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <wrl/client.h>

#include "Camera.h"
#include "AppPaths.h"
#include "D3D12Context.h"
#include "SceneObjectRenderer.h"
#include "D3DHelpers.h"
#include "ScenePaths.h"
#include "SceneRenderer.h"
#include "RenderingSystem.h"
#include "SpatialCulling.h"
#include "ShadowMap.h"
#include "PointShadowMap.h"
#include "TerrainRenderer.h"
#include "TerrainChecks.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
constexpr UINT kFrameCount = 2;
constexpr UINT kClientW = 1280;
constexpr UINT kClientH = 720;
constexpr UINT kSrvHeapCount = 512;
constexpr UINT kDeferredSrvBase = 400;
constexpr UINT kShadowSrvBase = kDeferredSrvBase + 3;
HWND g_hwnd = nullptr;
UINT g_width = kClientW;
UINT g_height = kClientH;
bool g_running = true;

D3D12Context g_d3d;
auto& g_device = g_d3d.device;
auto& g_queue = g_d3d.queue;
auto& g_factory = g_d3d.factory;
auto& g_swapChain = g_d3d.swapChain;
auto& g_rtvHeap = g_d3d.rtvHeap;
auto& g_rtvDescriptorSize = g_d3d.rtvDescriptorSize;
auto& g_renderTargets = g_d3d.renderTargets;
auto& g_dsvHeap = g_d3d.dsvHeap;
auto& g_depthStencil = g_d3d.depthStencil;
auto& g_cmdAlloc = g_d3d.commandAllocators;
auto& g_cmdList = g_d3d.commandList;
auto& g_fence = g_d3d.fence;
auto& g_fenceValue = g_d3d.fenceValue;
auto& g_fenceEvent = g_d3d.fenceEvent;
auto& g_frameFenceValues = g_d3d.frameFenceValues;
auto& g_swapSeenPresent = g_d3d.swapSeenPresent;

ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pipelineGeo;
ComPtr<ID3D12PipelineState> g_pipelineGeoWire;
ComPtr<ID3D12PipelineState> g_pipelineGeoSimple;
ComPtr<ID3D12PipelineState> g_pipelineGeoMarker;
ComPtr<ID3D12PipelineState> g_pipelineGeoSimpleWire;
ComPtr<ID3D12RootSignature> g_shadowRootSignature;
ComPtr<ID3D12PipelineState> g_shadowPipeline;
ComPtr<ID3D12PipelineState> g_pointShadowPipeline;

RenderingSystem g_renderSys;
ShadowMap g_shadowMap;
PointShadowMap g_pointShadowMap;
SceneRenderer g_sceneRenderer;
TerrainRenderer g_terrain;
bool g_terrainMode = false;
bool g_terrainCulling = true;
bool g_terrainLodColors = false;
std::wstring g_terrainError;
XMFLOAT3 g_otherCamPos{0.0f, 25.0f, -55.0f};
float g_otherCamYaw = 0.0f, g_otherCamPitch = 0.32f;

ComPtr<ID3D12DescriptorHeap> g_srvHeap;
UINT g_srvDescriptorSize = 0;

SceneObjectRenderer g_sceneObjectRenderer;

UINT g_frameIndex = 0;
float g_appTime = 0.0f;

XMFLOAT3 g_camPos{5.2f, 0.3f, 0.0f};
float g_camYaw = -XM_PIDIV2;
float g_camPitch = 0.0f;
bool g_camPrevRmb = false;
bool g_showMotionVectors = false;

struct MotionProbe
{
    bool enabled = false;
    float elapsed = 0;
    XMFLOAT3 center{}, axis{};
    XMFLOAT4X4 previousWorld{};

    XMMATRIX World() const
    {
        const float offset = 0.65f * sinf(elapsed * 2.4f);
        return XMMatrixScaling(0.22f, 0.22f, 0.22f) *
            XMMatrixTranslation(center.x + axis.x * offset, center.y, center.z + axis.z * offset);
    }

    void Toggle()
    {
        enabled = !enabled;
        if (!enabled) return;
        elapsed = 0;
        XMStoreFloat3(&center, XMLoadFloat3(&g_camPos) + Camera::Forward(g_camYaw, g_camPitch) * 1.8f);
        axis = {cosf(g_camYaw), 0, -sinf(g_camYaw)};
        XMStoreFloat4x4(&previousWorld, World());
    }
};

MotionProbe g_motionProbe;

void SwitchScene()
{
    if (!g_terrain.Ready()) return;
    g_terrainMode = !g_terrainMode;
    g_renderSys.Temporal().Invalidate();
    std::swap(g_camPos, g_otherCamPos);
    std::swap(g_camYaw, g_otherCamYaw);
    std::swap(g_camPitch, g_otherCamPitch);
    g_camPrevRmb = false;
}

// Sponza содержит много треугольников, поэтому тесселяция включается вручную клавишей T.
// Так первый кадр гарантированно появляется даже на встроенной видеокарте.
bool g_tessellationEnabled = false;
bool g_wireframeEnabled = false;
bool g_shadowsEnabled = true;
bool g_shadowDebugView = false;
bool g_cascadeColorDebug = false;
bool g_vignetteEnabled = false;
bool g_flyEyeEnabled = false;
UINT g_shadowMapDebugIndex = 0;

LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_qpcLast{};

void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        wchar_t buf[96];
        swprintf_s(buf, L"HRESULT 0x%08X", static_cast<unsigned>(hr));
        MessageBoxW(nullptr, buf, L"SecondSem CG", MB_OK | MB_ICONERROR);
        std::exit(static_cast<int>(hr));
    }
}

void CreateRtv()
{
    g_d3d.CreateRenderTargets();
}

void CreateDepth()
{
    g_d3d.CreateDepthBuffer(g_width, g_height);
}

void ResizeSwapChain(UINT w, UINT h)
{
    if (!g_swapChain || w == 0 || h == 0)
        return;
    g_d3d.Resize(w, h);
    g_width = w;
    g_height = h;
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    if (g_srvHeap && g_device)
    {
        g_renderSys.Resize(
            g_device.Get(), w, h, g_srvHeap.Get(), g_srvDescriptorSize);
    }
}

void UpdateWindowTitle()
{
    if (!g_hwnd)
        return;
    const auto& taa = g_renderSys.Temporal().GetSettings();
    wchar_t temporalStatus[160]{};
    swprintf_s(temporalStatus, L"F3 TAA %s | F4 vectors %s | F5 reprojection %s | F6 clamp %s | F7 probe %s | ",
        taa.enabled ? L"ON" : L"OFF", g_showMotionVectors ? L"ON" : L"OFF",
        taa.reproject ? L"ON" : L"OFF", taa.clampHistory ? L"ON" : L"OFF",
        g_motionProbe.enabled ? L"ON" : L"OFF");
    if (g_terrainMode) {
        const auto& stats = g_terrain.Statistics();
        wchar_t title[512]{};
        swprintf_s(title, L"Sponza + Terrain | observer (culling/LOD: primary camera) | F2: interior camera | R: wire %s | L: LOD colors %s | C: culling %s | tiles %u | triangles %u | culled nodes %u | LOD 0..5 %u/%u/%u/%u/%u/%u",
            g_wireframeEnabled ? L"ON" : L"OFF", g_terrainLodColors ? L"ON" : L"OFF",
            g_terrainCulling ? L"ON" : L"OFF", stats.patches, stats.triangles, stats.culled,
            stats.levels[0], stats.levels[1], stats.levels[2], stats.levels[3], stats.levels[4], stats.levels[5]);
        SetWindowTextW(g_hwnd, (std::wstring(temporalStatus) + title).c_str());
        return;
    }
    wchar_t title[340]{};
    swprintf_s(
        title,
        L"Sponza + Terrain | F2: overview | H: fly eye %s | Y: CSM %s | U: CSM result %s | I: cascades %s | 1-4: texture %u | 0: scene | V: vignette %s | T: tessellation %s | R: edges %s",
        g_flyEyeEnabled ? L"ON" : L"OFF",
        g_shadowsEnabled ? L"ON" : L"OFF",
        g_shadowDebugView ? L"ON" : L"OFF",
        g_cascadeColorDebug ? L"ON" : L"OFF",
        g_shadowMapDebugIndex,
        g_vignetteEnabled ? L"ON" : L"OFF",
        g_tessellationEnabled ? L"ON" : L"OFF",
        g_wireframeEnabled ? L"ON" : L"OFF");
    SetWindowTextW(g_hwnd, (std::wstring(temporalStatus) + title).c_str());
}

void CreateSrvHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kSrvHeapCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void CreateGeometryPipeline()
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 4;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].Descriptor.ShaderRegister = 2;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = _countof(params);
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, rsErr;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &rsErr));
    ThrowIfFailed(g_device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature)));

    const std::wstring sp = AppPaths::GeometryShaderFile();
    ComPtr<ID3DBlob> vs, hs, ds, ps;
    D3DHelpers::CompileShader(sp.c_str(), "GeometryVS", "vs_5_0", vs);
    D3DHelpers::CompileShader(sp.c_str(), "TessellationHS", "hs_5_0", hs);
    D3DHelpers::CompileShader(sp.c_str(), "TessellationDS", "ds_5_0", ds);
    D3DHelpers::CompileShader(sp.c_str(), "GeometryPS", "ps_5_0", ps);

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.HS = {hs->GetBufferPointer(), hs->GetBufferSize()};
    pso.DS = {ds->GetBufferPointer(), ds->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    for (UINT rt = 0; rt < GBuffer::TargetCount; ++rt)
        pso.BlendState.RenderTarget[rt].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = GBuffer::TargetCount;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R32_FLOAT;
    pso.RTVFormats[3] = GBuffer::MotionFormat;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.InputLayout = {layout, _countof(layout)};
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeo)));

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeoWire)));

    // Обычная геометрия сцены рисуется треугольниками без тесселяции.
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.HS = {};
    pso.DS = {};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeoSimple)));

    // Test objects are rendered from both sides, which avoids losing polygons
    // because of a mesh winding convention.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeoMarker)));

    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeoSimpleWire)));

}

bool LoadScene()
{
    const std::filesystem::path objPath = ScenePaths::FindSponzaObj(AppPaths::ExecutableDirectory());
    if (objPath.empty())
        return false;
    return g_sceneRenderer.Load(
        g_device.Get(), g_queue.Get(), g_cmdAlloc[0].Get(), g_cmdList.Get(),
        g_srvHeap.Get(), g_srvDescriptorSize, objPath, kDeferredSrvBase) &&
        g_renderSys.LoadIbl(g_device.Get(), g_queue.Get(), g_cmdAlloc[0].Get(), g_cmdList.Get(),
            g_srvHeap.Get(), AppPaths::ExecutableDirectory() + L"\\assets\\ibl",
            AppPaths::ExecutableDirectory() + L"\\assets\\cascade_debug");
}

XMFLOAT4 ScenePointLight()
{
    XMFLOAT3 minimum{}, maximum{};
    if (!g_sceneRenderer.GetShadowCasterBounds(minimum, maximum))
        return XMFLOAT4(0.0f, 3.0f, 0.0f, 30.0f);
    return XMFLOAT4((minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f, 30.0f);
}

void DrawScene(const XMMATRIX& viewProj)
{
    ID3D12PipelineState* pipeline =
        g_wireframeEnabled ? g_pipelineGeoSimpleWire.Get() : g_pipelineGeoSimple.Get();
    if (g_tessellationEnabled)
        pipeline = g_wireframeEnabled ? g_pipelineGeoWire.Get() : g_pipelineGeo.Get();

    g_sceneRenderer.Draw(
        g_cmdList.Get(), g_srvHeap.Get(), g_rootSignature.Get(), pipeline,
        viewProj, g_camPos, g_appTime, g_tessellationEnabled);
}

void CreateShadowPipeline()
{
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1; rs.pParameters = &param;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_shadowRootSignature)));
    ComPtr<ID3DBlob> vs, pointVs, pointPs;
    D3DHelpers::CompileShader((AppPaths::ExecutableDirectory() + L"\\ShadowPass.hlsl").c_str(), "ShadowVS", "vs_5_0", vs);
    D3DHelpers::CompileShader((AppPaths::ExecutableDirectory() + L"\\ShadowPass.hlsl").c_str(), "PointShadowVS", "vs_5_0", pointVs);
    D3DHelpers::CompileShader((AppPaths::ExecutableDirectory() + L"\\ShadowPass.hlsl").c_str(), "PointShadowPS", "ps_5_0", pointPs);
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}, };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = g_shadowRootSignature.Get(); p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    // Only light-facing scene geometry must enter the shadow map. Rendering
    // both sides in an enclosed Sponza hall makes reverse walls occlude the sun.
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    p.RasterizerState.DepthBias = 1000; p.RasterizerState.SlopeScaledDepthBias = 1.5f; p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE; p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    p.SampleMask = UINT_MAX; p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 0; p.DSVFormat = DXGI_FORMAT_D32_FLOAT; p.SampleDesc.Count = 1; p.InputLayout = {layout, _countof(layout)};
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&g_shadowPipeline)));

    p.VS = {pointVs->GetBufferPointer(), pointVs->GetBufferSize()};
    p.PS = {pointPs->GetBufferPointer(), pointPs->GetBufferSize()};
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&g_pointShadowPipeline)));
}

std::array<XMMATRIX, PointShadowMap::FaceCount> PointShadowMatrices(const XMFLOAT3& position, float range)
{
    const XMVECTOR eye = XMLoadFloat3(&position);
    const XMVECTOR directions[PointShadowMap::FaceCount] = {
        XMVectorSet(1, 0, 0, 0), XMVectorSet(-1, 0, 0, 0),
        XMVectorSet(0, 1, 0, 0), XMVectorSet(0, -1, 0, 0),
        XMVectorSet(0, 0, 1, 0), XMVectorSet(0, 0, -1, 0),
    };
    const XMVECTOR ups[PointShadowMap::FaceCount] = {
        XMVectorSet(0, 1, 0, 0), XMVectorSet(0, 1, 0, 0),
        XMVectorSet(0, 0, -1, 0), XMVectorSet(0, 0, 1, 0),
        XMVectorSet(0, 1, 0, 0), XMVectorSet(0, 1, 0, 0),
    };
    std::array<XMMATRIX, PointShadowMap::FaceCount> matrices{};
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XM_PIDIV2, 1.0f, (std::min)(0.1f, range * 0.01f), range);
    for (UINT face = 0; face < PointShadowMap::FaceCount; ++face)
        matrices[face] = XMMatrixLookToLH(eye, directions[face], ups[face]) * projection;
    return matrices;
}

std::array<float, ShadowMap::CascadeCount> CascadeSplits()
{
    // Camera visibility extends to 200 units, while CSM only needs to cover
    // the 40-unit Sponza hall. Four equal 10-unit intervals make each level
    // visible in the scene instead of placing all geometry in cascade zero.
    constexpr float shadowDistance = 40.0f;
    constexpr float step = shadowDistance / static_cast<float>(ShadowMap::CascadeCount);
    return {step, step * 2.0f, step * 3.0f, shadowDistance};
}

std::array<XMMATRIX, ShadowMap::CascadeCount> CascadeMatrices()
{
    std::array<XMMATRIX, ShadowMap::CascadeCount> result{};
    // Scene-anchored CSM: the light cameras remain fixed in world space.
    // The main camera selects a resolution level, but cannot make the shadow
    // projection slide over Sponza while the player moves.
    const XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(0.18f, -0.96f, -0.22f, 0));
    const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    const XMVECTOR lightUp = fabsf(XMVectorGetY(lightDir)) > 0.95f
        ? XMVectorSet(0, 0, 1, 0) : worldUp;

    XMFLOAT3 boundsMin{-20.0f, -2.0f, -12.0f};
    XMFLOAT3 boundsMax{18.0f, 15.0f, 12.0f};
    g_sceneRenderer.GetShadowCasterBounds(boundsMin, boundsMax);
    const XMVECTOR centre = XMVectorSet(
        (boundsMin.x + boundsMax.x) * 0.5f,
        (boundsMin.y + boundsMax.y) * 0.5f,
        (boundsMin.z + boundsMax.z) * 0.5f, 1.0f);
    float sceneRadius = 0.0f;
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
            {
                const XMVECTOR corner = XMVectorSet(
                    x ? boundsMax.x : boundsMin.x,
                    y ? boundsMax.y : boundsMin.y,
                    z ? boundsMax.z : boundsMin.z, 1.0f);
                sceneRadius = (std::max)(sceneRadius, XMVectorGetX(XMVector3Length(corner - centre)));
            }
    // Every level contains the complete hall. Different radii still give the
    // near cascade the highest texel density without camera-dependent motion.
    const float baseRadius = ceilf(sceneRadius + 2.0f);
    for (UINT cascade = 0; cascade < ShadowMap::CascadeCount; ++cascade)
    {
        const float radius = baseRadius + static_cast<float>(cascade) * 8.0f;
        const XMMATRIX lightView = XMMatrixLookAtLH(centre - lightDir * (radius * 2.0f), centre, lightUp);
        const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(
            -radius, radius, -radius, radius, 0.0f, radius * 4.0f);
        result[cascade] = lightView * lightProjection;
    }
    return result;
}

void DrawFrame(float dt)
{
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    // All mutable upload data has a separate slice for each frame index.
    const UINT64 fenceToWait = g_frameFenceValues[g_frameIndex];
    if (g_fence->GetCompletedValue() < fenceToWait)
    {
        ThrowIfFailed(g_fence->SetEventOnCompletion(fenceToWait, g_fenceEvent));
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }

    g_sceneRenderer.BeginFrame(g_frameIndex);
    g_sceneObjectRenderer.BeginFrame(g_frameIndex);
    g_renderSys.BeginFrame(g_frameIndex);
    Camera::Update(g_hwnd, dt, g_camPos, g_camYaw, g_camPitch, g_camPrevRmb);
    g_appTime += dt;
    const XMMATRIX viewProj = Camera::ViewProjection(g_camPos, g_camYaw, g_camPitch, g_width, g_height, 0.1f, 5000.0f);
    auto& temporal = g_renderSys.Temporal();
    const XMMATRIX rasterViewProj = temporal.BeginFrame(g_frameIndex, viewProj);
    const auto temporalCamera = temporal.CameraConstants(g_frameIndex);
    const auto cascadeSplits = CascadeSplits();
    const auto cascadeMatrices = CascadeMatrices();
    const bool hasPointLight = !g_renderSys.PointLights().empty();
    const XMFLOAT4 pointLightPositionRange = hasPointLight
        ? g_renderSys.PointLights().front().positionRadius : XMFLOAT4(0, 0, 0, 30);
    const auto pointMatrices = PointShadowMatrices(
        XMFLOAT3(pointLightPositionRange.x, pointLightPositionRange.y, pointLightPositionRange.z),
        pointLightPositionRange.w);

    ThrowIfFailed(g_cmdAlloc[g_frameIndex]->Reset());
    ID3D12PipelineState* initialPipeline = g_tessellationEnabled
        ? (g_wireframeEnabled ? g_pipelineGeoWire.Get() : g_pipelineGeo.Get())
        : (g_wireframeEnabled ? g_pipelineGeoSimpleWire.Get() : g_pipelineGeoSimple.Get());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[g_frameIndex].Get(), initialPipeline));

    if (g_shadowsEnabled)
    {
        g_shadowMap.TransitionToDepthWrite(g_cmdList.Get());
        D3D12_VIEWPORT shadowViewport{}; shadowViewport.Width = static_cast<float>(ShadowMap::Resolution); shadowViewport.Height = static_cast<float>(ShadowMap::Resolution); shadowViewport.MaxDepth = 1.0f;
        D3D12_RECT shadowScissor{0, 0, static_cast<LONG>(ShadowMap::Resolution), static_cast<LONG>(ShadowMap::Resolution)};
        g_cmdList->RSSetViewports(1, &shadowViewport); g_cmdList->RSSetScissorRects(1, &shadowScissor);
        for (UINT cascade = 0; cascade < ShadowMap::CascadeCount; ++cascade) {
            const auto dsv = g_shadowMap.Dsv(cascade);
            g_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            g_cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            g_sceneRenderer.DrawShadow(g_cmdList.Get(), g_shadowRootSignature.Get(), g_shadowPipeline.Get(), cascadeMatrices[cascade], cascade);
        }
        g_shadowMap.TransitionToShaderResource(g_cmdList.Get());

        if (hasPointLight)
        {
            g_pointShadowMap.TransitionToRenderTarget(g_cmdList.Get());
            D3D12_VIEWPORT pointViewport{};
            pointViewport.Width = static_cast<float>(PointShadowMap::Resolution);
            pointViewport.Height = static_cast<float>(PointShadowMap::Resolution);
            pointViewport.MaxDepth = 1.0f;
            D3D12_RECT pointScissor{0, 0, PointShadowMap::Resolution, PointShadowMap::Resolution};
            g_cmdList->RSSetViewports(1, &pointViewport);
            g_cmdList->RSSetScissorRects(1, &pointScissor);
            for (UINT face = 0; face < PointShadowMap::FaceCount; ++face)
            {
                const auto rtv = g_pointShadowMap.Rtv(face);
                const auto dsv = g_pointShadowMap.Dsv(face);
                constexpr float clearDistance[] = {1.0f, 0.0f, 0.0f, 0.0f};
                g_cmdList->ClearRenderTargetView(rtv, clearDistance, 0, nullptr);
                g_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                g_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                g_sceneRenderer.DrawPointShadow(g_cmdList.Get(), g_shadowRootSignature.Get(),
                    g_pointShadowPipeline.Get(), pointMatrices[face], face, pointLightPositionRange);
            }
            g_pointShadowMap.TransitionToShaderResource(g_cmdList.Get());
        }
    }

    GBuffer& gb = g_renderSys.GBufferTargets();
    gb.TransitionToRenderTargets(g_cmdList.Get());

    const float gbClearRgb[] = {0.0f, 0.0f, 0.0f};
    gb.ClearAndSetAsRenderTarget(g_cmdList.Get(), gbClearRgb);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(g_width);
    viewport.Height = static_cast<float>(g_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
    g_cmdList->RSSetViewports(1, &viewport);
    g_cmdList->RSSetScissorRects(1, &scissor);

    g_cmdList->SetGraphicsRootSignature(g_rootSignature.Get());
    g_cmdList->SetGraphicsRootConstantBufferView(3, temporalCamera);
    DrawScene(viewProj);
    if (g_motionProbe.enabled)
    {
        g_motionProbe.elapsed += dt;
        const XMMATRIX world = g_motionProbe.World();
        g_sceneObjectRenderer.DrawMotionProbe(g_cmdList.Get(), g_srvHeap.Get(), g_rootSignature.Get(),
            g_pipelineGeoMarker.Get(), viewProj, g_camPos, g_appTime,
            world, XMLoadFloat4x4(&g_motionProbe.previousWorld));
        XMStoreFloat4x4(&g_motionProbe.previousWorld, world);
    }
    if (hasPointLight) g_sceneObjectRenderer.DrawLightMarker(
        g_cmdList.Get(), g_srvHeap.Get(), g_rootSignature.Get(), g_pipelineGeoMarker.Get(),
        viewProj, g_camPos,
        XMFLOAT3(pointLightPositionRange.x, pointLightPositionRange.y, pointLightPositionRange.z),
        XMFLOAT3(g_renderSys.PointLights().front().colorIntensity.x,
            g_renderSys.PointLights().front().colorIntensity.y, g_renderSys.PointLights().front().colorIntensity.z), g_appTime);
    // The overview observes the primary camera's selection, not its own frustum.
    const auto& selectionEye = g_terrainMode ? g_otherCamPos : g_camPos;
    const XMMATRIX selectionVp = g_terrainMode
        ? Camera::ViewProjection(g_otherCamPos, g_otherCamYaw, g_otherCamPitch,
            g_width, g_height, 0.1f, 5000.0f)
        : viewProj;
    g_terrain.Draw(g_cmdList.Get(), viewProj, selectionVp, selectionEye, temporalCamera, g_height,
        g_wireframeEnabled, g_terrainCulling, g_terrainLodColors);
    UpdateWindowTitle();

    gb.TransitionToShaderResource(g_cmdList.Get());

    ComPtr<ID3D12Resource> backBuffer = g_renderTargets[g_frameIndex];
    const D3D12_RESOURCE_STATES rtBefore =
        g_swapSeenPresent[g_frameIndex] ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER toRt = D3DHelpers::Transition(backBuffer.Get(), rtBefore, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_cmdList->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(g_frameIndex) * g_rtvDescriptorSize;

    g_renderSys.UploadFrameConstants(
        g_camPos, rasterViewProj, g_width, g_height,
        cascadeMatrices, cascadeSplits, g_shadowsEnabled, g_shadowDebugView, g_cascadeColorDebug,
        g_vignetteEnabled, g_flyEyeEnabled, g_shadowMapDebugIndex);
    g_renderSys.DrawLightingPass(g_cmdList.Get(), g_srvHeap.Get(), g_width, g_height);
    g_renderSys.DrawPostProcessPass(g_cmdList.Get(), g_srvHeap.Get(), rtv, g_width, g_height, g_showMotionVectors);

    D3D12_RESOURCE_BARRIER toPresent =
        D3DHelpers::Transition(backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_cmdList->ResourceBarrier(1, &toPresent);
    g_swapSeenPresent[g_frameIndex] = true;

    ThrowIfFailed(g_cmdList->Close());
    ID3D12CommandList* lists[] = {g_cmdList.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    ThrowIfFailed(g_swapChain->Present(1, 0));

    const UINT64 signalValue = ++g_fenceValue;
    ThrowIfFailed(g_queue->Signal(g_fence.Get(), signalValue));
    g_frameFenceValues[g_frameIndex] = signalValue;
}

bool RunTerrainSmoke()
{
    const auto output = std::filesystem::path(AppPaths::ExecutableDirectory());
    std::wofstream log(output / L"terrain-check.txt", std::ios::app);
    std::wstring error;
    std::vector<uint8_t> culled, unculled, preview;
    auto capture = [&](std::vector<uint8_t>& image) {
        if (TerrainChecks::Capture(g_d3d, g_frameIndex, g_width, g_height, image, error)) return true;
        log << L"FAIL: " << error << L"\n"; return false;
    };
    DrawFrame(0); DrawFrame(0); // let hysteresis settle at the initial view
    if (!capture(preview) || g_terrain.Statistics().patches == 0) {
        log << L"FAIL: initial terrain view is empty\n"; return false;
    }
    TerrainChecks::SavePpm(output / L"terrain-preview.ppm", g_width, g_height, preview);
    const auto observerStats = g_terrain.Statistics();
    const auto observerPosition = g_camPos;
    g_camPos.x += 10;
    DrawFrame(0); DrawFrame(0);
    if (g_terrain.Statistics().patches != observerStats.patches ||
        g_terrain.Statistics().levels != observerStats.levels ||
        g_terrain.Statistics().culled != observerStats.culled) {
        log << L"FAIL: observer camera changes primary selection\n"; return false;
    }
    g_camPos = observerPosition;
    DrawFrame(0);
    if (!capture(culled)) return false;
    g_terrainCulling = false; DrawFrame(0);
    if (!capture(unculled) || culled == unculled ||
        g_terrain.Statistics().patches <= observerStats.patches) {
        log << L"FAIL: observer cannot see primary frustum removal\n"; return false;
    }
    g_terrainCulling = true;
    log << L"PASS: observer movement preserves primary selection; C reveals removed terrain.\n";
    // Next compare identical images from the primary view, where culling
    // must only remove geometry outside the rendered frustum.
    g_terrainMode = false;
    const auto initialPosition = g_camPos;
    const float initialYaw = g_camYaw, initialPitch = g_camPitch;
    // The overview can contain the whole map. Test culling from a closer
    // view where some patches deliberately extend past the screen edges.
    g_camPos = {0, 420, -820}; g_camYaw = 0; g_camPitch = 0.40f;
    DrawFrame(0); DrawFrame(0);
    if (!capture(culled)) return false;
    const auto stats = g_terrain.Statistics();
    if (stats.patches == 0 || stats.culled == 0) { log << L"FAIL: no visible terrain or no culling\n"; return false; }
    g_terrainCulling = false;
    DrawFrame(0);
    if (!capture(unculled)) return false;
    if (unculled != culled || g_terrain.Statistics().patches < stats.patches) {
        log << L"FAIL: culling changes the visible image\n"; return false;
    }
    log << L"PASS: culling preserves the rendered image; tiles " << stats.patches
        << L" vs " << g_terrain.Statistics().patches << L" with culling disabled.\n";
    g_terrainCulling = true;
    g_wireframeEnabled = true; DrawFrame(0); g_wireframeEnabled = false;
    g_terrainLodColors = true; DrawFrame(0);
    if (!capture(preview)) return false;
    TerrainChecks::SavePpm(output / L"terrain-lod.ppm", g_width, g_height, preview);
    g_terrainLodColors = false;
    g_terrainMode = true;
    const auto terrainPosition = g_camPos;
    SwitchScene(); DrawFrame(0); SwitchScene(); DrawFrame(0);
    if (g_camPos.x != terrainPosition.x || g_camPos.y != terrainPosition.y || g_camPos.z != terrainPosition.z) {
        log << L"FAIL: scene switching loses the terrain camera\n"; return false;
    }
    ResizeSwapChain(640, 360); DrawFrame(0);
    if (!capture(preview) || preview.size() != 640 * 360 * 4 || g_terrain.Statistics().patches == 0) {
        log << L"FAIL: resized terrain\n"; return false;
    }
    ResizeSwapChain(kClientW, kClientH);
    g_camPos = initialPosition; g_camYaw = initialYaw; g_camPitch = initialPitch;
    DrawFrame(0);
    g_d3d.WaitForGpu();
    const UINT errors = TerrainChecks::GpuErrors(g_device.Get());
    log << L"PASS: solid/wireframe/LOD colors, scene switching, resize 640x360 and back.\n";
    log << L"GPU errors: " << errors << L"; triangles in initial view: " << stats.triangles << L"\n";
    return errors == 0;
}

bool RunTemporalSmoke()
{
    const auto output = std::filesystem::path(AppPaths::ExecutableDirectory());
    std::wofstream log(output / L"taa-check.txt");
    auto& temporal = g_renderSys.Temporal();
    auto settings = temporal.GetSettings();
    const auto initialCamera = g_camPos;
    std::vector<uint8_t> pixels, reference;
    auto capture = [&](std::vector<uint8_t>& image) {
        std::wstring error;
        if (TerrainChecks::Capture(g_d3d, g_frameIndex, g_width, g_height, image, error))
            return true;
        log << L"FAIL: " << error << L"\n";
        return false;
    };
    auto coloredVectors = [](const std::vector<uint8_t>& image) {
        size_t count = 0;
        for (size_t i = 0; i < image.size(); i += 4)
            if (abs(int(image[i]) - 128) > 1 || abs(int(image[i + 1]) - 128) > 1)
                ++count;
        return count;
    };
    auto save = [&](const wchar_t* name) {
        TerrainChecks::SavePpm(output / name, g_width, g_height, pixels);
    };

    ResizeSwapChain(640, 360);
    DrawFrame(0);
    if (!capture(pixels)) return false;
    save(L"taa-off.ppm");

    settings.enabled = true;
    temporal.SetSettings(settings);
    for (int frame = 0; frame < 8; ++frame) DrawFrame(0);
    if (!capture(reference)) return false;
    if (pixels == reference) { log << L"FAIL: TAA did not change the image\n"; return false; }
    pixels.swap(reference);
    save(L"taa-on.ppm");

    g_showMotionVectors = true;
    DrawFrame(0);
    if (!capture(pixels)) return false;
    if (coloredVectors(pixels) != 0) { log << L"FAIL: jitter leaks into motion\n"; return false; }
    g_camPos.z += 0.15f;
    DrawFrame(0);
    if (!capture(pixels)) return false;
    if (coloredVectors(pixels) == 0) { log << L"FAIL: no camera vectors\n"; return false; }
    save(L"taa-camera-motion.ppm");

    g_camPos = initialCamera;
    g_motionProbe.Toggle();
    temporal.Invalidate();
    DrawFrame(0);
    g_motionProbe.elapsed = 0.3f;
    DrawFrame(0);
    if (!capture(pixels)) return false;
    const size_t probePixels = coloredVectors(pixels);
    if (probePixels == 0 || probePixels >= pixels.size() / 4) {
        log << L"FAIL: object motion is absent or fills the screen\n"; return false;
    }
    save(L"taa-object-motion.ppm");
    log << L"PASS: static vectors are zero despite jitter; camera and object produce motion.\n";
    g_showMotionVectors = false;

    // Hold the scene clock fixed to isolate history on newly exposed pixels.
    // Eight samples return to the first Halton offset for the comparison frame.
    auto showHistory = [&](bool reproject, bool clampHistory) {
        settings.reproject = reproject;
        settings.clampHistory = clampHistory;
        temporal.SetSettings(settings);
        temporal.Invalidate();
        g_motionProbe.elapsed = 0;
        for (int frame = 0; frame < 8; ++frame) DrawFrame(0);
        g_motionProbe.elapsed = 0.55f;
        DrawFrame(0);
        return capture(pixels);
    };
    auto errorAgainstReference = [&]() {
        uint64_t error = 0;
        for (size_t i = 0; i < pixels.size(); ++i)
            error += abs(int(pixels[i]) - int(reference[i]));
        return error;
    };
    g_motionProbe.elapsed = 0.55f;
    temporal.Invalidate();
    DrawFrame(0);
    if (!capture(reference)) return false;
    if (!showHistory(false, false)) return false;
    const auto noReprojection = errorAgainstReference();
    save(L"taa-same-uv.ppm");
    if (!showHistory(true, false)) return false;
    const auto noClamp = errorAgainstReference();
    save(L"taa-reproject.ppm");
    if (!showHistory(true, true)) return false;
    const auto protectedHistory = errorAgainstReference();
    save(L"taa-clamp.ppm");
    log << L"Distance from current frame (lower is better in this motion case): "
        << noReprojection << L" / " << noClamp << L" / " << protectedHistory << L"\n";
    if (noClamp >= noReprojection || protectedHistory >= noClamp) {
        log << L"FAIL: reprojection or clamping did not improve this motion case\n"; return false;
    }

    if (!g_terrain.Ready()) { log << L"FAIL: terrain unavailable for camera switch\n"; return false; }
    g_motionProbe.Toggle();
    SwitchScene();
    DrawFrame(0);
    if (!capture(pixels)) return false;
    temporal.Invalidate();
    DrawFrame(0);
    if (!capture(reference)) return false;
    if (pixels != reference) { log << L"FAIL: camera cut reused history\n"; return false; }

    DrawFrame(0);
    ResizeSwapChain(512, 320);
    DrawFrame(0);
    if (!capture(pixels)) return false;
    temporal.Invalidate();
    DrawFrame(0);
    if (!capture(reference)) return false;
    if (pixels != reference) { log << L"FAIL: resize reused history\n"; return false; }

    const UINT errors = TerrainChecks::GpuErrors(g_device.Get());
    log << L"PASS: accumulation, reprojection, clamping, camera cut and resize. GPU errors: " << errors << L"\n";
    return errors == 0;
}

void InitD3D(HWND hwnd)
{
    UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&g_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; g_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
            break;
        adapter.Reset();
    }
    if (!g_device)
        ThrowIfFailed(E_FAIL);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue)));

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = g_width;
    swapDesc.Height = g_height;
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = kFrameCount;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(g_factory->CreateSwapChainForHwnd(
        g_queue.Get(), hwnd, &swapDesc, nullptr, nullptr, &swapChain1));
    ThrowIfFailed(swapChain1.As(&g_swapChain));
    ThrowIfFailed(g_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)));
    CreateRtv();

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&g_dsvHeap)));
    CreateDepth();

    for (UINT i = 0; i < kFrameCount; ++i)
        ThrowIfFailed(g_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmdAlloc[i])));
    ThrowIfFailed(g_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&g_cmdList)));
    ThrowIfFailed(g_cmdList->Close());

    ThrowIfFailed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent)
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

    ThrowIfFailed(g_sceneObjectRenderer.Initialize(g_device.Get()));
    CreateSrvHeap();
    CreateGeometryPipeline();
    CreateShadowPipeline();
    g_renderSys.Init(
        g_device.Get(),
        g_width,
        g_height,
        g_srvHeap.Get(),
        kDeferredSrvBase,
        g_srvDescriptorSize,
        AppPaths::LightingShaderFile().c_str(),
        AppPaths::PostProcessShaderFile().c_str(),
        (AppPaths::ExecutableDirectory() + L"TemporalResolve.hlsl").c_str());
    g_shadowMap.Init(g_device.Get(), g_srvHeap.Get(), kShadowSrvBase, g_srvDescriptorSize);
    g_pointShadowMap.Init(g_device.Get(), g_srvHeap.Get(), kDeferredSrvBase + 10, g_srvDescriptorSize);
    if (!LoadScene())
    {
        MessageBoxW(
            hwnd,
            L"Не удалось загрузить сцену Sponza (sponza.obj).",
            L"SecondSem CG — ошибка загрузки сцены",
            MB_OK | MB_ICONERROR);
        std::exit(EXIT_FAILURE);
    }
    g_terrain.Initialize(g_device.Get(), g_queue.Get(),
        std::filesystem::path(AppPaths::ExecutableDirectory()) / L"assets/terrain/canyon",
        std::filesystem::path(AppPaths::ExecutableDirectory()) / L"Terrain.hlsl", g_terrainError);
    // Additional lights are supplied here without changing the shader.
    g_renderSys.SetPointLights({{ScenePointLight(), XMFLOAT4(1.0f, 0.62f, 0.22f, 6.0f)}});
}

void ShutdownD3D()
{
    g_d3d.Shutdown();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            g_running = false;
        else if (wp == VK_F2 && (lp & (1ll << 30)) == 0)
        {
            if (!g_terrain.Ready())
                MessageBoxW(hwnd, g_terrainError.c_str(), L"Terrain loading error", MB_OK | MB_ICONERROR);
            else SwitchScene();
            UpdateWindowTitle();
        }
        else if (wp >= VK_F3 && wp <= VK_F7 && (lp & (1ll << 30)) == 0)
        {
            auto& temporal = g_renderSys.Temporal();
            auto settings = temporal.GetSettings();
            switch (wp)
            {
            case VK_F3: settings.enabled = !settings.enabled; break;
            case VK_F4: g_showMotionVectors = !g_showMotionVectors; break;
            case VK_F5: settings.reproject = !settings.reproject; break;
            case VK_F6: settings.clampHistory = !settings.clampHistory; break;
            case VK_F7: g_motionProbe.Toggle(); temporal.Invalidate(); break;
            }
            temporal.SetSettings(settings);
            UpdateWindowTitle();
        }
        else if (wp == 'C' && (lp & (1ll << 30)) == 0)
        {
            g_terrainCulling = !g_terrainCulling;
            UpdateWindowTitle();
        }
        else if (wp == 'L' && (lp & (1ll << 30)) == 0)
        {
            g_terrainLodColors = !g_terrainLodColors;
            UpdateWindowTitle();
        }
        else if (wp == 'T' && (lp & (1ll << 30)) == 0)
        {
            g_tessellationEnabled = !g_tessellationEnabled;
            UpdateWindowTitle();
        }
        else if (wp == 'R' && (lp & (1ll << 30)) == 0)
        {
            g_wireframeEnabled = !g_wireframeEnabled;
            UpdateWindowTitle();
        }
        else if (wp == 'Y' && (lp & (1ll << 30)) == 0)
        {
            g_shadowsEnabled = !g_shadowsEnabled;
            UpdateWindowTitle();
        }
        else if (wp == 'U' && (lp & (1ll << 30)) == 0)
        {
            g_shadowDebugView = !g_shadowDebugView;
            UpdateWindowTitle();
        }
        else if (wp == 'I' && (lp & (1ll << 30)) == 0)
        {
            g_cascadeColorDebug = !g_cascadeColorDebug;
            UpdateWindowTitle();
        }
        else if (wp == 'V' && (lp & (1ll << 30)) == 0)
        {
            g_vignetteEnabled = !g_vignetteEnabled;
            UpdateWindowTitle();
        }
        else if (wp == 'H' && (lp & (1ll << 30)) == 0)
        {
            g_flyEyeEnabled = !g_flyEyeEnabled;
            UpdateWindowTitle();
        }
        else if (wp >= '1' && wp <= '4' && (lp & (1ll << 30)) == 0)
        {
            g_shadowMapDebugIndex = static_cast<UINT>(wp - '0');
            UpdateWindowTitle();
        }
        else if (wp == '0' && (lp & (1ll << 30)) == 0)
        {
            g_shadowMapDebugIndex = 0;
            UpdateWindowTitle();
        }
        return 0;
    case WM_SIZE:
        if (g_swapChain && wp != SIZE_MINIMIZED)
            ResizeSwapChain(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return static_cast<int>(coHr);

    const bool checkOnly = wcsstr(commandLine, L"--terrain-check") != nullptr;
    const bool smoke = wcsstr(commandLine, L"--terrain-smoke") != nullptr;
    if (checkOnly || smoke) {
        std::wstring error;
        const bool valid = TerrainChecks::Assets(
            std::filesystem::path(AppPaths::ExecutableDirectory()) / L"assets/terrain/canyon", error);
        std::wofstream log(std::filesystem::path(AppPaths::ExecutableDirectory()) / L"terrain-check.txt");
        log << (valid ? L"PASS: PNG16 precision, source dimensions, tile boundaries, quadtree coverage, distance LOD, frustum bounds, invalid input rejection.\n" : L"FAIL: ") << error << L"\n";
        if (!valid || checkOnly) {
            if (SUCCEEDED(coHr)) CoUninitialize();
            return valid ? 0 : 1;
        }
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SecondSemCG_Sponza";
    RegisterClassExW(&wc);

    RECT windowRect{0, 0, static_cast<LONG>(kClientW), static_cast<LONG>(kClientH)};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"SecondSem CG", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd)
        return static_cast<int>(HRESULT_FROM_WIN32(GetLastError()));

    InitD3D(g_hwnd);
    if (wcsstr(commandLine, L"--terrain")) {
        if (!g_terrain.Ready()) {
            std::wofstream log(std::filesystem::path(AppPaths::ExecutableDirectory()) / L"terrain-check.txt", std::ios::app);
            log << L"FAIL: " << g_terrainError << L"\n";
            ShutdownD3D(); DestroyWindow(g_hwnd);
            if (SUCCEEDED(coHr)) CoUninitialize();
            return 1;
        }
        SwitchScene();
    }
    const bool temporalSmoke = wcsstr(commandLine, L"--taa-smoke") != nullptr;
    if (smoke || temporalSmoke) {
        const bool passed = temporalSmoke ? RunTemporalSmoke() : RunTerrainSmoke();
        ShutdownD3D(); DestroyWindow(g_hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (SUCCEEDED(coHr)) CoUninitialize();
        return passed ? 0 : 1;
    }
    UpdateWindowTitle();
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcLast);

    // Окно показывается только после загрузки Sponza, шейдеров и ресурсов GPU.
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (g_running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running)
            break;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (g_qpcFreq.QuadPart == 0)
            QueryPerformanceFrequency(&g_qpcFreq);
        float dt = static_cast<float>(now.QuadPart - g_qpcLast.QuadPart) /
                   static_cast<float>(g_qpcFreq.QuadPart);
        g_qpcLast = now;
        if (dt > 0.1f)
            dt = 0.1f;
        DrawFrame(dt);
    }

    ClipCursor(nullptr);
    ShutdownD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (SUCCEEDED(coHr))
        CoUninitialize();
    return static_cast<int>(msg.wParam);
}
