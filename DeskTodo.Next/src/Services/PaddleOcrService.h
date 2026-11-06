#pragma once
#include <Windows.h>
#include <string>

namespace PaddleOcrService {
// Uses only the libraries and models shipped in the executable's ocr directory.
// No screenshot or recognized text is sent over the network.
bool RecognizeBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error);
}
