#pragma once

// RenderingSystem — высокоуровневая подсистема рендеринга для отложенного освещения:
// оркеструет G-buffer (геопроход) и полноэкранный проход света по нескольким источникам.

#include <cstdint>
#include <vector>
#include <array>
#include <filesystem>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "GBuffer.h"
#include "D3D12Context.h"
#include "LightTiling.h"

class RenderingSystem
{
public:
    using PointLight = LightTiling::PointLight;
    // The first point light owns the existing shadow cubemap and marker.
    // Other lights have finite radius and no shadow maps. Empty lists are valid.
    void SetPointLights(std::vector<PointLight> lights);
    const std::vector<PointLight>& PointLights() const { return m_pointLights; }
    // Select only after the corresponding GPU frame has completed.
    void BeginFrame(UINT frameIndex) { m_frameIndex = frameIndex; }
    void Init(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT gbufferSrvStartIndex,
        UINT srvDescriptorIncrement,
        const wchar_t* deferredHlslPath,
        const wchar_t* postProcessHlslPath);

    void Resize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT srvDescriptorIncrement);

    void UploadFrameConstants(
        const DirectX::XMFLOAT3& cameraPos,
        const DirectX::XMMATRIX& viewProjection,
        UINT screenW,
        UINT screenH,
        const std::array<DirectX::XMMATRIX, 4>& cascadeMatrices,
        const std::array<float, 4>& cascadeSplits,
        bool shadowsEnabled,
        bool shadowDebugView,
        bool cascadeColorDebug,
        bool vignetteEnabled,
        bool flyEyeEnabled,
        UINT shadowMapDebugIndex);

    void DrawLightingPass(
        ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* srvHeapShaderVisible,
        UINT screenW,
        UINT screenH);

    void DrawPostProcessPass(
        ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* srvHeapShaderVisible,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        UINT screenW,
        UINT screenH);

    bool LoadIbl(ID3D12Device* device, ID3D12CommandQueue* queue,
        ID3D12CommandAllocator* uploadAllocator, ID3D12GraphicsCommandList* uploadCommands,
        ID3D12DescriptorHeap* srvHeap, const std::filesystem::path& assetDirectory,
        const std::filesystem::path& cascadeTextureDirectory);

    GBuffer& GBufferTargets() { return m_gbuffer; }

private:
    UINT m_frameIndex = 0;
    float m_environmentMaxLod = 0.0f;
    struct LightFrame
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> lights, ranges, indices;
        uint8_t *lightsMapped = nullptr, *rangesMapped = nullptr, *indicesMapped = nullptr;
        UINT64 lightsCapacity = 0, rangesCapacity = 0, indicesCapacity = 0;
    };
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    std::array<LightFrame, D3D12Context::FrameCount> m_lightFrames;
    std::vector<PointLight> m_pointLights;
    void UploadLightTiles(const DirectX::XMMATRIX& viewProjection, UINT width, UINT height);
    void CreateLightingPipeline(ID3D12Device* device, const wchar_t* hlslPath);
    void CreatePostProcessPipeline(ID3D12Device* device, const wchar_t* hlslPath);
    void CreateLightingTarget(ID3D12Device* device, UINT width, UINT height);
    void CreateLightingTargetSrv(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap);
    void WriteDefaultLights();

    GBuffer m_gbuffer;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSigLight;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoLight;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSigPostProcess;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPostProcess;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightingCB;
    uint8_t* m_lightingCBMapped = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_postProcessCB;
    uint8_t* m_postProcessCBMapped = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightingTarget;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_lightingRtvHeap;
    UINT m_lightingWidth = 0;
    UINT m_lightingHeight = 0;

    UINT m_gbufferSrvBase = 0;
    UINT m_iblSrvBase = 0;
    UINT m_cascadeTextureSrvBase = 0;
    UINT m_postProcessSrvIndex = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_iblTextures;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_iblUploads;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> m_cascadeTextures;
    UINT m_srvDescriptorIncrement = 0;
};
