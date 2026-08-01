#pragma once
#include <string>
#include <vector>
#include "Models/DesktopItem.h"

namespace FileService {

std::vector<DesktopItem> ScanFolder(const std::wstring& folderPath);
bool TryScanFolder(const std::wstring& folderPath, std::vector<DesktopItem>& items);
bool DescribePath(const std::wstring& fullPath, DesktopItem& item,
                  bool includeHidden = false);
// Legacy attribute migration helper. New logical grouping never passes true:
// hiding the real path also hides it from C:\Users\...\Desktop in Explorer.
bool SetLogicalGroupingHidden(const std::wstring& fullPath, bool hidden);
bool ShellOpenFile(const std::wstring& fullPath);
std::wstring GetFileExtension(const std::wstring& fileName);
bool IsHiddenOrSystem(const std::wstring& fullPath);

}
