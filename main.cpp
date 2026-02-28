#include "Window.h"
#include "Game.h"
#include <memory>
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    auto window = std::make_unique<Window>(hInstance, 1280, 720, L"DX12 Phong Cube");
    if (!window->Initialize())
    {
        MessageBox(nullptr, L"Window initialization failed!", L"Error", MB_OK);
        return -1;
    }
    auto game = std::make_unique<Game>(
        window->GetHWND(), window->GetWidth(), window->GetHeight());

    // БАГ #4 ИСПРАВЛЕН: OnResize должен быть установлен ДО game->Initialize(),
    // потому что Initialize открывает командный список и может обработать
    // WM_SIZE. Также добавлена защита: коллбэк проверяет что game жив.
    window->OnResize = [&](int w, int h)
    {
        if (game) game->Resize(w, h);
    };

    if (!game->Initialize())
        return -1;

    return window->Run(
        [&](float dt)
        {
            window->GetInputDevice()->Update();
            game->Update(dt);
        },
        [&]()
        {
            game->Render();
        }
    );
}