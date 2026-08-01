#pragma once
#include <string>

namespace FenceStorageService {

std::wstring GetDefaultFenceFolderPath(const std::wstring& fenceId);
bool EnsureFenceFolder(const std::wstring& fenceId);

}
