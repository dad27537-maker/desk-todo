#pragma once

#include <Windows.h>
#include <string>

namespace OcrService {

enum class Backend { Automatic, Paddle, Windows };
bool RecognizeBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error,
                     Backend backend = Backend::Automatic);
bool RecognizeLatexBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error);
std::wstring ConvertTextToLatex(const std::wstring& text);

}
