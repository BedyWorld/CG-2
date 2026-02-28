#include "TextureLoader.h"
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// ============================================================
//  Load image via Windows Imaging Component (WIC)
// ============================================================
TextureData LoadTextureWIC(const std::wstring& path)
{
    TextureData td;

    // Initialise COM (safe to call multiple times)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return td;

    // Open file
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder)))
        return td;

    // Get first frame
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return td;

    // Convert to 32-bit RGBA (handles all source formats)
    ComPtr<IWICFormatConverter> conv;
    factory->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom)))
        return td;

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    td.width  = w;
    td.height = h;
    td.pixels.resize(w * h * 4);

    conv->CopyPixels(nullptr, w * 4,
        static_cast<UINT>(td.pixels.size()), td.pixels.data());

    td.valid = true;
    return td;
}

// ============================================================
//  Procedural checkerboard (fallback when no texture file)
// ============================================================
TextureData CreateCheckerboard(UINT size, UINT tileSize)
{
    TextureData td;
    td.width  = size;
    td.height = size;
    td.pixels.resize(size * size * 4);
    td.valid  = true;

    for (UINT y = 0; y < size; ++y)
    {
        for (UINT x = 0; x < size; ++x)
        {
            bool light = ((x / tileSize) + (y / tileSize)) % 2 == 0;
            UINT i = (y * size + x) * 4;
            td.pixels[i + 0] = light ? 210 : 50;   // R
            td.pixels[i + 1] = light ? 210 : 50;   // G
            td.pixels[i + 2] = light ? 220 : 180;  // B (slight blue tint on light)
            td.pixels[i + 3] = 255;                 // A
        }
    }
    return td;
}
