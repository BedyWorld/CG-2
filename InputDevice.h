#pragma once
#include <Windows.h>
#include <array>
class InputDevice
{
public:
    InputDevice(HWND hwnd);
    void Initialize();
    void Update();
    void OnKeyDown(int key);
    void OnKeyUp(int key);
    void OnMouseMove(int x, int y);
    void OnMouseButton(int button, bool pressed);
    bool IsKeyDown(int key)   const;
    bool IsMouseDown(int btn) const;
    int  GetMouseX()          const { return mouseX_; }
    int  GetMouseY()          const { return mouseY_; }
    int  GetMouseDeltaX()     const { return mouseDX_; }
    int  GetMouseDeltaY()     const { return mouseDY_; }
private:
    HWND hwnd_;
    std::array<bool, 256> keys_ = {};
    std::array<bool, 3>   mouseBtn_ = {};
    int mouseX_ = 0, mouseY_ = 0;
    int mouseDX_ = 0, mouseDY_ = 0;
    int prevX_ = 0, prevY_ = 0;
};