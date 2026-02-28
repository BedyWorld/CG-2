#pragma once
#include <Windows.h>
#include <stdexcept>
#include <string>

inline void ThrowIfFailed(HRESULT hr, const char* msg = "")
{
    if (FAILED(hr))
    {
        char buf[256];
        sprintf_s(buf, "HRESULT=0x%08X  %s", (unsigned)hr, msg);
        throw std::runtime_error(buf);
    }
}
