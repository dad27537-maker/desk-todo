#pragma once

#include <Windows.h>
#include <string>

namespace OcrService {

bool RecognizeBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error);
bool RecognizeLatexBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error);
std::wstring ConvertTextToLatex(const std::wstring& text);

}
