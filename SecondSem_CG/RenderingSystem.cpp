#include "RenderingSystem.h"

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

constexpr UINT kMaxLights = 128;
constexpr UINT kStaticLightCount = 3;
constexpr UINT kMaxRainLights = kMaxLights - kStaticLightCount;
constexpr UINT kRainTilesX = 6;
constexpr UINT kRainTilesZ = 5;
constexpr UINT kRainTileCount = kRainTilesX * kRainTilesZ;
constexpr UINT kRainTileCapacity = 16;

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
    UINT lightCount = 0;
    UINT padHdr[3]{};
    LightGpu lights[kMaxLights]{};
    // uint4 повторяет упаковку массивов uint4 в HLSL constant buffer.
    UINT rainTileCounts[8][4]{};
    UINT rainTileLightIndices[120][4]{};
};

static_assert(sizeof(LightingCBGPU) == 10288);

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
    cb.lights[1].position_range = XMFLOAT4(0.f, 4.5f, 2.f, 22.f);
    cb.lights[1].color_intensity = XMFLOAT4(0.12f, 1.f, 0.82f, 5.5f);
  

    XMVECTOR sunDir = XMVector3Normalize(XMVectorSet(0.35f, 0.82f, 0.45f, 0.f));
    cb.lights[2].type = LIGHT_DIR;
    XMStoreFloat4(&cb.lights[2].direction_cosOuter, sunDir);
    cb.lights[2].direction_cosOuter.w = 0.f;
    cb.lights[2].color_intensity = XMFLOAT4(10.f, 0.55f, 1.f, 0.38f);

    std::memcpy(m_lightingCBMapped, &cb, sizeof(cb));
}

void RenderingSystem::UpdateLightRain(float deltaTime)
{
    if (deltaTime <= 0.0f)
        return;

    // До заполнения пула все упавшие источники остаются на полу. После 125-го
    // дождевого источника новые капли вытесняют самые старые.
    constexpr float kSpawnRate = 18.0f;
    // OBJ масштабируется в 0.01 и поворачивается вокруг X; его нижняя точка
    // оказывается примерно на Y = 1.26 в мировых координатах.
    constexpr float kFloorY = 1.35f;
    // Sponza повёрнута на PI вокруг X, поэтому визуальный верх сцены —
    // это отрицательные значения мирового Y.
    constexpr float kSpawnY = -5.5f;
    m_rainSpawnRemainder += deltaTime * kSpawnRate;

    auto random01 = [this]() {
        m_randomState = m_randomState * 1664525u + 1013904223u;
        return static_cast<float>((m_randomState >> 8) & 0x00FFFFFFu) / 16777215.0f;
    };
    auto makeDrop = [&]() {
        RainLight drop{};
        drop.position = XMFLOAT3(
            -5.5f + random01() * 11.0f,
            kFloorY + random01() * (kSpawnY - kFloorY),
            -4.5f + random01() * 9.0f);
        drop.fallSpeed = 2.6f + random01() * 2.2f;
        return drop;
    };

    // Дождь виден сразу после запуска: уже есть 100 капель на разных высотах.
    if (m_rainLights.empty())
    {
        m_rainLights.reserve(kMaxRainLights);
        for (UINT i = 0; i < 100; ++i)
            m_rainLights.push_back(makeDrop());
    }

    while (m_rainSpawnRemainder >= 1.0f)
    {
        m_rainSpawnRemainder -= 1.0f;
        if (m_rainLights.size() == kMaxRainLights)
            m_rainLights.erase(m_rainLights.begin());

        RainLight drop = makeDrop();
        drop.position.y = kSpawnY;
        m_rainLights.push_back(drop);
    }

    for (RainLight& drop : m_rainLights)
        drop.position.y = (std::min)(kFloorY, drop.position.y + drop.fallSpeed * deltaTime);
}

void RenderingSystem::CreateLightingPipeline(ID3D12Device* device, const wchar_t* hlslPath)
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;
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

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
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
    UINT screenW,
    UINT screenH,
    float deltaTime)
{
    UpdateLightRain(deltaTime);

    auto* cb = reinterpret_cast<LightingCBGPU*>(m_lightingCBMapped);
    cb->cameraPos_pad = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.f);
    const float iw = screenW > 0 ? 1.f / static_cast<float>(screenW) : 1.f;
    const float ih = screenH > 0 ? 1.f / static_cast<float>(screenH) : 1.f;
    cb->invScreen_pad = XMFLOAT4(iw, ih, 0.f, 0.f);

    cb->lightCount = kStaticLightCount + static_cast<UINT>(m_rainLights.size());

    const XMVECTOR axis = XMVector3Normalize(XMLoadFloat3(&cameraForward));
    const XMVECTOR eye = XMLoadFloat3(&cameraPos);

    LightGpu& spot = cb->lights[0];
    spot.type = LIGHT_SPOT;
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&spot.position_range), eye);
    spot.position_range.w = 45.f;
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(&spot.direction_cosOuter), axis);
    spot.direction_cosOuter.w = cosf(XM_PI / 7.f);
    spot.spotCosInner = cosf(XM_PI / 10.f);
    spot.color_intensity = XMFLOAT4(1.f, 0.97f, 0.9f, 5.5f);

    for (UINT i = 0; i < m_rainLights.size(); ++i)
    {
        const RainLight& drop = m_rainLights[i];
        LightGpu& light = cb->lights[kStaticLightCount + i];
        light.type = LIGHT_POINT;
        light.position_range = XMFLOAT4(drop.position.x, drop.position.y, drop.position.z, 1.8f);
        // Небольшие различия оттенка делают отдельные "капли" различимыми.
        const float hue = static_cast<float>((i * 37u) % 100u) / 100.0f;
        light.color_intensity = XMFLOAT4(0.25f + hue * 0.35f, 0.45f + hue * 0.35f, 1.0f, 12.0f);

        // Экранный пиксель проверяет только источники из своей и соседних ячеек.
        // Поэтому 125 источников не превращаются в 125 вычислений на каждый пиксель.
        const int tileX = (std::clamp)(static_cast<int>((drop.position.x + 6.0f) / 2.0f), 0, 5);
        const int tileZ = (std::clamp)(static_cast<int>((drop.position.z + 5.0f) / 2.0f), 0, 4);
        const UINT tile = static_cast<UINT>(tileZ * kRainTilesX + tileX);
        UINT& count = cb->rainTileCounts[tile / 4][tile % 4];
        if (count < kRainTileCapacity)
        {
            const UINT index = tile * kRainTileCapacity + count++;
            cb->rainTileLightIndices[index / 4][index % 4] = kStaticLightCount + i;
        }
    }
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
