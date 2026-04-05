#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "D3DApp.h"
#include "GameTimer.h"

HWND    g_hWnd = nullptr;
D3DApp* g_pApp = nullptr;   // глобальный указатель — доступен из WndProc


LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        // ── Мышь ────────────────────────────────────────────────
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (g_pApp) g_pApp->OnMouseDown(wParam, LOWORD(lParam), HIWORD(lParam));
        SetCapture(hWnd);   // захват мыши — события идут даже за пределами окна
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (g_pApp) g_pApp->OnMouseUp(wParam);
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (g_pApp) g_pApp->OnMouseMove(wParam, LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_MOUSEWHEEL:
        if (g_pApp) g_pApp->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

        // ── Клавиатура ──────────────────────────────────────────
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            PostQuitMessage(0);
        return 0;

        // ── Закрытие ────────────────────────────────────────────
    case WM_CLOSE:
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool InitWindow(HINSTANCE hInstance, int nShowCmd)
{
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"MyWindowClass";

    if (!RegisterClassEx(&wc))
        return false;

    RECT r{ 0, 0, 1280, 720 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    g_hWnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        L"MyWindowName",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left,
        r.bottom - r.top,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_hWnd)
        return false;

    ShowWindow(g_hWnd, nShowCmd);
    UpdateWindow(g_hWnd);
    return true;
}

int Run()
{
    MSG msg{};

    GameTimer timer;
    timer.Reset();

    D3DApp app(g_hWnd);
    g_pApp = &app;          // теперь WndProc может вызывать методы app

    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            timer.Tick();
            app.UpdateCB(timer.DeltaTime());  // передаём dt, не TotalTime!
            app.Draw();
        }
    }

    g_pApp = nullptr;
    return (int)msg.wParam;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nShowCmd)
{
    if (!InitWindow(hInstance, nShowCmd))
        return 0;

    return Run();
}