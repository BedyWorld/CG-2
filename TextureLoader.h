#pragma once
#include <Windows.h>
#include <wincodec.h>
#include <vector>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

struct TextureData
{
    std::vector<uint8_t> pixels; // RGBA8, row-major, top-to-bottom
    UINT width  = 0;
    UINT height = 0;
    bool valid  = false;
};

// Загрузить PNG/JPG/BMP через WIC
TextureData LoadTextureWIC(const std::wstring& path);

// Загрузить TGA нативным парсером (WIC не поддерживает TGA без кодека)
TextureData LoadTextureTGA(const std::wstring& path);

// Автодетект по расширению: .tga -> LoadTextureTGA, остальное -> LoadTextureWIC
TextureData LoadTextureAuto(const std::wstring& path);

// Создать однотонную текстуру
TextureData CreateSolidColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255, UINT size = 4);

// Fallback flat normal map (128, 128, 255)
TextureData CreateFlatNormal(UINT size = 4);

// Legacy
TextureData CreateCheckerboard(UINT size = 256, UINT tileSize = 32);
