#include "InputDevice.h"
InputDevice::InputDevice(HWND hwnd) : hwnd_(hwnd)
{
    keys_.fill(false);
    mouseBtn_.fill(false);
}
void InputDevice::Initialize()
{
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = 0;
    rid.hwndTarget = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}
void InputDevice::OnKeyDown(int key)
{
    if (key >= 0 && key < 256)
        keys_[key] = true;
}
void InputDevice::OnKeyUp(int key)
{
    if (key >= 0 && key < 256)
        keys_[key] = false;
}
void InputDevice::OnMouseMove(int x, int y)
{
    mouseX_ = x;
    mouseY_ = y;
}
void InputDevice::OnMouseButton(int button, bool pressed)
{
    if (button >= 0 && button < 3)
        mouseBtn_[button] = pressed;
}
bool InputDevice::IsKeyDown(int key) const
{
    if (key >= 0 && key < 256)
        return keys_[key];
    return false;
}
bool InputDevice::IsMouseDown(int btn) const
{
    if (btn >= 0 && btn < 3)
        return mouseBtn_[btn];
    return false;
}
void InputDevice::Update()
{
    mouseDX_ = mouseX_ - prevX_;
    mouseDY_ = mouseY_ - prevY_;
    prevX_ = mouseX_;
    prevY_ = mouseY_;
}