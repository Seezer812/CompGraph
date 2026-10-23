#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "TerrainRenderer.h"
#include "TextureUtil.h"
#include "D3DHelpers.h"
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

bool TerrainRenderer::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
    const std::filesystem::path& directory, const std::filesystem::path& shader, std::wstring& error)
{
    m_ready = false;
    if (!m_data.Load(directory / L"HeightMap.png", error, true)) return false;
    auto check = [&](HRESULT hr, const wchar_t* operation) {
        if (FAILED(hr)) { error = operation; return false; }
        return true;
    };
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 1; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_heap)), L"Cannot create terrain texture heap")) return false;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (!check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), L"Terrain upload allocator failed")) return false;
    if (!check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&commands)), L"Terrain upload command list failed")) return false;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!Tex::CreateTexture2DFromFile(device, commands.Get(), m_heap.Get(), 0, increment,
        directory / L"Color.png", m_color, uploads, error)) {
        if (error.empty()) error = L"Cannot upload Color.png";
        return false;
    }
    const auto colorDescription = m_color->GetDesc();
    if (colorDescription.Width != m_data.Width() || colorDescription.Height != m_data.HeightPixels()) {
        error = L"Color.png and HeightMap.png must have identical dimensions."; return false;
    }
    Tex::WriteTexture2DSrv(device, m_color.Get(), m_heap.Get(), 0, increment, true);
    if (!check(commands->Close(), L"Terrain upload close failed")) return false;
    ComPtr<ID3D12Fence> fence;
    if (!check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), L"Terrain upload fence failed")) return false;
    HANDLE complete = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!complete) { error = L"Terrain upload event failed"; return false; }
    ID3D12CommandList* lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    HRESULT hr = queue->Signal(fence.Get(), 1);
    if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, complete);
    if (SUCCEEDED(hr)) WaitForSingleObject(complete, INFINITE);
    CloseHandle(complete);
    if (!check(hr, L"Terrain texture upload failed")) return false;
    // Upload resources survive until the dedicated upload fence completes.
    uploads.clear();

    const auto& vertices = m_data.Vertices(); const auto& indices = m_data.Indices();
    m_vertices = D3DHelpers::CreateUploadBuffer(device, vertices.data(), vertices.size() * sizeof(TerrainData::Vertex));
    m_indices = D3DHelpers::CreateUploadBuffer(device, indices.data(), indices.size() * sizeof(uint32_t));
    m_vertexView = {m_vertices->GetGPUVirtualAddress(), UINT(vertices.size() * sizeof(TerrainData::Vertex)), sizeof(TerrainData::Vertex)};
    m_indexView = {m_indices->GetGPUVirtualAddress(), UINT(indices.size() * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};

    D3D12_DESCRIPTOR_RANGE textureRange{};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 20;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &textureRange};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 2; root.pParameters = parameters;
    root.NumStaticSamplers = 1; root.pStaticSamplers = &sampler;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, diagnostics;
    hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &diagnostics);
    if (!check(hr, L"Cannot serialize terrain root signature")) return false;
    if (!check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&m_root)), L"Cannot create terrain root signature")) return false;
    ComPtr<ID3DBlob> vs, ps;
    constexpr UINT shaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
    hr = D3DCompileFromFile(shader.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "TerrainVS", "vs_5_0", shaderFlags, 0, &vs, &diagnostics);
    if (!check(hr, L"Cannot compile TerrainVS")) {
        if (diagnostics) OutputDebugStringA(static_cast<const char*>(diagnostics->GetBufferPointer())); return false;
    }
    diagnostics.Reset();
    hr = D3DCompileFromFile(shader.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "TerrainPS", "ps_5_0", shaderFlags, 0, &ps, &diagnostics);
    if (!check(hr, L"Cannot compile TerrainPS")) {
        if (diagnostics) OutputDebugStringA(static_cast<const char*>(diagnostics->GetBufferPointer())); return false;
    }
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = m_root.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // skirts are visible from both sides
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE; p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    p.SampleMask = UINT_MAX; p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 3;
    p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    p.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    p.RTVFormats[2] = DXGI_FORMAT_R32_FLOAT;
    for (UINT i = 1; i < 3; ++i)
        p.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.DSVFormat = DXGI_FORMAT_D32_FLOAT; p.SampleDesc.Count = 1;
    p.InputLayout = {layout, _countof(layout)};
    if (!check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&m_solid)), L"Cannot create terrain pipeline")) return false;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    if (!check(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&m_wire)), L"Cannot create terrain wireframe pipeline")) return false;
    m_ready = true;
    return true;
}

void TerrainRenderer::Draw(ID3D12GraphicsCommandList* cmd, const XMMATRIX& vp,
    const XMMATRIX& selectionVp, const XMFLOAT3& selectionEye,
    UINT height, bool wireframe, bool culling, bool lodColors)
{
    if (!m_ready) return;
    m_data.Select(selectionVp, selectionEye, height, culling);
    ID3D12DescriptorHeap* heaps[] = {m_heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_root.Get());
    cmd->SetGraphicsRootDescriptorTable(1, m_heap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetPipelineState(wireframe ? m_wire.Get() : m_solid.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &m_vertexView); cmd->IASetIndexBuffer(&m_indexView);
    struct Constants { XMFLOAT4X4 vp; XMFLOAT4 color; } constants;
    static_assert(sizeof(Constants) == 80);
    XMStoreFloat4x4(&constants.vp, vp);
    constexpr XMFLOAT3 colors[] = {{0.8f,0.15f,0.15f},{0.9f,0.5f,0.1f},{0.8f,0.8f,0.1f},
        {0.1f,0.7f,0.2f},{0.1f,0.5f,0.9f},{0.65f,0.15f,0.8f}};
    for (int index : m_data.Selected()) {
        const auto& node = m_data.Nodes()[index];
        const auto c = colors[node.level];
        constants.color = {c.x, c.y, c.z, lodColors ? 1.0f : 0.0f};
        cmd->SetGraphicsRoot32BitConstants(0, 20, &constants, 0);
        cmd->DrawIndexedInstanced(UINT(m_data.Indices().size()), 1, 0, node.baseVertex, 0);
    }
}
