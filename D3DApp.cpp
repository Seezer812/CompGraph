#include "D3DApp.h"
#include <stdexcept>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>

// ?? DirectXTK / DirectXTex для загрузки текстур ??????????????????
// Подключите одну из библиотек в проект, например DirectXTex:
//   https://github.com/Microsoft/DirectXTex
// Или DDSTextureLoader из d3d12book:
//   https://github.com/d3dcoder/d3d12book/blob/master/Common/DDSTextureLoader.h
// Здесь используется DDSTextureLoader12 (Luna style):
#include "DDSTextureLoader12.h"  // LoadDDSTextureFromFile
#include "d3dx12.h"               // UpdateSubresources, GetRequiredIntermediateSize

#pragma comment(lib, "d3dcompiler.lib")

#define ThrowIfFailed(x) if(FAILED(x)) throw std::runtime_error("DX12 Error at line " + std::to_string(__LINE__));

using namespace DirectX;

// ================================================================

D3DApp::D3DApp(HWND hwnd) : m_hWnd(hwnd)
{
    InitD3D();
}

// ================================================================
void D3DApp::InitD3D()
{
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    ThrowIfFailed(D3D12CreateDevice(
        nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&mDevice)));

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&mCommandQueue)));

    ThrowIfFailed(mDevice->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCmdAllocator)));

    ThrowIfFailed(mDevice->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        mCmdAllocator.Get(), nullptr, IID_PPV_ARGS(&mCmdList)));
    mCmdList->Close();

    // SwapChain
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = SwapChainBufferCount;
    sd.BufferDesc.Width = mClientWidth;
    sd.BufferDesc.Height = mClientHeight;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.OutputWindow = m_hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    ComPtr<IDXGISwapChain> swapChain;
    ThrowIfFailed(factory->CreateSwapChain(mCommandQueue.Get(), &sd, &swapChain));
    ThrowIfFailed(swapChain.As(&mSwapChain));

    // RTV heap
    mRTVDescriptorSize =
        mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mSrvDescriptorSize =
        mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = SwapChainBufferCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mRTVHeap)));

    CreateRTV();
    CreateDepthStencil();

    // Fence создаём ПЕРВЫМ — он нужен внутри BuildTextures/FlushCommandQueue
    ThrowIfFailed(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));
    mFenceValue = 0;

    BuildRootSignature();
    BuildPSO();
    BuildConstantBuffer();
    BuildGeometry();
    BuildTextures();
    BuildViewportScissor();
}

// ================================================================
void D3DApp::CreateRTV()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        mRTVHeap->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < SwapChainBufferCount; ++i)
    {
        ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainBuffer[i])));
        mDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(), nullptr, handle);
        handle.ptr += (SIZE_T)mRTVDescriptorSize;
    }
}

// ================================================================
void D3DApp::CreateDepthStencil()
{
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&mDSVHeap)));

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = (UINT64)mClientWidth;
    depthDesc.Height = (UINT)mClientHeight;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE optClear{};
    optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &optClear, IID_PPV_ARGS(&mDepthStencilBuffer)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc{};
    dsvViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvViewDesc.Flags = D3D12_DSV_FLAG_NONE;

    mDevice->CreateDepthStencilView(
        mDepthStencilBuffer.Get(), &dsvViewDesc,
        mDSVHeap->GetCPUDescriptorHandleForHeapStart());
}

// ================================================================
//  RootSignature:
//    slot 0 ? таблица [CBV(b0)]
//    slot 1 ? таблица [SRV(t0)]
//    статический сэмплер s0
// ================================================================
void D3DApp::BuildRootSignature()
{
    // ?? Диапазоны дескрипторов ????????????????????????????????
    D3D12_DESCRIPTOR_RANGE cbvRange{};
    cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    cbvRange.NumDescriptors = 1;
    cbvRange.BaseShaderRegister = 0;
    cbvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    // ?? Root параметры ????????????????????????????????????????
    D3D12_ROOT_PARAMETER params[2]{};

    // slot 0: CBV таблица
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &cbvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // slot 1: SRV таблица (текстура)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // ?? Статический сэмплер (s0) — LINEAR WRAP ???????????????
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;   // s0
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = _countof(params);
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, errors;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ThrowIfFailed(mDevice->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSig)));
}

// ================================================================
void D3DApp::BuildPSO()
{
    auto vs = CompileShader(L"Shaders.hlsl", "VSMain", "vs_5_0");
    auto ps = CompileShader(L"Shaders.hlsl", "PSMain", "ps_5_0");

    // Вершинный формат теперь включает TEXCOORD
    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24,   // ? новое
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_BACK;
    rast.FrontCounterClockwise = FALSE;
    rast.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC rtBlend =
    {
        FALSE, FALSE,
        D3D12_BLEND_ONE,  D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE,  D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL
    };
    for (int i = 0; i < 8; i++) blend.RenderTarget[i] = rtBlend;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds.StencilEnable = FALSE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = { inputLayout, _countof(inputLayout) };
    pso.pRootSignature = mRootSig.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.RasterizerState = rast;
    pso.BlendState = blend;
    pso.DepthStencilState = ds;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mPSO)));
}

// ================================================================
//  BuildConstantBuffer  — общая CBV/SRV-куча:
//    слот 0 = CBV
//    слоты 1..MaxTextures = SRV текстур
// ================================================================
void D3DApp::BuildConstantBuffer()
{
    // Создаём общую CBV_SRV_UAV кучу
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1 + MaxTextures;    // CBV + SRVs
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap)));

    // Константный буфер
    UINT cbSize = (sizeof(PerObjectCB) + 255) & ~255u;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = cbSize;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&mConstBuffer)));

    ThrowIfFailed(mConstBuffer->Map(0, nullptr, (void**)&mCbvMappedData));

    // CBV дескриптор в слот 0
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = mConstBuffer->GetGPUVirtualAddress();
    cbv.SizeInBytes = cbSize;

    mDevice->CreateConstantBufferView(
        &cbv, mSrvHeap->GetCPUDescriptorHandleForHeapStart());

    mNextSrvIndex = 1; // следующий свободный слот для SRV
}

// ================================================================
//  Загрузка DDS текстуры (DDSTextureLoader12)
//  Возвращает индекс SRV в куче (?1) или -1 при ошибке
// ================================================================
int D3DApp::LoadTextureDDS(const std::wstring& path)
{
    // ?? Шаг 1: загрузить данные DDS с диска (без GPU upload) ?????
    ComPtr<ID3D12Resource> tex;
    std::unique_ptr<uint8_t[]> ddsData;
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;

    HRESULT hr = DirectX::LoadDDSTextureFromFile(
        mDevice.Get(),
        path.c_str(),
        tex.GetAddressOf(),
        ddsData,
        subresources);

    if (FAILED(hr))
        return -1;

    // ?? Шаг 2: создать upload heap и скопировать данные на GPU ???
    const UINT64 uploadSize = GetRequiredIntermediateSize(
        tex.Get(), 0, (UINT)subresources.size());

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuf;
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE,
        &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&uploadBuf)));

    // ?? Шаг 3: записать команды copy в command list ???????????????
    ThrowIfFailed(mCmdAllocator->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAllocator.Get(), nullptr));

    // Барьер: COMMON ? COPY_DEST
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = tex.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        mCmdList->ResourceBarrier(1, &barrier);
    }

    UpdateSubresources(
        mCmdList.Get(),
        tex.Get(),
        uploadBuf.Get(),
        0, 0,
        (UINT)subresources.size(),
        subresources.data());

    // Барьер: COPY_DEST ? PIXEL_SHADER_RESOURCE
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = tex.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        mCmdList->ResourceBarrier(1, &barrier);
    }

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* cmds[] = { mCmdList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmds);
    FlushCommandQueue();  // ждём пока GPU закончит копирование

    // ?? Шаг 4: создать SRV дескриптор ????????????????????????????
    int idx = mNextSrvIndex++;
    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        mSrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)idx * (SIZE_T)mSrvDescriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = tex->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = tex->GetDesc().MipLevels;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    mDevice->CreateShaderResourceView(tex.Get(), &srvDesc, handle);

    mTextures.push_back(tex);
    mTextureUploads.push_back(uploadBuf);  // держим до следующего FlushCommandQueue

    return idx;
}

// ================================================================
//  BuildTextures — загружаем текстуры для всех объектов
// ================================================================
void D3DApp::BuildTextures()
{
    for (auto& ri : mRenderItems)
    {
        if (!ri.material.diffuseTexture.empty())
        {
            // Конвертируем string ? wstring
            std::wstring wpath(
                ri.material.diffuseTexture.begin(),
                ri.material.diffuseTexture.end());

            // Пробуем загрузить DDS (можно добавить WIC для jpg/png)
            int idx = LoadTextureDDS(wpath);
            ri.SrvIndex = idx;
        }
    }
}

// ================================================================
//  BuildGeometry — загружаем OBJ и создаём RenderItem на группу
// ================================================================
void D3DApp::BuildGeometry()
{
    auto groups = LoadOBJ("model/model.obj");

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    for (auto& grp : groups)
    {
        RenderItem ri;
        ri.material = grp.material;
        ri.IndexCount = (UINT)grp.mesh.indices.size();

        UINT vbSize = (UINT)(grp.mesh.vertices.size() * sizeof(Vertex));
        UINT ibSize = (UINT)(grp.mesh.indices.size() * sizeof(uint32_t));

        // Vertex buffer
        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = vbSize;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(mDevice->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE,
            &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ri.VB)));

        void* mapped = nullptr;
        ThrowIfFailed(ri.VB->Map(0, nullptr, &mapped));
        memcpy(mapped, grp.mesh.vertices.data(), vbSize);
        ri.VB->Unmap(0, nullptr);

        ri.VBV.BufferLocation = ri.VB->GetGPUVirtualAddress();
        ri.VBV.StrideInBytes = sizeof(Vertex);
        ri.VBV.SizeInBytes = vbSize;

        // Index buffer
        bufDesc.Width = ibSize;
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE,
            &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&ri.IB)));

        ThrowIfFailed(ri.IB->Map(0, nullptr, &mapped));
        memcpy(mapped, grp.mesh.indices.data(), ibSize);
        ri.IB->Unmap(0, nullptr);

        ri.IBV.BufferLocation = ri.IB->GetGPUVirtualAddress();
        ri.IBV.Format = DXGI_FORMAT_R32_UINT;
        ri.IBV.SizeInBytes = ibSize;

        mRenderItems.push_back(std::move(ri));
    }
}

// ================================================================
void D3DApp::BuildViewportScissor()
{
    mViewport.TopLeftX = 0;
    mViewport.TopLeftY = 0;
    mViewport.Width = (float)mClientWidth;
    mViewport.Height = (float)mClientHeight;
    mViewport.MinDepth = 0.0f;
    mViewport.MaxDepth = 1.0f;

    mScissor = { 0, 0, mClientWidth, mClientHeight };
}

// ================================================================
//  UpdateCB  — обновляем камеру и UV-анимацию
// ================================================================
void D3DApp::UpdateCB(float dt)
{
    mTotalTime += dt;
    mCamera.Update(dt);

    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX view = mCamera.GetViewMatrix();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4,
        (float)mClientWidth / (float)mClientHeight,
        0.1f, 1000.0f);

    PerObjectCB cb{};
    XMStoreFloat4x4(&cb.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.WorldViewProj, XMMatrixTranspose(world * view * proj));

    cb.LightPosW = { 5.0f, 8.0f, -5.0f };
    cb.EyePosW = mCamera.GetEyePosW();
    cb.DiffuseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
    cb.SpecColorPower = { 1.0f, 1.0f, 1.0f, 32.0f };

    // ?? Текстурная анимация: прокрутка + тайлинг ?????????????
    // Тайлинг: текстура повторяется 2x2
    cb.UVTileX = 2.0f;
    cb.UVTileY = 2.0f;
    // Анимация: медленная прокрутка по U
    cb.UVOffsetX = mTotalTime * 0.05f;   // скорость прокрутки
    cb.UVOffsetY = 0.0f;

    // UseTexture проставляется в Draw() отдельно для каждого объекта
    cb.UseTexture = 0;

    memcpy(mCbvMappedData, &cb, sizeof(cb));
}

// ================================================================
//  Draw
// ================================================================
void D3DApp::Draw()
{
    ThrowIfFailed(mCmdAllocator->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAllocator.Get(), nullptr));

    // PRESENT ? RENDER_TARGET
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = mSwapChainBuffer[mCurrBackBuffer].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        mCmdList->ResourceBarrier(1, &barrier);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
        mRTVHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (SIZE_T)mCurrBackBuffer * (SIZE_T)mRTVDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
        mDSVHeap->GetCPUDescriptorHandleForHeapStart();

    mCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    float clearColor[] = { 0.18f, 0.18f, 0.22f, 1.0f };
    mCmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    mCmdList->ClearDepthStencilView(dsvHandle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, nullptr);

    mCmdList->RSSetViewports(1, &mViewport);
    mCmdList->RSSetScissorRects(1, &mScissor);

    mCmdList->SetGraphicsRootSignature(mRootSig.Get());
    mCmdList->SetPipelineState(mPSO.Get());

    // Устанавливаем общую CBV/SRV кучу
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    mCmdList->SetDescriptorHeaps(1, heaps);

    // slot 0: CBV (дескриптор в позиции 0 кучи)
    mCmdList->SetGraphicsRootDescriptorTable(
        0, mSrvHeap->GetGPUDescriptorHandleForHeapStart());

    mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // ?? Рисуем каждый RenderItem ?????????????????????????????
    for (const auto& ri : mRenderItems)
    {
        // Обновляем UseTexture в CB
        // Патчируем только это поле без пересчёта матриц
        bool hasTexture = (ri.SrvIndex >= 1);
        PerObjectCB* cbPtr = reinterpret_cast<PerObjectCB*>(mCbvMappedData);
        cbPtr->UseTexture = hasTexture ? 1 : 0;
        // Диффузный цвет из материала (используется когда нет текстуры)
        cbPtr->DiffuseColor = {
            ri.material.Kd[0], ri.material.Kd[1], ri.material.Kd[2], 1.0f
        };
        cbPtr->SpecColorPower = {
            ri.material.Ks[0], ri.material.Ks[1], ri.material.Ks[2],
            ri.material.Ns
        };

        if (hasTexture)
        {
            // slot 1: SRV текстуры этого объекта
            D3D12_GPU_DESCRIPTOR_HANDLE srvGpu =
                mSrvHeap->GetGPUDescriptorHandleForHeapStart();
            srvGpu.ptr += (SIZE_T)ri.SrvIndex * (SIZE_T)mSrvDescriptorSize;
            mCmdList->SetGraphicsRootDescriptorTable(1, srvGpu);
        }
        else
        {
            // Указываем на слот 0 (CBV) — шейдер всё равно не будет
            // сэмплировать (UseTexture == 0), но дескриптор должен
            // быть валидным — указываем на первый SRV если есть,
            // иначе на CBV (безопасно при UseTexture=0).
            mCmdList->SetGraphicsRootDescriptorTable(
                1, mSrvHeap->GetGPUDescriptorHandleForHeapStart());
        }

        mCmdList->IASetVertexBuffers(0, 1, &ri.VBV);
        mCmdList->IASetIndexBuffer(&ri.IBV);
        mCmdList->DrawIndexedInstanced(ri.IndexCount, 1, 0, 0, 0);
    }

    // RENDER_TARGET ? PRESENT
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = mSwapChainBuffer[mCurrBackBuffer].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        mCmdList->ResourceBarrier(1, &barrier);
    }

    ThrowIfFailed(mCmdList->Close());

    ID3D12CommandList* cmds[] = { mCmdList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmds);

    ThrowIfFailed(mSwapChain->Present(1, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    FlushCommandQueue();
}

// ================================================================
void D3DApp::FlushCommandQueue()
{
    mFenceValue++;
    ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mFenceValue));

    if (mFence->GetCompletedValue() < mFenceValue)
    {
        HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        ThrowIfFailed(mFence->SetEventOnCompletion(mFenceValue, ev));
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }
}

// ================================================================
ComPtr<ID3DBlob> D3DApp::CompileShader(
    const wchar_t* filename,
    const char* entry,
    const char* target)
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> bytecode, errors;
    HRESULT hr = D3DCompileFromFile(
        filename, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, target, flags, 0, &bytecode, &errors);
    if (errors) OutputDebugStringA((char*)errors->GetBufferPointer());
    ThrowIfFailed(hr);
    return bytecode;
}