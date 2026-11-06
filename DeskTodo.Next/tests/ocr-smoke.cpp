#include <Windows.h>
#include <Ole2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include "Services/OcrService.h"

using Microsoft::WRL::ComPtr;

namespace {
std::string Utf8(const std::wstring& text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring Compact(const std::wstring& text) {
    std::wstring result;
    for (wchar_t ch : text) if (!std::iswspace(ch)) result += ch;
    return result;
}
HBITMAP ReadImage(const std::filesystem::path& path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom))) return nullptr;
    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap && FAILED(converter->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(pixels)))) {
        DeleteObject(bitmap);
        return nullptr;
    }
    return bitmap;
}
HBITMAP Render(const wchar_t* text, int width, int height, int size,
               COLORREF foreground = RGB(0, 0, 0), COLORREF background = RGB(255, 255, 255)) {
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    auto oldBitmap = SelectObject(dc, bitmap);
    RECT area{0, 0, width, height};
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(dc, &area, brush);
    DeleteObject(brush);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"SimSun");
    auto oldFont = SelectObject(dc, font);
    SetTextColor(dc, foreground);
    SetBkMode(dc, TRANSPARENT);
    area.left = 5; area.top = 5;
    DrawTextW(dc, text, -1, &area, DT_LEFT | DT_TOP);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return bitmap;
}
}

int wmain(int argc, wchar_t** argv) {
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 1;
    int failed = 0, checked = 0;
    auto backend = OcrService::Backend::Automatic;
#if defined(DESKTODO_HAS_PADDLE_OCR)
    // Model/runtime failures must not be hidden by the Windows fallback.
    backend = OcrService::Backend::Paddle;
#endif
    if (argc > 1 && std::wstring(argv[1]) == L"--windows") backend = OcrService::Backend::Windows;
    if (argc > 1 && std::wstring(argv[1]) == L"--automatic") backend = OcrService::Backend::Automatic;
    auto check = [&](const char* name, HBITMAP bitmap, const std::wstring& expected) {
        ++checked;
        auto start = std::chrono::steady_clock::now();
        std::wstring text, error;
        const bool success = OcrService::RecognizeBitmap(bitmap, text, error, backend);
        const bool passed = bitmap && (expected.empty() ? !success : success && Compact(text).find(Compact(expected)) != std::wstring::npos);
        DeleteObject(bitmap);
        std::cout << (passed ? "PASS " : "FAIL ") << name << " ["
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
            << " ms]: " << Utf8(success ? text : error) << '\n';
        if (!passed) ++failed;
    };
    if (argc > 2 && std::wstring(argv[1]) == L"--image") {
        check("supplied image", ReadImage(argv[2]), argc > 3 ? argv[3] : L"文档");
    } else {
        check("original user image", ReadImage(std::filesystem::path(DESKTODO_OCR_FIXTURE_DIR) / L"ocr-small-chinese.png"), L"文档");
        check("small Chinese", Render(L"文档", 65, 43, 18), L"文档");
        check("dark background", Render(L"文字识别", 140, 44, 20, RGB(245, 245, 245), RGB(25, 25, 25)), L"文字识别");
        check("low contrast", Render(L"文档", 90, 44, 18, RGB(170, 170, 170)), L"文档");
        check("English and digits", Render(L"DESKTODO OCR 12345", 1000, 150, 64), L"DESKTODO OCR 12345");
        check("multiline Chinese and English", Render(L"中文文字识别\r\nDeskTodo 2026", 500, 150, 32), L"中文文字识别DeskTodo 2026");
        check("blank", Render(L"", 65, 43, 18), L"");
        std::wstring text, error;
        ++checked;
        if (OcrService::RecognizeBitmap(nullptr, text, error) || error.empty()) ++failed;
        const auto latex = OcrService::ConvertTextToLatex(L"x² + y₂ ≤ 4 × π + a/b");
        ++checked;
        if (latex.find(L"x^{2}") == std::wstring::npos || latex.find(L"y_{2}") == std::wstring::npos ||
            latex.find(L"\\le") == std::wstring::npos || latex.find(L"\\frac{a}{b}") == std::wstring::npos) ++failed;
    }
    OleUninitialize();
    std::cout << checked << " checks, " << failed << " failures\n";
    return failed ? 1 : 0;
}
