#include "Window.h"
#include <stdexcept>

// ============================================================
//  Constructor / Destructor
// ============================================================
Window::Window(HINSTANCE hInstance, int width, int height, const std::wstring& title)
    : hInstance_(hInstance), width_(width), height_(height), title_(title)
{
}

Window::~Window()
{
    if (hwnd_)
        DestroyWindow(hwnd_);
}

// ============================================================
//  Initialize
// ============================================================
bool Window::Initialize()
{
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance_;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX12WindowClass";

    if (!RegisterClassExW(&wc))
        return false;

    // Calculate window rect for desired client area
    RECT rc = { 0, 0, width_, height_ };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExW(
        0,
        L"DX12WindowClass",
        title_.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance_, this);  // pass 'this' for WndProc

    if (!hwnd_)
        return false;

    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);

    // Create input device
    input_ = std::make_unique<InputDevice>(hwnd_);
    input_->Initialize();

    // Init timer
    QueryPerformanceFrequency(&frequency_);
    QueryPerformanceCounter(&lastTime_);

    return true;
}

// ============================================================
//  Run (message + game loop)
// ============================================================
int Window::Run(std::function<void(float)> onUpdate,
                std::function<void()>      onRender)
{
    MSG msg = {};
    while (true)
    {
        // Drain the Windows message queue
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                return static_cast<int>(msg.wParam);

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Compute delta time
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - lastTime_.QuadPart)
                   / static_cast<float>(frequency_.QuadPart);
        lastTime_ = now;

        onUpdate(dt);
        onRender();
    }
}

// ============================================================
//  Static WndProc  (routes messages to the instance)
// ============================================================
LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg,
                                  WPARAM wParam, LPARAM lParam)
{
    Window* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        // Store the Window* passed in CreateWindowEx
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    else
    {
        self = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self)
        return self->HandleMessage(msg, wParam, lParam);

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ============================================================
//  Instance message handler
// ============================================================
LRESULT Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        width_  = LOWORD(lParam);
        height_ = HIWORD(lParam);
        if (OnResize)
            OnResize(width_, height_);
        return 0;

    case WM_KEYDOWN:
        if (input_) input_->OnKeyDown(static_cast<int>(wParam));
        if (wParam == VK_ESCAPE) PostQuitMessage(0);
        return 0;

    case WM_KEYUP:
        if (input_) input_->OnKeyUp(static_cast<int>(wParam));
        return 0;

    case WM_MOUSEMOVE:
        if (input_) input_->OnMouseMove(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_LBUTTONDOWN:
        if (input_) input_->OnMouseButton(0, true);
        return 0;
    case WM_LBUTTONUP:
        if (input_) input_->OnMouseButton(0, false);
        return 0;

    case WM_RBUTTONDOWN:
        if (input_) input_->OnMouseButton(1, true);
        return 0;
    case WM_RBUTTONUP:
        if (input_) input_->OnMouseButton(1, false);
        return 0;

    case WM_MBUTTONDOWN:
        if (input_) input_->OnMouseButton(2, true);
        return 0;
    case WM_MBUTTONUP:
        if (input_) input_->OnMouseButton(2, false);
        return 0;
    }

    return DefWindowProc(hwnd_, msg, wParam, lParam);
}
