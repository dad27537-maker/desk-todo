#include "FenceStorageService.h"
#include "DesktopPathService.h"
#include <filesystem>

namespace FenceStorageService {

std::wstring GetDefaultFenceFolderPath(const std::wstring& fenceId) {
    return GetFenceRootPath() + L"\\" + fenceId;
}

bool EnsureFenceFolder(const std::wstring& fenceId) {
    std::wstring path = GetDefaultFenceFolderPath(fenceId);
    std::error_code ec;
    return std::filesystem::create_directories(path, ec) || !ec;
}

}
