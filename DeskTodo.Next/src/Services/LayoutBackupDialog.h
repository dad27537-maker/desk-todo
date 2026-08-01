#pragma once

#include <Windows.h>
#include <string>

namespace LayoutBackupDialog {

std::wstring SelectSavePath(HWND owner);
std::wstring SelectOpenPath(HWND owner);

}
