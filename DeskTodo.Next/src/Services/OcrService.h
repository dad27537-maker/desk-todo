#pragma once

#include <Windows.h>
#include <string>

namespace OcrService {

enum class Backend { Automatic, Paddle, Windows };
bool RecognizeBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error,
                     Backend backend = Backend::Automatic);
// BCP-47 source language; auto retains the existing local-model selection.
// Explicit languages never fall back to an unrelated Windows OCR language.
bool RecognizeBitmapForLanguage(HBITMAP bitmap, std::wstring& text,
                               std::wstring& error, const std::wstring& languageTag);
bool RecognizeLatexBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error);
std::wstring ConvertTextToLatex(const std::wstring& text);

}
