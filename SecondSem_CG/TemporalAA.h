#pragma once

#include <array>
#include <cstdint>
#include <DirectXMath.h>
#include <d3d12.h>
#include <wrl/client.h>

class TemporalAA
{
public:
    struct Settings
    {
        bool enabled = false;
        bool reproject = true;
        bool clampHistory = true;
    };

    static constexpr UINT HistorySrvCount = 2;
    void Init(ID3D12Device* device, UINT firstHistorySrv, UINT srvIncrement,
        UINT frameCount, const wchar_t* shaderPath);
    void Resize(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap, UINT width, UINT height);
    void Invalidate();
    void SetSettings(Settings settings);
    const Settings& GetSettings() const { return m_settings; }

    DirectX::XMMATRIX BeginFrame(UINT frameIndex, const DirectX::XMMATRIX& viewProjection);
    D3D12_GPU_VIRTUAL_ADDRESS CameraConstants(UINT frameIndex) const;

    UINT Resolve(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* srvHeap,
        UINT currentColorSrv, UINT motionSrv);

private:
    Settings m_settings;
    bool m_historyValid = false;
    bool m_previousCameraValid = false;
    UINT m_readIndex = 0;
    UINT m_jitterIndex = 0;
    UINT m_width = 0, m_height = 0;
    UINT m_firstHistorySrv = 0, m_srvIncrement = 0, m_rtvIncrement = 0;
    DirectX::XMFLOAT4X4 m_previousViewProjection{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cameraBuffer;
    std::uint8_t* m_cameraMapped = nullptr;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, HistorySrvCount> m_history;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
};
