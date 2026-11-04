#include "Services/TranslationService.h"
#include <ole2.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <winrt/base.h>

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) != "--live") {
        std::cout << "Pass --live to translate synthetic test text over the network.\n";
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HBITMAP bitmap = nullptr;
    try {
        using namespace TranslationService;
        HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
        bitmap = CreateCompatibleBitmap(screen, 600, 130);
        ReleaseDC(nullptr, screen);
        auto old = SelectObject(dc, bitmap);
        RECT rect{0, 0, 600, 130};
        FillRect(dc, &rect, GetSysColorBrush(COLOR_WINDOW));
        HFONT font = CreateFontW(-36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto oldFont = SelectObject(dc, font);
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        TextOutW(dc, 20, 40, L"Hello world!", 12);
        SelectObject(dc, oldFont);
        SelectObject(dc, old);
        DeleteObject(font);
        DeleteDC(dc);
        GdiFlush();
        Settings settings;
        settings.source = L"en";
        Result result;
        std::wstring error;
        std::atomic<bool> cancelled{false};
        bool screenshotTranslated = false;
        std::thread worker([&] { screenshotTranslated = TranslateBitmap(bitmap, settings, result, error, cancelled); });
        worker.join();
        if (!screenshotTranslated) throw std::runtime_error(winrt::to_string(error));
        if (result.original.find(L"Hello") == std::wstring::npos || result.translated.find(L"世界") == std::wstring::npos)
            throw std::runtime_error("synthetic English screenshot did not translate to Chinese");
        std::cout << "Local OCR -> Google Chinese: " << winrt::to_string(result.original) << " -> " << winrt::to_string(result.translated) << '\n';
        settings.source = L"auto";
        settings.target = L"ja";
        if (!TranslateText(L"Hello world!", settings, result, error, cancelled) || result.translated.find(L"世界") == std::wstring::npos)
            throw std::runtime_error(winrt::to_string(error) + " Japanese translation failed");
        std::cout << "Google Japanese: " << winrt::to_string(result.translated) << '\n';
        settings.provider = Provider::MyMemory;
        settings.target = L"zh-Hans";
        if (!TranslateText(L"This is a simple sentence. Hello world!", settings, result, error, cancelled) || result.translated.empty())
            throw std::runtime_error(winrt::to_string(error));
        std::cout << "Local language detection -> MyMemory Chinese: " << winrt::to_string(result.translated) << '\n';
        DeleteObject(bitmap);
        std::cout << "Live translation passed using only synthetic input; no Youdao service.\n";
        return 0;
    } catch (const std::exception& error) {
        if (bitmap) DeleteObject(bitmap);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
