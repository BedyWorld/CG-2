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

// Восстановить карту высот из карты нормалей.
// Тангенс-пространственная нормаль задаёт градиент поверхности
// (dh/du, dh/dv) = (-nx/nz, -ny/nz). Интегрируем его, решая уравнение
// Пуассона Lap(h) = div(g) методом Гаусса-Зейделя с заворотом границ.
// Нужна как замена displacement-текстуре там, где ассет её не содержит
// (Crytek Sponza поставляется только с _diff/_ddn/_spec).
// Результат: grayscale RGBA8, 0.5 = нулевой уровень рельефа.
// Сетка 64x64 при 300 итерациях даёт корреляцию с эталонным рельефом 0.9976
// против 0.9956 у 128x128 при 600 и обходится в 7.7 раза дешевле: на меньшей
// сетке Гаусс-Зейдель сходится за меньшее число проходов, а displacement
// всё равно низкочастотный.
TextureData CreateHeightFromNormal(const TextureData& normalMap,
    UINT outSize = 64, int iterations = 300);

// Legacy
TextureData CreateCheckerboard(UINT size = 256, UINT tileSize = 32);
