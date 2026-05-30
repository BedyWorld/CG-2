#include "Window.h"
#include "Game.h"
#include <memory>

static Game* g_game = nullptr;

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        if (g_game) g_game->OnMouseDown(0);
        return 0;

    case WM_RBUTTONDOWN:
        if (g_game) g_game->OnMouseDown(1);
        return 0;

    case WM_KEYDOWN:
        if (g_game) g_game->OnKeyDown(static_cast<int>(wParam));
        return 0;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (LONG)LOWORD(lParam), (LONG)HIWORD(lParam) };
        ClientToScreen(hwnd, &pt);
        if (g_game) g_game->OnMouseMove(pt.x, pt.y);
        return 0;
    }

    case WM_SIZE:
        // Resize обрабатывается в Game::Update через rs_->Resize
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE,
                   _In_ LPSTR, _In_ int nCmdShow)
{
    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = GameWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
    wc.lpszClassName = L"DX12GameWindow";
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        0, L"DX12GameWindow",
        L"DX12 Deferred + Tessellation  |  Click=capture  WASD=move  Mouse=look"
        L"  LMB=shoot  +/-=displacement  R=clear  Esc=release",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd)
    {
        MessageBox(nullptr, L"Window creation failed!", L"Error", MB_OK);
        return -1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    auto game = std::make_unique<Game>(hwnd, 1280, 720);
    g_game = game.get();

    if (!game->Initialize())
    {
        MessageBox(nullptr, L"Game initialization failed!", L"Error", MB_OK);
        return -1;
    }

    MSG msg = {};
    LARGE_INTEGER freq, lastTime, currentTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);

    while (true)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_game = nullptr;
                return (int)msg.wParam;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        QueryPerformanceCounter(&currentTime);
        float dt = (float)(currentTime.QuadPart - lastTime.QuadPart)
                 / (float)freq.QuadPart;
        dt = min(dt, 0.033f);
        lastTime = currentTime;

        game->Update(dt);
        game->Render();
    }

    g_game = nullptr;
    return 0;
}
