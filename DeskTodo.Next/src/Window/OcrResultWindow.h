#pragma once

#include <Windows.h>
#include <string>

namespace OcrResultWindow {

void Show(HWND owner, const std::wstring& text,
          const std::wstring& title = L"OCR 识别结果");

}
