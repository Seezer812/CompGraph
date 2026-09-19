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
#include "RainSphereRenderer.h"
#include "D3DHelpers.h"
#include "ScenePaths.h"
#include "SceneRenderer.h"
#include "WaveWallRenderer.h"
#include "RenderingSystem.h"
#include "SpatialCulling.h"
#include "ShadowMap.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
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
ComPtr<ID3D12PipelineState> g_pipelineWaveWall;
ComPtr<ID3D12PipelineState> g_pipelineWaveWallWire;
ComPtr<ID3D12RootSignature> g_shadowRootSignature;
ComPtr<ID3D12PipelineState> g_shadowPipeline;

RenderingSystem g_renderSys;
ShadowMap g_shadowMap;
SceneRenderer g_sceneRenderer;
WaveWallRenderer g_waveWallRenderer;

ComPtr<ID3D12DescriptorHeap> g_srvHeap;
UINT g_srvDescriptorSize = 0;

RainSphereRenderer g_rainSphereRenderer;

UINT g_frameIndex = 0;
float g_appTime = 0.0f;

// Sponza was rotated around X: its interior is toward negative Y, while the
// floor is at y ≈ 1.26. The old y=1.4 spawn was consequently below the floor.
XMFLOAT3 g_camPos{0.0f, -0.5f, 4.5f};
float g_camYaw = -XM_PIDIV2; // Spawn looking 90 degrees left, toward -X.
float g_camPitch = -0.12f;
bool g_camPrevRmb = false;
// Sponza содержит много треугольников, поэтому тесселяция включается вручную клавишей T.
// Так первый кадр гарантированно появляется даже на встроенной видеокарте.
bool g_tessellationEnabled = false;
bool g_wireframeEnabled = false;
bool g_frustumCullingEnabled = true;
bool g_shadowsEnabled = true;
bool g_shadowDebugView = false;
bool g_ssaoEnabled = true;
bool g_ssaoDebugView = false;
SpatialCulling::Stats g_cullingStats{};

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

void WaitForGpu()
{
    g_d3d.WaitForGpu();
}

void ExecuteCommandList()
{
    ThrowIfFailed(g_cmdList->Close());
    ID3D12CommandList* lists[] = {g_cmdList.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
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
    wchar_t title[320]{};
    swprintf_s(
        title,
        L"SecondSem CG | Y: CSM %s | U: CSM map %s | O: SSAO %s | I: AO map %s | T: tessellation %s | R: edges %s | F: frustum + octree %s | objects: %u/2000",
        g_shadowsEnabled ? L"ON" : L"OFF",
        g_shadowDebugView ? L"ON" : L"OFF",
        g_ssaoEnabled ? L"ON" : L"OFF",
        g_ssaoDebugView ? L"ON" : L"OFF",
        g_tessellationEnabled ? L"ON" : L"OFF",
        g_wireframeEnabled ? L"ON" : L"OFF",
        g_frustumCullingEnabled ? L"ON" : L"OFF",
        g_cullingStats.visibleObjects);
    SetWindowTextW(g_hwnd, title);
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

    D3D12_ROOT_PARAMETER params[3]{};
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
    rs.NumParameters = 3;
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
    for (UINT rt = 0; rt < 3; ++rt)
        pso.BlendState.RenderTarget[rt].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = 3;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R32_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.InputLayout = {layout, _countof(layout)};
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeo)));

    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeoWire)));

    // Quad-domain pipeline for the procedural wave wall.
    ComPtr<ID3DBlob> wallHs, wallDs;
    D3DHelpers::CompileShader(sp.c_str(), "WaveWallHS", "hs_5_0", wallHs);
    D3DHelpers::CompileShader(sp.c_str(), "WaveWallDS", "ds_5_0", wallDs);
    pso.HS = {wallHs->GetBufferPointer(), wallHs->GetBufferSize()};
    pso.DS = {wallDs->GetBufferPointer(), wallDs->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // This panel is a visible deformation overlay, so both sides must be rendered.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineWaveWall)));
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineWaveWallWire)));

    // Шарики дождя остаются обычными треугольниками и не проходят через тесселяцию.
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.HS = {};
    pso.DS = {};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeoSimple)));

    // The demonstration cubes use hand-authored faces; render both sides so
    // they remain unmistakably visible regardless of their winding.
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
        g_srvHeap.Get(), g_srvDescriptorSize, objPath) &&
        g_waveWallRenderer.Initialize(g_device.Get());
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
    g_waveWallRenderer.Draw(
        g_cmdList.Get(), g_srvHeap.Get(), g_rootSignature.Get(),
        g_wireframeEnabled ? g_pipelineWaveWallWire.Get() : g_pipelineWaveWall.Get(),
        viewProj, g_appTime);
}

void CreateShadowPipeline()
{
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1; rs.pParameters = &param;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sig, err;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err));
    ThrowIfFailed(g_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_shadowRootSignature)));
    ComPtr<ID3DBlob> vs;
    D3DHelpers::CompileShader((AppPaths::ExecutableDirectory() + L"\\ShadowPass.hlsl").c_str(), "ShadowVS", "vs_5_0", vs);
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
}

std::array<float, ShadowMap::CascadeCount> CascadeSplits()
{
    // Blended logarithmic/linear distribution: dense close to the camera.
    constexpr float nearZ = 0.1f, farZ = 100.0f, lambda = 0.82f;
    std::array<float, ShadowMap::CascadeCount> result{};
    for (UINT i = 1; i <= ShadowMap::CascadeCount; ++i) {
        const float t = static_cast<float>(i) / ShadowMap::CascadeCount;
        const float logarithmic = nearZ * powf(farZ / nearZ, t);
        const float linear = nearZ + (farZ - nearZ) * t;
        result[i - 1] = lambda * logarithmic + (1.0f - lambda) * linear;
    }
    return result;
}

std::array<XMMATRIX, ShadowMap::CascadeCount> CascadeMatrices(const std::array<float, ShadowMap::CascadeCount>& splits)
{
    std::array<XMMATRIX, ShadowMap::CascadeCount> result{};
    // Must exactly match the directional light in RenderingSystem.cpp.
    const XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(0.62f, 0.70f, -0.35f, 0));
    const XMVECTOR eye = XMLoadFloat3(&g_camPos);
    const XMVECTOR forward = Camera::Forward(g_camYaw, g_camPitch);
    const XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
    const XMVECTOR up = XMVector3Normalize(XMVector3Cross(forward, right));
    const float aspect = static_cast<float>(g_width) / (std::max)(1.0f, static_cast<float>(g_height));
    constexpr float tanHalfFov = 0.41421356f; // tan(PI / 8), matches Camera::ViewProjection.
    for (UINT i = 0; i < ShadowMap::CascadeCount; ++i) {
        const float previous = i == 0 ? 0.1f : splits[i - 1];
        const float current = splits[i];
        const float nearHalfH = previous * tanHalfFov, nearHalfW = nearHalfH * aspect;
        const float farHalfH = current * tanHalfFov, farHalfW = farHalfH * aspect;
        XMVECTOR corners[8];
        UINT corner = 0;
        for (float distance : {previous, current}) {
            const float halfH = distance == previous ? nearHalfH : farHalfH;
            const float halfW = distance == previous ? nearHalfW : farHalfW;
            const XMVECTOR planeCenter = eye + forward * distance;
            for (int y : {-1, 1})
                for (int x : {-1, 1})
                    corners[corner++] = planeCenter + right * (halfW * x) + up * (halfH * y);
        }

        XMVECTOR center = XMVectorZero();
        for (const XMVECTOR& point : corners) center += point;
        center /= 8.0f;
        const XMVECTOR lightPosition = center - lightDir * 80.0f;
        const XMMATRIX lightView = XMMatrixLookAtLH(lightPosition, center, worldUp);
        float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
        float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
        for (const XMVECTOR& point : corners) {
            const XMVECTOR ls = XMVector3TransformCoord(point, lightView);
            minX = (std::min)(minX, XMVectorGetX(ls)); maxX = (std::max)(maxX, XMVectorGetX(ls));
            minY = (std::min)(minY, XMVectorGetY(ls)); maxY = (std::max)(maxY, XMVectorGetY(ls));
            minZ = (std::min)(minZ, XMVectorGetZ(ls)); maxZ = (std::max)(maxZ, XMVectorGetZ(ls));
        }
        constexpr float xyPadding = 3.0f, zPadding = 20.0f;
        result[i] = lightView * XMMatrixOrthographicOffCenterLH(
            minX - xyPadding, maxX + xyPadding, minY - xyPadding, maxY + xyPadding,
            (std::max)(0.1f, minZ - zPadding), maxZ + zPadding);
    }
    return result;
}

void DrawFrame(float dt)
{
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    const UINT64 fenceToWait = g_frameFenceValues[g_frameIndex];
    if (g_fence->GetCompletedValue() < fenceToWait)
    {
        ThrowIfFailed(g_fence->SetEventOnCompletion(fenceToWait, g_fenceEvent));
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }

    Camera::Update(g_hwnd, dt, g_camPos, g_camYaw, g_camPitch, g_camPrevRmb);
    g_appTime += dt;
    const XMMATRIX viewProj = Camera::ViewProjection(g_camPos, g_camYaw, g_camPitch, g_width, g_height);
    const auto cascadeSplits = CascadeSplits();
    const auto cascadeMatrices = CascadeMatrices(cascadeSplits);

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
    }

    GBuffer& gb = g_renderSys.GBufferTargets();
    gb.TransitionToRenderTargets(g_cmdList.Get());

    const float gbClearRgb[] = {0.06f, 0.07f, 0.10f};
    gb.ClearAndSetAsRenderTarget(g_cmdList.Get(), gbClearRgb);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(g_width);
    viewport.Height = static_cast<float>(g_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
    g_cmdList->RSSetViewports(1, &viewport);
    g_cmdList->RSSetScissorRects(1, &scissor);

    DrawScene(viewProj);
    g_rainSphereRenderer.Draw(
        g_cmdList.Get(), g_srvHeap.Get(), g_rootSignature.Get(), g_pipelineGeoSimple.Get(),
        g_renderSys.RainLights(), viewProj, g_camPos, g_appTime, g_frustumCullingEnabled,
        true, g_cullingStats);
    UpdateWindowTitle();

    gb.TransitionToShaderResource(g_cmdList.Get());

    ComPtr<ID3D12Resource> backBuffer = g_renderTargets[g_frameIndex];
    const D3D12_RESOURCE_STATES rtBefore =
        g_swapSeenPresent[g_frameIndex] ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER toRt = D3DHelpers::Transition(backBuffer.Get(), rtBefore, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_cmdList->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(g_frameIndex) * g_rtvDescriptorSize;

    XMFLOAT3 camForward{};
    XMStoreFloat3(&camForward, Camera::Forward(g_camYaw, g_camPitch));
    g_renderSys.UploadFrameConstants(
        g_camPos, camForward, viewProj, g_width, g_height, dt,
        cascadeMatrices, cascadeSplits, g_shadowsEnabled, g_shadowDebugView,
        g_ssaoEnabled, g_ssaoDebugView);
    g_renderSys.DrawLightingPass(g_cmdList.Get(), g_srvHeap.Get(), rtv, g_width, g_height);

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

    ThrowIfFailed(g_rainSphereRenderer.Initialize(g_device.Get()));
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
        AppPaths::LightingShaderFile().c_str());
    g_shadowMap.Init(g_device.Get(), g_srvHeap.Get(), kShadowSrvBase, g_srvDescriptorSize);
    if (!LoadScene())
    {
        MessageBoxW(
            hwnd,
            L"Не удалось загрузить сцену Sponza. Проверьте наличие папки Sponza, файла sponza.obj и текстур рядом с исполняемым файлом.",
            L"SecondSem CG — ошибка загрузки сцены",
            MB_OK | MB_ICONERROR);
        std::exit(EXIT_FAILURE);
    }
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
        else if (wp == 'F' && (lp & (1ll << 30)) == 0)
        {
            g_frustumCullingEnabled = !g_frustumCullingEnabled;
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
        else if (wp == 'O' && (lp & (1ll << 30)) == 0)
        {
            g_ssaoEnabled = !g_ssaoEnabled;
            UpdateWindowTitle();
        }
        else if (wp == 'I' && (lp & (1ll << 30)) == 0)
        {
            g_ssaoDebugView = !g_ssaoDebugView;
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

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return static_cast<int>(coHr);

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

    ShutdownD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (SUCCEEDED(coHr))
        CoUninitialize();
    return static_cast<int>(msg.wParam);
}
