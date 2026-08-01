#pragma once

#include <string>

namespace ShellIntegrationService {

bool EnsureDesktopShortcut(const std::wstring& executablePath);
bool EnsureDesktopContextMenu(const std::wstring& executablePath);
bool IsProtectedDesktopItem(const std::wstring& path);
bool CreateItemShortcut(const std::wstring& targetPath,
                        const std::wstring& destinationDirectory,
                        std::wstring* shortcutPath = nullptr);

}
