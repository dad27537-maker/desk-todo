#pragma once
#include <Windows.h>

namespace TranslationWindow {
// Takes ownership of the selected screenshot, including on window creation failure.
void Show(HWND owner, HBITMAP screenshot);
}
