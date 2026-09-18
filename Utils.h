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

struct LoadTimer
{
    const char* name;
    LARGE_INTEGER t0;
    explicit LoadTimer(const char* n) : name(n) { QueryPerformanceCounter(&t0); }
    ~LoadTimer()
    {
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        double ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f.QuadPart);
        char buf[192];
        sprintf_s(buf, "[load] %-28s %8.1f ms\n", name, ms);
        OutputDebugStringA(buf);
    }
};
