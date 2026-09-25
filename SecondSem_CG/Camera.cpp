#include "Camera.h"

#include <algorithm>

using namespace DirectX;

namespace Camera
{

XMVECTOR Forward(float yaw, float pitch)
{
    return XMVector3Normalize(XMVectorSet(
        sinf(yaw) * cosf(pitch), -sinf(pitch), cosf(yaw) * cosf(pitch), 0.0f));
}

XMMATRIX ViewProjection(
    const XMFLOAT3& position, float yaw, float pitch, UINT viewportWidth, UINT viewportHeight)
{
    const XMVECTOR eye = XMLoadFloat3(&position);
    // The imported Sponza scene is oriented with negative Y as world-up.
    const XMMATRIX view = XMMatrixLookToLH(eye, Forward(yaw, pitch), XMVectorSet(0, -1, 0, 0));
    const float aspect = static_cast<float>(viewportWidth) / static_cast<float>((std::max)(1u, viewportHeight));
    return view * XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, NearPlane, FarPlane);
}

void Update(
    HWND window, float deltaTime, XMFLOAT3& position, float& yaw, float& pitch, bool& wasRightMouseDown)
{
    if (!window || deltaTime <= 0.0f)
        return;

    constexpr float moveSpeed = 4.0f;
    constexpr float lookSpeed = 0.0022f;
    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0)
    {
        RECT client{};
        GetClientRect(window, &client);
        POINT center{(client.right - client.left) / 2, (client.bottom - client.top) / 2};
        ClientToScreen(window, &center);
        POINT cursor{};
        GetCursorPos(&cursor);
        if (wasRightMouseDown)
        {
            yaw -= static_cast<float>(cursor.x - center.x) * lookSpeed;
            pitch -= static_cast<float>(cursor.y - center.y) * lookSpeed;
            pitch = std::clamp(pitch, -XM_PIDIV2 + 0.02f, XM_PIDIV2 - 0.02f);
        }
        SetCursorPos(center.x, center.y);
        wasRightMouseDown = true;
        POINT upperLeft{0, 0};
        ClientToScreen(window, &upperLeft);
        POINT lowerRight{client.right, client.bottom};
        ClientToScreen(window, &lowerRight);
        const RECT clip{upperLeft.x, upperLeft.y, lowerRight.x, lowerRight.y};
        ClipCursor(&clip);
    }
    else
    {
        ClipCursor(nullptr);
        wasRightMouseDown = false;
    }

    float x = 0, y = 0, z = 0;
    if (GetAsyncKeyState('W') & 0x8000) z += 1;
    if (GetAsyncKeyState('S') & 0x8000) z -= 1;
    if (GetAsyncKeyState('D') & 0x8000) x += 1;
    if (GetAsyncKeyState('A') & 0x8000) x -= 1;
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) y += 1;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) y -= 1;

    const XMVECTOR forward = Forward(yaw, pitch);
    const XMVECTOR up = XMVectorSet(0, -1, 0, 0);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
    XMVECTOR delta = XMVectorScale(forward, z) + XMVectorScale(right, x) + XMVectorScale(up, y);
    if (XMVectorGetX(XMVector3LengthSq(delta)) > 1e-8f)
    {
        delta = XMVector3Normalize(delta);
        XMStoreFloat3(&position, XMVectorAdd(XMLoadFloat3(&position), XMVectorScale(delta, moveSpeed * deltaTime)));
    }
}

} // namespace Camera
