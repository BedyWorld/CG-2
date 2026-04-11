#include "Window.h"
#include "Game.h"
#include <memory>

// Глобальный указатель на Game для WndProc
static Game* g_game = nullptr;

// Обработчик сообщений для окна
LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        if (g_game)
        {
            g_game->OnShoot();
            return 0;
        }
        break;

    case WM_RBUTTONDOWN:
        if (g_game && (GetAsyncKeyState(VK_CONTROL) & 0x8000))
        {
            g_game->ClearStuckLights();
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == 'R')
        {
            if (g_game)
                g_game->ClearStuckLights();
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    // Передаём остальные сообщения стандартному обработчику окна
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    // Регистрируем класс окна
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = GameWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)COLOR_WINDOW;
    wc.lpszClassName = L"DX12GameWindow";

    RegisterClassEx(&wc);

    // Создаём окно
    HWND hwnd = CreateWindowEx(
        0,
        L"DX12GameWindow",
        L"DX12 Deferred Rendering - Light Bullets (LMB: Shoot, R: Clear lights)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 720,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd)
    {
        MessageBox(nullptr, L"Window creation failed!", L"Error", MB_OK);
        return -1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Создаём игру
    auto game = std::make_unique<Game>(hwnd, 1280, 720);
    g_game = game.get();

    if (!game->Initialize())
    {
        MessageBox(nullptr, L"Game initialization failed!", L"Error", MB_OK);
        return -1;
    }

    // Главный цикл
    MSG msg = {};
    LARGE_INTEGER freq, lastTime, currentTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);

    while (true)
    {
        // Обрабатываем сообщения
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

        // Вычисляем delta time
        QueryPerformanceCounter(&currentTime);
        float deltaTime = (float)(currentTime.QuadPart - lastTime.QuadPart) / (float)freq.QuadPart;
        deltaTime = min(deltaTime, 0.033f); // Ограничиваем 33 мс
        lastTime = currentTime;

        // Обновляем и рендерим
        game->Update(deltaTime);
        game->Render();
    }

    g_game = nullptr;
    return 0;
}