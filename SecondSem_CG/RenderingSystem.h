#pragma once

// RenderingSystem — высокоуровневая подсистема рендеринга для отложенного освещения:
// оркеструет G-buffer (геопроход) и полноэкранный проход света по нескольким источникам.

#include <cstdint>
#include <vector>
#include <array>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "GBuffer.h"

class RenderingSystem
{
public:
    struct RainLight
    {
        DirectX::XMFLOAT3 position{};
        float fallSpeed = 0.0f;
    };

    void Init(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT gbufferSrvStartIndex,
        UINT srvDescriptorIncrement,
        const wchar_t* deferredHlslPath);

    void Resize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT srvDescriptorIncrement);

    void UploadFrameConstants(
        const DirectX::XMFLOAT3& cameraPos,
        const DirectX::XMFLOAT3& cameraForward,
        const DirectX::XMMATRIX& viewProjection,
        UINT screenW,
        UINT screenH,
        float deltaTime,
        const std::array<DirectX::XMMATRIX, 4>& cascadeMatrices,
        const std::array<float, 4>& cascadeSplits,
        bool shadowsEnabled,
        bool shadowDebugView,
        bool ssaoEnabled,
        bool ssaoDebugView);

    void DrawLightingPass(
        ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* srvHeapShaderVisible,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        UINT screenW,
        UINT screenH);

    GBuffer& GBufferTargets() { return m_gbuffer; }
    const std::vector<RainLight>& RainLights() const { return m_rainLights; }

private:
    void CreateLightingPipeline(ID3D12Device* device, const wchar_t* hlslPath);
    void CreateSsaoPipeline(ID3D12Device* device, const wchar_t* hlslPath);
    void CreateSsaoTarget(ID3D12Device* device, UINT width, UINT height, ID3D12DescriptorHeap* srvHeap);
    void DrawSsaoPass(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* srvHeapShaderVisible, UINT screenW, UINT screenH);
    void WriteDefaultLights();
    void UpdateLightRain(float deltaTime);

    GBuffer m_gbuffer;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSigLight;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoLight;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSigSsao;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSsao;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightingCB;
    uint8_t* m_lightingCBMapped = nullptr;

    // Full-screen AO intermediate: sampled by the lighting pass after it has
    // been filled from the normal/depth buffers in the same frame.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_ssaoTarget;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_ssaoRtvHeap;
    D3D12_RESOURCE_STATES m_ssaoState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    UINT m_gbufferSrvBase = 0;
    UINT m_srvDescriptorIncrement = 0;
    std::vector<RainLight> m_rainLights;
    float m_rainSpawnRemainder = 0.0f;
    uint32_t m_randomState = 0xC0FFEEu;
};
