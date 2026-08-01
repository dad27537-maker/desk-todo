#include "ShellIntegrationService.h"

#include "DesktopPathService.h"

#include <Windows.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <shobjidl.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {

bool SetRegistryString(HKEY root, const std::wstring& subKey,
                       const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

struct MenuCommand {
    const wchar_t* order;
    const wchar_t* id;
    const wchar_t* title;
    const wchar_t* argument;
};

constexpr MenuCommand Commands[] = {
    { L"01", L"Refresh", L"\u5237\u65b0\u683c\u5b50", L"--refresh" },
    { L"02", L"NewFence", L"\u65b0\u5efa\u683c\u5b50", L"--new-fence" },
    { L"03", L"AutoOrganize", L"\u4e00\u952e\u6574\u7406\u684c\u9762", L"--auto-organize" },
    { L"04", L"ToggleVisibility", L"\u663e\u793a/\u9690\u85cf\u6240\u6709\u683c\u5b50", L"--toggle-visibility" },
    { L"05", L"ToggleLock", L"\u9501\u5b9a/\u89e3\u9501\u5168\u90e8\u683c\u5b50", L"--toggle-lock" },
    { L"06", L"Settings", L"\u8bbe\u7f6e", L"--settings" },
    { L"07", L"Screenshot", L"\u622a\u56fe\u4e0e\u8d34\u56fe", L"--screenshot" },
    { L"08", L"BackupLayout", L"\u5907\u4efd\u684c\u9762\u5e03\u5c40", L"--backup-layout" },
    { L"09", L"RestoreLayout", L"\u8fd8\u539f\u684c\u9762\u5e03\u5c40", L"--restore-layout" },
    { L"10", L"Exit", L"\u9000\u51fa DeskTodo", L"--exit" },
};

void DeleteExistingCommandStoreEntries() {
    const std::wstring commandStore =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, commandStore.c_str(), 0,
                      KEY_READ | KEY_WRITE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) {
        return;
    }

    std::vector<std::wstring> namesToDelete;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(key, index, name, &nameLength,
                                          nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) continue;
        if (wcsncmp(name, L"DeskTodo.", 9) == 0) {
            namesToDelete.emplace_back(name);
        }
    }

    for (const auto& name : namesToDelete) {
        RegDeleteTreeW(key, name.c_str());
    }
    RegCloseKey(key);
}

void DeleteDeskTodoShellEntries(const std::wstring& shellRoot) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, shellRoot.c_str(), 0,
                      KEY_READ | KEY_WRITE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) return;
    std::vector<std::wstring> namesToDelete;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(key, index, name, &nameLength,
                                          nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) continue;
        if (_wcsnicmp(name, L"DeskTodo", 8) == 0) namesToDelete.emplace_back(name);
    }
    for (const auto& name : namesToDelete) RegDeleteTreeW(key, name.c_str());
    RegCloseKey(key);
}

}

namespace ShellIntegrationService {

bool IsProtectedDesktopItem(const std::wstring& path) {
    return false;
}

bool CreateItemShortcut(const std::wstring& targetPath,
                        const std::wstring& destinationDirectory,
                        std::wstring* shortcutPath) {
    if (shortcutPath) shortcutPath->clear();
    if (targetPath.empty() || destinationDirectory.empty() ||
        !std::filesystem::exists(targetPath)) return false;

    std::error_code error;
    std::filesystem::create_directories(destinationDirectory, error);
    if (error) return false;

    const std::filesystem::path target(targetPath);
    std::wstring base = target.filename().wstring();
    if (base.empty()) base = L"项目";
    std::filesystem::path destination =
        std::filesystem::path(destinationDirectory) / (base + L".lnk");
    for (int index = 2; std::filesystem::exists(destination) && index < 10000; ++index) {
        destination = std::filesystem::path(destinationDirectory) /
            (base + L" (" + std::to_wstring(index) + L").lnk");
    }

    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&shellLink)))) return false;
    shellLink->SetPath(targetPath.c_str());
    shellLink->SetWorkingDirectory(
        (std::filesystem::is_directory(target, error) ? target : target.parent_path()).c_str());

    IPersistFile* persist = nullptr;
    bool saved = false;
    if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&persist)))) {
        saved = SUCCEEDED(persist->Save(destination.c_str(), TRUE));
        persist->Release();
    }
    shellLink->Release();
    if (saved && shortcutPath) *shortcutPath = destination.wstring();
    return saved;
}

bool EnsureDesktopShortcut(const std::wstring& executablePath) {
    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&shellLink)))) {
        return false;
    }
    shellLink->SetPath(executablePath.c_str());
    shellLink->SetWorkingDirectory(std::filesystem::path(executablePath).parent_path().c_str());
    shellLink->SetDescription(L"DeskTodo 桌面整理");
    shellLink->SetIconLocation(executablePath.c_str(), 0);
    shellLink->SetHotkey(MAKEWORD('D', HOTKEYF_CONTROL | HOTKEYF_ALT));

    IPersistFile* persist = nullptr;
    bool saved = false;
    if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&persist))) && persist) {
        const std::wstring desktop = GetUserDesktopPath();
        const std::wstring shortcutPath = desktop + L"\\DeskTodo 启动.lnk";
        const bool shortcutExisted =
            std::filesystem::exists(shortcutPath);
        saved = SUCCEEDED(persist->Save(shortcutPath.c_str(), TRUE));
        persist->Release();
        if (saved) {
            const std::vector<std::wstring> legacyShortcuts{
                desktop + L"\\DeskTodo.lnk",
                desktop + L"\\DeskTodo.Next.lnk"
            };
            for (const auto& legacyShortcut : legacyShortcuts) {
                if (DeleteFileW(legacyShortcut.c_str())) {
                    SHChangeNotify(
                        SHCNE_DELETE,
                        SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                        legacyShortcut.c_str(), nullptr);
                }
            }
            SHChangeNotify(
                shortcutExisted
                    ? SHCNE_UPDATEITEM
                    : SHCNE_CREATE,
                SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                shortcutPath.c_str(), nullptr);
        }
    }
    shellLink->Release();
    return saved;
}

bool EnsureDesktopContextMenu(const std::wstring& executablePath) {
    // Context-menu integration was removed. Clean current and legacy entries
    // on every launch so an older build cannot leave duplicate commands behind.
    (void)executablePath;
    DeleteDeskTodoShellEntries(L"Software\\Classes\\DesktopBackground\\Shell");
    DeleteDeskTodoShellEntries(L"Software\\Classes\\Directory\\Background\\shell");
    DeleteExistingCommandStoreEntries();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#if 0
    const std::wstring root = L"Software\\Classes\\DesktopBackground\\Shell\\DeskTodo";
    HKEY menuKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, root.c_str(), 0,
                      KEY_WRITE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY,
                      &menuKey) == ERROR_SUCCESS) {
        RegDeleteTreeW(menuKey, L"shell");
        RegCloseKey(menuKey);
    }
    DeleteExistingCommandStoreEntries();

    std::wstring subCommands;
    for (size_t i = 0; i < std::size(Commands); ++i) {
        if (i > 0) subCommands += L";";
        subCommands += L"DeskTodo.";
        subCommands += Commands[i].id;
    }
    bool ok = SetRegistryString(HKEY_CURRENT_USER, root, L"MUIVerb", L"DeskTodo 桌面整理") &&
              SetRegistryString(HKEY_CURRENT_USER, root, L"Icon", executablePath) &&
              SetRegistryString(HKEY_CURRENT_USER, root, L"SubCommands", subCommands);

    const std::wstring commandStore =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\DeskTodo.";
    for (const auto& command : Commands) {
        const std::wstring commandKey = commandStore + command.id;
        ok = SetRegistryString(HKEY_CURRENT_USER, commandKey, L"MUIVerb", command.title) && ok;
        ok = SetRegistryString(HKEY_CURRENT_USER, commandKey, L"Icon", executablePath) && ok;
        ok = SetRegistryString(HKEY_CURRENT_USER, commandKey + L"\\command", nullptr,
                               Quote(executablePath) + L" " + command.argument) && ok;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
#endif
}

}
