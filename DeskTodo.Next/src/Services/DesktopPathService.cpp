#include "DesktopPathService.h"
#include <ShlObj.h>
#include <filesystem>
#include <vector>

namespace {

std::wstring StripExtendedPrefix(std::wstring value) {
    if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        value.replace(0, 8, L"\\\\");
    } else if (value.rfind(L"\\\\?\\", 0) == 0) {
        value.erase(0, 4);
    }
    return value;
}

} // namespace

std::wstring GetDesktopPath() {
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path) == S_OK) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    return L"C:\\Users\\Public\\Desktop";
}

std::wstring GetAppDataPath() {
    wchar_t overridePath[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(
        L"DESKTODO_DATA_ROOT", overridePath, static_cast<DWORD>(std::size(overridePath)));
    if (overrideLength > 0 && overrideLength < std::size(overridePath)) {
        return overridePath;
    }

    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path) == S_OK) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result + L"\\DeskTodo.Next";
    }
    return L"C:\\DeskTodo.Next";
}

std::wstring GetConfigPath() {
    return GetAppDataPath() + L"\\config.json";
}

std::wstring GetUserDesktopPath() {
    wchar_t overridePath[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(
        L"DESKTODO_DESKTOP_ROOT", overridePath,
        static_cast<DWORD>(std::size(overridePath)));
    if (overrideLength > 0 && overrideLength < std::size(overridePath)) {
        return overridePath;
    }

    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path) == S_OK) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    return L"";
}

std::wstring GetPublicDesktopPath() {
    wchar_t overridePath[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(
        L"DESKTODO_PUBLIC_DESKTOP_ROOT", overridePath,
        static_cast<DWORD>(std::size(overridePath)));
    if (overrideLength > 0 && overrideLength < std::size(overridePath)) return overridePath;
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_PublicDesktop, 0, nullptr, &path) == S_OK) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    return L"";
}

std::wstring GetFenceRootPath() {
    return GetAppDataPath() + L"\\Fences";
}

bool EnsureDirectoryExists(const std::wstring& path) {
    std::error_code ec;
    return std::filesystem::create_directories(path, ec) || !ec;
}

std::wstring NormalizePathIdentity(const std::wstring& path) {
    if (path.empty()) return {};

    std::wstring normalized;
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required > 0) {
        std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
        const DWORD written = GetFullPathNameW(
            path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (written > 0 && written < buffer.size()) normalized.assign(buffer.data(), written);
    }
    if (normalized.empty()) normalized = path;

    HANDLE handle = CreateFileW(normalized.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        const DWORD finalRequired = GetFinalPathNameByHandleW(
            handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (finalRequired > 0) {
            std::vector<wchar_t> finalBuffer(static_cast<size_t>(finalRequired) + 1);
            const DWORD finalWritten = GetFinalPathNameByHandleW(
                handle, finalBuffer.data(), static_cast<DWORD>(finalBuffer.size()),
                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (finalWritten > 0 && finalWritten < finalBuffer.size()) {
                normalized.assign(finalBuffer.data(), finalWritten);
            }
        }
        CloseHandle(handle);
    }

    return std::filesystem::path(StripExtendedPrefix(normalized))
        .lexically_normal().wstring();
}

bool IsDesktopFilesystemPath(const std::wstring& path) {
    const std::wstring parent = NormalizePathIdentity(
        std::filesystem::path(path).parent_path().wstring());
    if (parent.empty()) return false;
    const bool overridden = GetEnvironmentVariableW(
        L"DESKTODO_DESKTOP_ROOT", nullptr, 0) > 0 || GetEnvironmentVariableW(
        L"DESKTODO_PUBLIC_DESKTOP_ROOT", nullptr, 0) > 0;
    static const std::wstring cachedUser = NormalizePathIdentity(GetUserDesktopPath());
    static const std::wstring cachedPublic = NormalizePathIdentity(GetPublicDesktopPath());
    const std::wstring user = overridden
        ? NormalizePathIdentity(GetUserDesktopPath()) : cachedUser;
    const std::wstring publicDesktop = overridden
        ? NormalizePathIdentity(GetPublicDesktopPath()) : cachedPublic;
    return (!user.empty() && _wcsicmp(parent.c_str(), user.c_str()) == 0) ||
           (!publicDesktop.empty() && _wcsicmp(parent.c_str(), publicDesktop.c_str()) == 0);
}
