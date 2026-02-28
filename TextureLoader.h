#pragma once
#include <Windows.h>
#include <wincodec.h>
#include <vector>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// Raw RGBA8 pixel data from a loaded image
struct TextureData
{
    std::vector<uint8_t> pixels; // RGBA8, row-major, top-to-bottom
    UINT width  = 0;
    UINT height = 0;
    bool valid  = false;
};

// Load any image format (PNG, JPG, BMP, TGA, DDS...) via WIC
// Returns TextureData.valid == false on failure
TextureData LoadTextureWIC(const std::wstring& path);

// Create a procedural checkerboard texture (used as fallback)
TextureData CreateCheckerboard(UINT size = 256, UINT tileSize = 32);
