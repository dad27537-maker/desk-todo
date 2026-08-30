#pragma once

#include <Windows.h>

#include <string>

namespace AppSettings {

struct Hotkey {
    UINT modifiers = MOD_CONTROL | MOD_SHIFT;
    UINT virtualKey = 'A';
};

constexpr UINT DefaultScreenshotHotkeyModifiers = MOD_CONTROL | MOD_SHIFT;
constexpr UINT DefaultScreenshotHotkey = 'A';

inline bool IsValidScreenshotHotkey(const Hotkey& hotkey) {
    constexpr UINT allowedModifiers = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    return (hotkey.modifiers & allowedModifiers) != 0 &&
           (hotkey.modifiers & ~allowedModifiers) == 0 &&
           hotkey.virtualKey >= 0x20 && hotkey.virtualKey <= 0xFE;
}

inline Hotkey DefaultScreenshotHotkeyValue() {
    return { DefaultScreenshotHotkeyModifiers, DefaultScreenshotHotkey };
}

inline HKEY OpenSettingsKey(REGSAM access) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DeskTodo\\Settings",
                      0, access, &key) != ERROR_SUCCESS) {
        return nullptr;
    }
    return key;
}

inline Hotkey GetScreenshotHotkey() {
    Hotkey result = DefaultScreenshotHotkeyValue();
    HKEY key = OpenSettingsKey(KEY_QUERY_VALUE);
    if (!key) return result;

    DWORD modifiers = 0;
    DWORD virtualKey = 0;
    DWORD bytes = sizeof(DWORD);
    const bool readModifiers =
        RegQueryValueExW(key, L"ScreenshotHotkeyModifiers", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&modifiers), &bytes) == ERROR_SUCCESS;
    bytes = sizeof(DWORD);
    const bool readVirtualKey =
        RegQueryValueExW(key, L"ScreenshotHotkeyVirtualKey", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&virtualKey), &bytes) == ERROR_SUCCESS;
    RegCloseKey(key);

    Hotkey stored{ modifiers, virtualKey };
    return readModifiers && readVirtualKey && IsValidScreenshotHotkey(stored)
        ? stored : result;
}

inline bool SetScreenshotHotkey(const Hotkey& hotkey) {
    if (!IsValidScreenshotHotkey(hotkey)) return false;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\DeskTodo\\Settings",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const DWORD modifiers = hotkey.modifiers;
    const DWORD virtualKey = hotkey.virtualKey;
    const LONG modifiersResult = RegSetValueExW(
        key, L"ScreenshotHotkeyModifiers", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&modifiers), sizeof(modifiers));
    const LONG virtualKeyResult = RegSetValueExW(
        key, L"ScreenshotHotkeyVirtualKey", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&virtualKey), sizeof(virtualKey));
    RegCloseKey(key);
    return modifiersResult == ERROR_SUCCESS && virtualKeyResult == ERROR_SUCCESS;
}

inline std::wstring HotkeyKeyName(UINT virtualKey) {
    if (virtualKey >= '0' && virtualKey <= '9') return std::wstring(1, static_cast<wchar_t>(virtualKey));
    if (virtualKey >= 'A' && virtualKey <= 'Z') return std::wstring(1, static_cast<wchar_t>(virtualKey));
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
    }
    switch (virtualKey) {
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_INSERT: return L"Insert";
    case VK_DELETE: return L"Delete";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PageUp";
    case VK_NEXT: return L"PageDown";
    case VK_LEFT: return L"Left";
    case VK_RIGHT: return L"Right";
    case VK_UP: return L"Up";
    case VK_DOWN: return L"Down";
    default: return L"按键";
    }
}

inline std::wstring FormatHotkey(const Hotkey& hotkey) {
    std::wstring result;
    if (hotkey.modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (hotkey.modifiers & MOD_ALT) result += L"Alt+";
    if (hotkey.modifiers & MOD_SHIFT) result += L"Shift+";
    if (hotkey.modifiers & MOD_WIN) result += L"Win+";
    result += HotkeyKeyName(hotkey.virtualKey);
    return result;
}

inline Hotkey FromHotkeyControl(WORD packed) {
    // HOTKEY_CLASS uses the HOTKEYF_* bit layout for ALT/CTRL/SHIFT. Its
    // fourth bit means an extended key rather than MOD_WIN, so do not pass it
    // through as a Windows-key modifier.
    return { static_cast<UINT>(HIBYTE(packed)) &
                 (MOD_ALT | MOD_CONTROL | MOD_SHIFT),
             LOBYTE(packed) };
}

inline WORD ToHotkeyControl(Hotkey hotkey) {
    return MAKEWORD(static_cast<BYTE>(hotkey.virtualKey),
                    static_cast<BYTE>(hotkey.modifiers &
                                      (MOD_ALT | MOD_CONTROL | MOD_SHIFT)));
}

} // namespace AppSettings
