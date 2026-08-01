#include "FileService.h"
#include <filesystem>
#include <shellapi.h>
#include <ShlObj.h>

namespace FileService {

bool DescribePath(const std::wstring& fullPath, DesktopItem& item,
                  bool includeHidden) {
    const DWORD attributes = GetFileAttributesW(fullPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (!includeHidden &&
         (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0)) return false;
    std::filesystem::path path(fullPath);
    const std::wstring name = path.filename().wstring();
    if (name.empty()) return false;
    item = {};
    item.name = name;
    item.fullPath = path.lexically_normal().wstring();
    item.isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    item.ext = GetFileExtension(name);
    return true;
}

bool SetLogicalGroupingHidden(const std::wstring& fullPath, bool hidden) {
    const DWORD attributes = GetFileAttributesW(fullPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    constexpr DWORD suppressionAttributes =
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    const DWORD updated = hidden
        ? (attributes | suppressionAttributes)
        : (attributes & ~suppressionAttributes);
    if (updated == attributes) return true;
    if (!SetFileAttributesW(fullPath.c_str(), updated)) return false;

    // This notification is only used to migrate attributes written by older
    // versions. Current logical grouping suppresses the desktop IShellView
    // directly and never hides the real filesystem object.
    SHChangeNotify(hidden
            ? (directory ? SHCNE_RMDIR : SHCNE_DELETE)
            : (directory ? SHCNE_MKDIR : SHCNE_CREATE),
        SHCNF_PATHW | SHCNF_FLUSHNOWAIT, fullPath.c_str(), nullptr);
    SHChangeNotify(SHCNE_ATTRIBUTES, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                   fullPath.c_str(), nullptr);
    const std::wstring parent = std::filesystem::path(fullPath).parent_path().wstring();
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                   parent.c_str(), nullptr);
    return true;
}

std::vector<DesktopItem> ScanFolder(const std::wstring& folderPath) {
    std::vector<DesktopItem> items;
    TryScanFolder(folderPath, items);
    return items;
}

bool TryScanFolder(const std::wstring& folderPath, std::vector<DesktopItem>& items) {
    items.clear();
    const DWORD folderAttributes = GetFileAttributesW(folderPath.c_str());
    if (folderAttributes == INVALID_FILE_ATTRIBUTES ||
        (folderAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;

    WIN32_FIND_DATAW ffd;
    std::wstring searchPath = folderPath + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    do {
        std::wstring name = ffd.cFileName;
        if (name == L"." || name == L"..") continue;

        std::wstring fullPath = folderPath + L"\\" + name;
        if (IsHiddenOrSystem(fullPath)) continue;

        DesktopItem item;
        if (DescribePath(fullPath, item)) items.push_back(std::move(item));
    } while (FindNextFileW(hFind, &ffd) != 0);

    FindClose(hFind);
    return true;
}

bool ShellOpenFile(const std::wstring& fullPath) {
    HINSTANCE result = ShellExecuteW(nullptr, L"open", fullPath.c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    return (INT_PTR)result > 32;
}

std::wstring GetFileExtension(const std::wstring& fileName) {
    size_t dot = fileName.rfind(L'.');
    if (dot == std::wstring::npos) return L"";
    std::wstring ext = fileName.substr(dot);
    for (auto& c : ext) c = towlower(c);
    return ext;
}

bool IsHiddenOrSystem(const std::wstring& fullPath) {
    DWORD attrs = GetFileAttributesW(fullPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;
    return (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
}

}
