#include <Windows.h>
#include <Ole2.h>

#include <iostream>
#include <string>

#include "Services/OcrService.h"

int main() {
    const std::wstring latex = OcrService::ConvertTextToLatex(
        L"x² + y₂ ≤ 4 × π + a/b");
    const bool latexValid = latex.find(L"x^{2}") != std::wstring::npos &&
        latex.find(L"y_{2}") != std::wstring::npos &&
        latex.find(L"\\le") != std::wstring::npos &&
        latex.find(L"\\times") != std::wstring::npos &&
        latex.find(L"\\pi") != std::wstring::npos &&
        latex.find(L"\\frac{a}{b}") != std::wstring::npos;
    if (!latexValid) {
        std::wcerr << L"LaTeX conversion failed: " << latex << L'\n';
        return 3;
    }
    const HRESULT oleResult = OleInitialize(nullptr);
    constexpr int width = 1000;
    constexpr int height = 240;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    RECT area{ 0, 0, width, height };
    FillRect(memory, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    HFONT font = CreateFontW(-96, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HGDIOBJ oldFont = SelectObject(memory, font);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(0, 0, 0));
    DrawTextW(memory, L"DESKTODO OCR 12345", -1, &area,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(memory, oldFont);
    DeleteObject(font);
    SelectObject(memory, oldBitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    std::wstring text;
    std::wstring error;
    const bool recognized = OcrService::RecognizeBitmap(bitmap, text, error);
    DeleteObject(bitmap);
    if (!recognized) {
        std::wcerr << L"OCR failed: " << error << L'\n';
        if (SUCCEEDED(oleResult)) OleUninitialize();
        return 1;
    }
    std::wcout << L"OCR text: " << text << L'\n';
    const bool valid = !text.empty() && text.find(L"OCR") != std::wstring::npos;
    if (SUCCEEDED(oleResult)) OleUninitialize();
    return valid ? 0 : 2;
}
