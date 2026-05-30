#include "TextureLoader.h"
#include <wrl/client.h>
#include <fstream>
#include <algorithm>
using Microsoft::WRL::ComPtr;

// ============================================================
//  WIC loader (PNG, JPG, BMP, ...)
// ============================================================
TextureData LoadTextureWIC(const std::wstring& path)
{
    TextureData td;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return td;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) return td;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return td;

    ComPtr<IWICFormatConverter> conv;
    factory->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return td;

    UINT w=0, h=0; conv->GetSize(&w, &h);
    td.width=w; td.height=h;
    td.pixels.resize(w*h*4);
    conv->CopyPixels(nullptr, w*4, (UINT)td.pixels.size(), td.pixels.data());
    td.valid = true;
    return td;
}

// ============================================================
//  TGA loader (нативный, без зависимостей)
//  Поддерживает:
//    type 2  — uncompressed RGB/RGBA
//    type 3  — uncompressed grayscale
//    type 10 — RLE RGB/RGBA
// ============================================================
TextureData LoadTextureTGA(const std::wstring& path)
{
    TextureData td;
    std::ifstream f(path, std::ios::binary);
    if (!f) return td;

    // --- Header (18 bytes) ---
    uint8_t hdr[18] = {};
    f.read(reinterpret_cast<char*>(hdr), 18);
    if (f.gcount() < 18) return td;

    uint8_t  idLen      = hdr[0];
    uint8_t  colorMapType = hdr[1];
    uint8_t  imageType  = hdr[2];
    // hdr[3..7] — color map spec (игнорируем)
    uint16_t width  = hdr[12] | (hdr[13] << 8);
    uint16_t height = hdr[14] | (hdr[15] << 8);
    uint8_t  bpp    = hdr[16];
    uint8_t  imgDesc= hdr[17];

    if (width == 0 || height == 0) return td;

    // Пропускаем Image ID
    if (idLen > 0) f.seekg(idLen, std::ios::cur);

    // Пропускаем Color Map если есть
    if (colorMapType == 1)
    {
        uint16_t cmLen    = hdr[5] | (hdr[6] << 8);
        uint8_t  cmDepth  = hdr[7];
        f.seekg(cmLen * ((cmDepth + 7) / 8), std::ios::cur);
    }

    int channels = bpp / 8;
    if (channels < 1 || channels > 4) return td;

    // Читаем пиксели
    uint32_t pixCount = (uint32_t)width * height;
    std::vector<uint8_t> raw;

    if (imageType == 2 || imageType == 3)
    {
        // Uncompressed
        raw.resize((size_t)pixCount * channels);
        f.read(reinterpret_cast<char*>(raw.data()), raw.size());
    }
    else if (imageType == 10)
    {
        // RLE compressed
        raw.reserve((size_t)pixCount * channels);
        while ((uint32_t)raw.size() < (uint32_t)pixCount * channels)
        {
            uint8_t rep; f.read(reinterpret_cast<char*>(&rep), 1);
            uint8_t cnt = (rep & 0x7F) + 1;
            if (rep & 0x80)
            {
                // RLE packet
                uint8_t px[4] = {};
                f.read(reinterpret_cast<char*>(px), channels);
                for (int i = 0; i < cnt; ++i)
                    for (int c = 0; c < channels; ++c)
                        raw.push_back(px[c]);
            }
            else
            {
                // Raw packet
                size_t bytes = (size_t)cnt * channels;
                size_t pos   = raw.size();
                raw.resize(pos + bytes);
                f.read(reinterpret_cast<char*>(raw.data() + pos), bytes);
            }
        }
    }
    else return td; // неподдерживаемый тип

    // Конвертируем в RGBA8
    td.width  = width;
    td.height = height;
    td.pixels.resize((size_t)width * height * 4);

    bool flipV = !(imgDesc & 0x20); // бит 5 = 0 означает bottom-left origin

    for (uint32_t y = 0; y < height; ++y)
    {
        uint32_t srcRow = flipV ? (height - 1 - y) : y;
        for (uint32_t x = 0; x < width; ++x)
        {
            size_t   srcIdx = ((size_t)srcRow * width + x) * channels;
            size_t   dstIdx = ((size_t)y      * width + x) * 4;
            uint8_t* dst    = td.pixels.data() + dstIdx;

            if (channels == 1)
            {
                // Grayscale
                uint8_t g = raw[srcIdx];
                dst[0] = dst[1] = dst[2] = g; dst[3] = 255;
            }
            else if (channels == 3)
            {
                // BGR -> RGBA
                dst[0] = raw[srcIdx + 2]; // R
                dst[1] = raw[srcIdx + 1]; // G
                dst[2] = raw[srcIdx + 0]; // B
                dst[3] = 255;
            }
            else if (channels == 4)
            {
                // BGRA -> RGBA
                dst[0] = raw[srcIdx + 2];
                dst[1] = raw[srcIdx + 1];
                dst[2] = raw[srcIdx + 0];
                dst[3] = raw[srcIdx + 3];
            }
        }
    }

    td.valid = true;
    return td;
}

// ============================================================
//  Автодетект по расширению
// ============================================================
TextureData LoadTextureAuto(const std::wstring& path)
{
    // Приводим расширение к нижнему регистру для сравнения
    std::wstring ext;
    size_t dot = path.rfind(L'.');
    if (dot != std::wstring::npos)
    {
        ext = path.substr(dot);
        for (auto& c : ext) c = towlower(c);
    }

    if (ext == L".tga")
        return LoadTextureTGA(path);
    else
        return LoadTextureWIC(path);
}

// ============================================================
//  Helpers
// ============================================================
TextureData CreateSolidColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a, UINT size)
{
    TextureData td;
    td.width = td.height = size;
    td.pixels.resize(size * size * 4);
    td.valid = true;
    for (UINT i = 0; i < size * size; ++i)
    {
        td.pixels[i*4+0] = r;
        td.pixels[i*4+1] = g;
        td.pixels[i*4+2] = b;
        td.pixels[i*4+3] = a;
    }
    return td;
}

TextureData CreateFlatNormal(UINT size)
{
    return CreateSolidColor(128, 128, 255, 255, size);
}

TextureData CreateCheckerboard(UINT size, UINT tileSize)
{
    return CreateSolidColor(180, 180, 180, 255, 4);
}
