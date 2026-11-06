#include "TemporalAA.h"
#include "D3DHelpers.h"

#include <cstdlib>
#include <cstring>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT CameraStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr DXGI_FORMAT HistoryFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr UINT JitterPeriod = 8;
constexpr float CurrentWeight = 0.10f;

struct CameraData
{
    XMFLOAT4X4 previousViewProjection;
    XMFLOAT4 jitterNdc;
};
static_assert(sizeof(CameraData) <= CameraStride);

void Check(HRESULT result)
{
    if (FAILED(result))
        std::exit(static_cast<int>(result));
}

float Halton(UINT index, UINT base)
{
    float result = 0, factor = 1;
    for (; index != 0; index /= base)
    {
        factor /= static_cast<float>(base);
        result += factor * static_cast<float>(index % base);
    }
    return result;
}
}

void TemporalAA::Init(ID3D12Device* device, UINT firstHistorySrv, UINT srvIncrement,
    UINT frameCount, const wchar_t* shaderPath)
{
    m_firstHistorySrv = firstHistorySrv;
    m_srvIncrement = srvIncrement;
    m_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = HistorySrvCount;
    Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_rtvHeap)));

    m_cameraBuffer = D3DHelpers::CreateUploadBuffer(device, nullptr, UINT64(frameCount) * CameraStride);
    const D3D12_RANGE noReads{0, 0};
    Check(m_cameraBuffer->Map(0, &noReads, reinterpret_cast<void**>(&m_cameraMapped)));

    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 4;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (UINT i = 0; i < 3; ++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        parameters[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i + 1].DescriptorTable = {1, &ranges[i]};
        parameters[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = _countof(parameters);
    root.pParameters = parameters;
    root.NumStaticSamplers = 1;
    root.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> signature, errors;
    Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors));
    Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&m_root)));

    ComPtr<ID3DBlob> vs, ps;
    D3DHelpers::CompileShader(shaderPath, "TemporalVS", "vs_5_0", vs);
    D3DHelpers::CompileShader(shaderPath, "TemporalPS", "ps_5_0", ps);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = m_root.Get();
    pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.SampleMask = UINT_MAX;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = HistoryFormat;
    pipeline.SampleDesc.Count = 1;
    Check(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_pipeline)));
}

void TemporalAA::Resize(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT width, UINT height)
{
    if (width == m_width && height == m_height)
        return;
    m_width = width;
    m_height = height;
    Invalidate();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width;
    texture.Height = height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = HistoryFormat;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for (UINT i = 0; i < HistorySrvCount; ++i)
    {
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(m_history[i].ReleaseAndGetAddressOf())));
        auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(i) * m_rtvIncrement;
        device->CreateRenderTargetView(m_history[i].Get(), nullptr, rtv);

        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = HistoryFormat;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        auto srv = srvHeap->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += SIZE_T(m_firstHistorySrv + i) * m_srvIncrement;
        device->CreateShaderResourceView(m_history[i].Get(), &view, srv);
    }
}

void TemporalAA::Invalidate()
{
    m_historyValid = false;
    m_previousCameraValid = false;
    m_jitterIndex = 0;
}

void TemporalAA::SetSettings(Settings settings)
{
    if (m_settings.enabled != settings.enabled ||
        m_settings.reproject != settings.reproject ||
        m_settings.clampHistory != settings.clampHistory)
        Invalidate();
    m_settings = settings;
}

XMMATRIX TemporalAA::BeginFrame(UINT frameIndex, const XMMATRIX& viewProjection)
{
    CameraData data{};
    if (!m_previousCameraValid)
        XMStoreFloat4x4(&m_previousViewProjection, viewProjection);
    data.previousViewProjection = m_previousViewProjection;
    if (m_settings.enabled)
    {
        const UINT sample = m_jitterIndex + 1;
        data.jitterNdc.x = (Halton(sample, 2) - 0.5f) * 2.0f / m_width;
        data.jitterNdc.y = (Halton(sample, 3) - 0.5f) * 2.0f / m_height;
        m_jitterIndex = (m_jitterIndex + 1) % JitterPeriod;
    }
    std::memcpy(m_cameraMapped + SIZE_T(frameIndex) * CameraStride, &data, sizeof(data));
    XMStoreFloat4x4(&m_previousViewProjection, viewProjection);
    m_previousCameraValid = true;
    return viewProjection * XMMatrixTranslation(data.jitterNdc.x, data.jitterNdc.y, 0);
}

D3D12_GPU_VIRTUAL_ADDRESS TemporalAA::CameraConstants(UINT frameIndex) const
{
    return m_cameraBuffer->GetGPUVirtualAddress() + UINT64(frameIndex) * CameraStride;
}

UINT TemporalAA::Resolve(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* srvHeap,
    UINT currentColorSrv, UINT motionSrv)
{
    if (!m_settings.enabled)
        return currentColorSrv;

    // History order follows submitted frames, independently of the swap chain.
    const UINT writeIndex = 1 - m_readIndex;
    auto* output = m_history[writeIndex].Get();
    const auto toTarget = D3DHelpers::Transition(output,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &toTarget);

    auto rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(writeIndex) * m_rtvIncrement;
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0, 0, float(m_width), float(m_height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(m_width), LONG(m_height)};
    cmd->RSSetViewports(1, &viewport);
    cmd->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_root.Get());
    cmd->SetPipelineState(m_pipeline.Get());
    const struct { UINT valid, reproject, clamp; float weight; } constants{
        m_historyValid, m_settings.reproject, m_settings.clampHistory, CurrentWeight};
    cmd->SetGraphicsRoot32BitConstants(0, 4, &constants, 0);

    const UINT indices[] = {currentColorSrv, m_firstHistorySrv + m_readIndex, motionSrv};
    for (UINT i = 0; i < _countof(indices); ++i)
    {
        auto srv = srvHeap->GetGPUDescriptorHandleForHeapStart();
        srv.ptr += UINT64(indices[i]) * m_srvIncrement;
        cmd->SetGraphicsRootDescriptorTable(i + 1, srv);
    }
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    const auto toSrv = D3DHelpers::Transition(output,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &toSrv);
    m_readIndex = writeIndex;
    m_historyValid = true;
    return m_firstHistorySrv + writeIndex;
}
