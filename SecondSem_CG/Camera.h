#pragma once

#include <windows.h>
#include <DirectXMath.h>

namespace Camera
{

DirectX::XMVECTOR Forward(float yaw, float pitch);

DirectX::XMMATRIX ViewProjection(
    const DirectX::XMFLOAT3& position,
    float yaw,
    float pitch,
    UINT viewportWidth,
    UINT viewportHeight);

void Update(
    HWND window,
    float deltaTime,
    DirectX::XMFLOAT3& position,
    float& yaw,
    float& pitch,
    bool& wasRightMouseDown);

} // namespace Camera
