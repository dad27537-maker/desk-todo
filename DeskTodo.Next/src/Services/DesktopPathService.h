#pragma once
#include <string>

std::wstring GetDesktopPath();
std::wstring GetUserDesktopPath();
std::wstring GetPublicDesktopPath();
std::wstring GetAppDataPath();
std::wstring GetConfigPath();
std::wstring GetFenceRootPath();
bool EnsureDirectoryExists(const std::wstring& path);
// Produces the stable path identity used by logical fences. Existing paths are
// resolved through their filesystem handle (junctions/symlinks included);
// unavailable paths still receive an absolute, lexically-normal form.
std::wstring NormalizePathIdentity(const std::wstring& path);
bool IsDesktopFilesystemPath(const std::wstring& path);
