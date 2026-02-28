#pragma once
#include <Windows.h>
#include <functional>
#include <string>
#include <memory>
#include "InputDevice.h"

class Window
{
public:
    Window(HINSTANCE hInstance, int width, int height, const std::wstring& title);
    ~Window();

    bool Initialize();

    // Run the message loop.
    // onUpdate(deltaTime) is called each frame before rendering.
    // onRender() is called each frame to render.
    int Run(std::function<void(float)> onUpdate,
            std::function<void()>      onRender);

    HWND         GetHWND()        const { return hwnd_; }
    int          GetWidth()       const { return width_; }
    int          GetHeight()      const { return height_; }
    InputDevice* GetInputDevice() const { return input_.get(); }

    // Called by Game on resize
    std::function<void(int, int)> OnResize;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    HINSTANCE    hInstance_;
    HWND         hwnd_ = nullptr;
    int          width_;
    int          height_;
    std::wstring title_;

    std::unique_ptr<InputDevice> input_;

    LARGE_INTEGER frequency_ = {};
    LARGE_INTEGER lastTime_  = {};
};
