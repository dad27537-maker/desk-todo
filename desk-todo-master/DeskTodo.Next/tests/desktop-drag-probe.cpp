#include <Windows.h>
#include <CommCtrl.h>
#include <ole2.h>
#include <shellapi.h>
#include <ShlObj.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

#include "Services/DesktopShellService.h"
#include "Services/ShellIntegrationService.h"

namespace {

void SendLeftButton(DWORD flags) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    SendInput(1, &input, sizeof(input));
}

void SendKey(WORD key, DWORD flags) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = flags;
    SendInput(1, &input, sizeof(input));
}

void SendMouseWheel(int delta) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    SendInput(1, &input, sizeof(input));
}

void SendUnicodeText(const std::wstring& text) {
    for (wchar_t character : text) {
        const SHORT key = VkKeyScanW(character);
        if (key != -1) {
            const WORD virtualKey = LOBYTE(key);
            if (HIBYTE(key) & 1) SendKey(VK_SHIFT, 0);
            SendKey(virtualKey, 0);
            SendKey(virtualKey, KEYEVENTF_KEYUP);
            if (HIBYTE(key) & 1) SendKey(VK_SHIFT, KEYEVENTF_KEYUP);
            Sleep(30);
            continue;
        }
        INPUT inputs[2]{};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = character;
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[1] = inputs[0];
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
    }
}

void SendWindowMouse(HWND window, UINT message, int x, int y, bool down) {
    SendMessageW(window, message, down ? MK_LBUTTON : 0, MAKELPARAM(x, y));
}

void ClickWindow(HWND window, int x, int y) {
    SendWindowMouse(window, WM_LBUTTONDOWN, x, y, true);
    SendWindowMouse(window, WM_LBUTTONUP, x, y, false);
}

POINT SetPhysicalCursorPosition(POINT desired) {
    POINT command = desired;
    POINT actual{};
    for (int attempt = 0; attempt < 4; ++attempt) {
        SetCursorPos(command.x, command.y);
        Sleep(50);
        if (!GetPhysicalCursorPos(&actual)) break;
        const int dx = desired.x - actual.x;
        const int dy = desired.y - actual.y;
        if (std::abs(dx) <= 1 && std::abs(dy) <= 1) break;
        command.x += dx;
        command.y += dy;
    }
    return actual;
}

HWND FindVisiblePopupMenuForProcess(DWORD processId) {
    HWND after = nullptr;
    while ((after = FindWindowExW(nullptr, after, L"#32768", nullptr)) != nullptr) {
        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(after, &windowProcessId);
        if (windowProcessId == processId && IsWindowVisible(after)) return after;
    }
    return nullptr;
}

HWND FindVisibleTooltipForProcess(DWORD processId) {
    HWND after = nullptr;
    while ((after = FindWindowExW(nullptr, after, TOOLTIPS_CLASSW, nullptr)) != nullptr) {
        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(after, &windowProcessId);
        if (windowProcessId == processId && IsWindowVisible(after)) return after;
    }
    return nullptr;
}

bool SelectFenceRefreshMenu(HWND overlay, POINT menuPoint) {
    if (!overlay) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(overlay, &processId);
    SetPhysicalCursorPosition(menuPoint);
    PostMessageW(overlay, WM_RBUTTONUP, 0, 0);

    HWND popup = nullptr;
    for (int attempt = 0; attempt < 40 && !popup; ++attempt) {
        Sleep(25);
        popup = FindVisiblePopupMenuForProcess(processId);
    }
    if (!popup) return false;

    constexpr UINT MN_GETHMENU_MESSAGE = 0x01E1;
    HMENU menu = reinterpret_cast<HMENU>(
        SendMessageW(popup, MN_GETHMENU_MESSAGE, 0, 0));
    RECT itemRect{};
    constexpr int REFRESH_MENU_INDEX = 7;
    if (!menu || !GetMenuItemRect(nullptr, menu, REFRESH_MENU_INDEX, &itemRect)) {
        SendKey(VK_ESCAPE, 0);
        SendKey(VK_ESCAPE, KEYEVENTF_KEYUP);
        return false;
    }
    SetPhysicalCursorPosition({ (itemRect.left + itemRect.right) / 2,
                                (itemRect.top + itemRect.bottom) / 2 });
    SendLeftButton(MOUSEEVENTF_LEFTDOWN);
    SendLeftButton(MOUSEEVENTF_LEFTUP);
    return true;
}

std::vector<DWORD> CaptureScreenRegion(int x, int y, int width, int height) {
    std::vector<DWORD> pixels(static_cast<size_t>(width) * height);
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                      &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, x, y,
           SRCCOPY | CAPTUREBLT);
    if (bits) {
        memcpy(pixels.data(), bits, pixels.size() * sizeof(DWORD));
    }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return pixels;
}

int CountChangedPixels(const std::vector<DWORD>& baseline,
                       const std::vector<DWORD>& current) {
    const size_t count = (std::min)(baseline.size(), current.size());
    int changed = 0;
    for (size_t index = 0; index < count; ++index) {
        const DWORD a = baseline[index];
        const DWORD b = current[index];
        const int db = std::abs(static_cast<int>(a & 0xff) -
                                static_cast<int>(b & 0xff));
        const int dg = std::abs(static_cast<int>((a >> 8) & 0xff) -
                                static_cast<int>((b >> 8) & 0xff));
        const int dr = std::abs(static_cast<int>((a >> 16) & 0xff) -
                                static_cast<int>((b >> 16) & 0xff));
        if (db + dg + dr > 36) ++changed;
    }
    return changed;
}

}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (argc == 3 && wcscmp(argv[1], L"--diagnose-shell") == 0) {
        if (FAILED(OleInitialize(nullptr))) return 3;
        std::wcout << DesktopShellService::DiagnoseDesktopView(argv[2]) << L"\n";
        OleUninitialize();
        return 0;
    }

    if (argc == 2 && wcscmp(argv[1], L"--refresh-desktop") == 0) {
        if (FAILED(OleInitialize(nullptr))) return 3;
        DesktopShellService::RefreshDesktop();
        OleUninitialize();
        Sleep(500);
        return 0;
    }

    if (argc == 5 && wcscmp(argv[1], L"--restore-path") == 0) {
        if (FAILED(OleInitialize(nullptr))) return 3;
        const std::wstring path = argv[2];
        const POINT point{ _wtoi(argv[3]), _wtoi(argv[4]) };
        DesktopShellService::SetPathSuppressedOnDesktop(path, false);
        const bool positioned = DesktopShellService::RestorePathToDesktopPosition(
            path, point);
        OleUninitialize();
        return positioned ? 0 : 7;
    }

    if (argc == 3 && wcscmp(argv[1], L"--suppress-path") == 0) {
        if (FAILED(OleInitialize(nullptr))) return 3;
        const bool suppressed = DesktopShellService::SetPathSuppressedOnDesktop(
            argv[2], true);
        POINT position{};
        const bool stillLocated = DesktopShellService::GetPathPositionOnDesktop(
            argv[2], position);
        std::wcout << L"suppressed=" << suppressed
                   << L" item-position-readable=" << stillLocated;
        if (stillLocated) {
            std::wcout << L" position=" << position.x << L"," << position.y;
        }
        std::wcout << L" " << DesktopShellService::DiagnoseDesktopView(argv[2])
                   << L"\n";
        OleUninitialize();
        return suppressed ? 0 : 8;
    }

    if (argc == 3 && (wcscmp(argv[1], L"--verify-refresh-hidden") == 0 ||
                      wcscmp(argv[1], L"--verify-refresh-hidden-key") == 0)) {
        if (FAILED(OleInitialize(nullptr))) return 3;
        HWND overlay = FindWindowW(L"DeskTodoNext_Overlay", nullptr);
        if (!overlay) {
            OleUninitialize();
            return 10;
        }

        const std::wstring path = argv[2];
        int visibleSamples = 0;
        int totalSamples = 0;
        int visibleRefreshes = 0;
        ULONGLONG firstVisibleMs = 0;
        ULONGLONG lastVisibleMs = 0;
        const bool usePhysicalKey =
            wcscmp(argv[1], L"--verify-refresh-hidden-key") == 0;
        constexpr int refreshCount = 8;
        for (int refresh = 0; refresh < refreshCount; ++refresh) {
            const ULONGLONG refreshStarted = GetTickCount64();
            if (usePhysicalKey) {
                SendKey(VK_F5, 0);
                SendKey(VK_F5, KEYEVENTF_KEYUP);
            } else {
                PostMessageW(overlay, WM_HOTKEY, 1002, 0);
            }

            const ULONGLONG deadline = GetTickCount64() + 900;
            bool visibleThisRefresh = false;
            while (GetTickCount64() < deadline) {
                POINT position{};
                const bool visible = DesktopShellService::GetPathPositionOnDesktop(
                    path, position);
                ++totalSamples;
                if (visible) {
                    ++visibleSamples;
                    visibleThisRefresh = true;
                    const ULONGLONG elapsed = GetTickCount64() - refreshStarted;
                    if (!firstVisibleMs) firstVisibleMs = elapsed;
                    lastVisibleMs = elapsed;
                }
                Sleep(2);
            }
            if (visibleThisRefresh) ++visibleRefreshes;
        }

        const bool fileStillExists =
            GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        POINT finalPosition{};
        const bool finallyVisible = DesktopShellService::GetPathPositionOnDesktop(
            path, finalPosition);
        OleUninitialize();

        std::wcout << L"refreshes=" << refreshCount
                   << L" samples=" << totalSamples
                   << L" visible-samples=" << visibleSamples
                   << L" visible-refreshes=" << visibleRefreshes
                   << L" first-visible-ms=" << firstVisibleMs
                   << L" last-visible-ms=" << lastVisibleMs
                   << L" finally-visible=" << (finallyVisible ? 1 : 0)
                   << L" file-exists=" << (fileStillExists ? 1 : 0) << L"\n";
        return visibleSamples == 0 && !finallyVisible && fileStillExists ? 0 : 13;
    }

    if (argc == 6 && wcscmp(argv[1], L"--verify-refresh-pixels") == 0) {
        HWND overlay = FindWindowW(L"DeskTodoNext_Overlay", nullptr);
        if (!overlay) return 10;
        const int x = _wtoi(argv[2]);
        const int y = _wtoi(argv[3]);
        const int width = _wtoi(argv[4]);
        const int height = _wtoi(argv[5]);
        if (width <= 0 || height <= 0) return 2;

        SetPhysicalCursorPosition({ x + width + 200, y + height + 200 });
        Sleep(500);
        const auto baseline = CaptureScreenRegion(x, y, width, height);
        int idleMaxChanged = 0;
        const ULONGLONG idleDeadline = GetTickCount64() + 700;
        while (GetTickCount64() < idleDeadline) {
            idleMaxChanged = (std::max)(idleMaxChanged, CountChangedPixels(
                baseline, CaptureScreenRegion(x, y, width, height)));
            Sleep(5);
        }

        int refreshMaxChanged = 0;
        constexpr int refreshCount = 8;
        for (int refresh = 0; refresh < refreshCount; ++refresh) {
            SendKey(VK_F5, 0);
            SendKey(VK_F5, KEYEVENTF_KEYUP);
            const ULONGLONG deadline = GetTickCount64() + 900;
            while (GetTickCount64() < deadline) {
                refreshMaxChanged = (std::max)(refreshMaxChanged,
                    CountChangedPixels(baseline,
                        CaptureScreenRegion(x, y, width, height)));
                Sleep(5);
            }
        }

        const int allowedChanged = idleMaxChanged + 64;
        std::wcout << L"refreshes=" << refreshCount
                   << L" region=" << x << L"," << y << L"," << width
                   << L"," << height
                   << L" idle-max-changed=" << idleMaxChanged
                   << L" refresh-max-changed=" << refreshMaxChanged
                   << L" allowed=" << allowedChanged << L"\n";
        return refreshMaxChanged <= allowedChanged ? 0 : 14;
    }

    if (argc == 8 &&
        wcscmp(argv[1], L"--verify-fence-refresh-pixels") == 0) {
        HWND overlay = FindWindowW(L"DeskTodoNext_Overlay", nullptr);
        if (!overlay) return 10;
        const POINT menuPoint{ _wtoi(argv[2]), _wtoi(argv[3]) };
        const int x = _wtoi(argv[4]);
        const int y = _wtoi(argv[5]);
        const int width = _wtoi(argv[6]);
        const int height = _wtoi(argv[7]);
        if (width <= 0 || height <= 0) return 2;

        SetPhysicalCursorPosition({ x + width + 200, y + height + 200 });
        Sleep(500);
        const auto baseline = CaptureScreenRegion(x, y, width, height);
        int maxChanged = 0;
        int menuFailures = 0;
        constexpr int refreshCount = 8;
        for (int refresh = 0; refresh < refreshCount; ++refresh) {
            if (!SelectFenceRefreshMenu(overlay, menuPoint)) {
                ++menuFailures;
                continue;
            }
            const ULONGLONG deadline = GetTickCount64() + 900;
            while (GetTickCount64() < deadline) {
                maxChanged = (std::max)(maxChanged, CountChangedPixels(
                    baseline, CaptureScreenRegion(x, y, width, height)));
                Sleep(5);
            }
        }
        std::wcout << L"fence-menu-refreshes=" << refreshCount
                   << L" menu-failures=" << menuFailures
                   << L" region=" << x << L"," << y << L"," << width
                   << L"," << height
                   << L" max-changed=" << maxChanged << L"\n";
        return menuFailures == 0 && maxChanged <= 64 ? 0 : 15;
    }

    if (argc == 3 && wcscmp(argv[1], L"--notify-created") == 0) {
        const DWORD attributes = GetFileAttributesW(argv[2]);
        if (attributes == INVALID_FILE_ATTRIBUTES) return 9;
        SHChangeNotify((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                ? SHCNE_MKDIR : SHCNE_CREATE,
            SHCNF_PATHW | SHCNF_FLUSH, argv[2], nullptr);
        DesktopShellService::RefreshDesktop();
        Sleep(500);
        return 0;
    }

    if (argc == 4 && wcscmp(argv[1], L"--cursor") == 0) {
        const POINT actual = SetPhysicalCursorPosition(
            { _wtoi(argv[2]), _wtoi(argv[3]) });
        std::wcout << L"cursor=" << actual.x << L"," << actual.y << L"\n";
        return 0;
    }

    if (argc == 8 && wcscmp(argv[1], L"--fence-menu-dismiss") == 0) {
        const POINT menuPoint{ _wtoi(argv[2]), _wtoi(argv[3]) };
        const POINT outsidePoint{ _wtoi(argv[4]), _wtoi(argv[5]) };
        const POINT collapsePoint{ _wtoi(argv[6]), _wtoi(argv[7]) };

        HWND overlay = FindWindowW(L"DeskTodoNext_Overlay", nullptr);
        if (!overlay) return 10;
        DWORD overlayProcessId = 0;
        GetWindowThreadProcessId(overlay, &overlayProcessId);

        // The overlay intentionally uses WS_EX_NOACTIVATE. Post the click to
        // its input path so this probe is deterministic even when another
        // application's modal dialog owns the foreground test desktop.
        SetPhysicalCursorPosition(menuPoint);
        PostMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        PostMessageW(overlay, WM_LBUTTONUP, 0, 0);

        HWND popup = nullptr;
        for (int attempt = 0; attempt < 30 && !popup; ++attempt) {
            Sleep(50);
            popup = FindVisiblePopupMenuForProcess(overlayProcessId);
        }
        const bool menuOpened = popup != nullptr;

        SetPhysicalCursorPosition(outsidePoint);
        SendLeftButton(MOUSEEVENTF_LEFTDOWN);
        SendLeftButton(MOUSEEVENTF_LEFTUP);
        bool menuDismissed = false;
        for (int attempt = 0; attempt < 30; ++attempt) {
            Sleep(50);
            if (!popup || !IsWindow(popup) || !IsWindowVisible(popup)) {
                menuDismissed = true;
                break;
            }
        }

        // The original bug left the overlay stuck in menu mode, so the next
        // title-bar click was swallowed. Exercise the collapse control after
        // dismissal to prove normal fence input resumed.
        Sleep(250);
        SetPhysicalCursorPosition(collapsePoint);
        SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        SendMessageW(overlay, WM_LBUTTONUP, 0, 0);
        Sleep(500);

        std::wcout << L"menu-opened=" << (menuOpened ? 1 : 0)
                   << L" outside-dismissed=" << (menuDismissed ? 1 : 0)
                   << L" collapse-clicked=1\n";
        return menuOpened && menuDismissed ? 0 : 11;
    }

    if (argc == 6 && wcscmp(argv[1], L"--tooltip-click-away") == 0) {
        const POINT itemPoint{ _wtoi(argv[2]), _wtoi(argv[3]) };
        const POINT desktopPoint{ _wtoi(argv[4]), _wtoi(argv[5]) };
        HWND overlay = FindWindowW(L"DeskTodoNext_Overlay", nullptr);
        if (!overlay) return 10;
        DWORD overlayProcessId = 0;
        GetWindowThreadProcessId(overlay, &overlayProcessId);

        SetPhysicalCursorPosition(itemPoint);
        PostMessageW(overlay, WM_MOUSEMOVE, 0, 0);
        SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        SendMessageW(overlay, WM_LBUTTONUP, 0, 0);
        PostMessageW(overlay, WM_MOUSEMOVE, 0, 0);
        Sleep(100);

        SetPhysicalCursorPosition(desktopPoint);
        SendLeftButton(MOUSEEVENTF_LEFTDOWN);
        SendLeftButton(MOUSEEVENTF_LEFTUP);
        Sleep(800);

        const bool staleTooltipVisible =
            FindVisibleTooltipForProcess(overlayProcessId) != nullptr;

        SetPhysicalCursorPosition(itemPoint);
        PostMessageW(overlay, WM_MOUSEMOVE, 0, 0);
        Sleep(700);
        const bool hoverTooltipShown =
            FindVisibleTooltipForProcess(overlayProcessId) != nullptr;

        SetPhysicalCursorPosition(desktopPoint);
        SendLeftButton(MOUSEEVENTF_LEFTDOWN);
        SendLeftButton(MOUSEEVENTF_LEFTUP);
        Sleep(250);
        const bool visibleTooltipDismissed =
            FindVisibleTooltipForProcess(overlayProcessId) == nullptr;

        std::wcout << L"stale-tooltip-after-desktop-click="
                   << (staleTooltipVisible ? 1 : 0)
                   << L" hover-tooltip-shown=" << (hoverTooltipShown ? 1 : 0)
                   << L" visible-tooltip-dismissed="
                   << (visibleTooltipDismissed ? 1 : 0) << L"\n";
        return !staleTooltipVisible && hoverTooltipShown &&
            visibleTooltipDismissed ? 0 : 12;
    }

    if (argc == 2 && wcscmp(argv[1], L"--show-overlay") == 0) {
        HWND overlay = FindWindowW(L"DeskTodoNext_Overlay", nullptr);
        if (!overlay) return 10;
        ShowWindow(overlay, SW_SHOWNOACTIVATE);
        RECT rect{};
        GetWindowRect(overlay, &rect);
        std::wcout << L"overlay shown " << rect.left << L"," << rect.top
                   << L"," << rect.right << L"," << rect.bottom << L"\n";
        return 0;
    }

    if (argc == 2 && wcscmp(argv[1], L"--capture-icons") == 0) {
        HWND capture = FindWindowW(L"DeskTodoCaptureOverlay", nullptr);
        if (!capture) return 9;
        SendWindowMouse(capture, WM_LBUTTONDOWN, 600, 200, true);
        SendWindowMouse(capture, WM_MOUSEMOVE, 1600, 700, true);
        SendWindowMouse(capture, WM_LBUTTONUP, 1600, 700, false);

        ClickWindow(capture, 1020, 734);  // Pen icon.
        ClickWindow(capture, 789, 789);   // Thickest pen preset.
        SendWindowMouse(capture, WM_LBUTTONDOWN, 760, 350, true);
        SendWindowMouse(capture, WM_MOUSEMOVE, 1120, 470, true);
        SendWindowMouse(capture, WM_LBUTTONUP, 1120, 470, false);

        ClickWindow(capture, 1076, 734);  // Arrow icon.
        SendWindowMouse(capture, WM_LBUTTONDOWN, 900, 300, true);
        SendWindowMouse(capture, WM_MOUSEMOVE, 1250, 430, true);
        SendWindowMouse(capture, WM_LBUTTONUP, 1250, 430, false);

        ClickWindow(capture, 1188, 734);  // Rectangle icon and its click hint.
        SendWindowMouse(capture, WM_LBUTTONDOWN, 850, 260, true);
        SendWindowMouse(capture, WM_MOUSEMOVE, 1300, 560, true);
        SendWindowMouse(capture, WM_LBUTTONUP, 1300, 560, false);
        Sleep(150);
        std::wcout << L"capture icon flow: PASS\n";
        return 0;
    }

    if (argc == 2 && wcscmp(argv[1], L"--capture-hover") == 0) {
        HWND capture = FindWindowW(L"DeskTodoCaptureOverlay", nullptr);
        if (!capture) return 9;
        SendWindowMouse(capture, WM_LBUTTONDOWN, 600, 200, true);
        SendWindowMouse(capture, WM_MOUSEMOVE, 1600, 700, true);
        SendWindowMouse(capture, WM_LBUTTONUP, 1600, 700, false);
        // Keep the real pointer over the pen icon as well; otherwise a normal
        // mouse move can immediately replace the synthetic hover message.
        SetPhysicalCursorPosition({ -1540, 734 });
        Sleep(300);
        std::wcout << L"capture hover hint: PASS\n";
        return 0;
    }

    if (argc == 4 && wcscmp(argv[1], L"--shortcut") == 0) {
        if (FAILED(OleInitialize(nullptr))) return 3;
        std::wstring created;
        const bool ok = ShellIntegrationService::CreateItemShortcut(
            argv[2], argv[3], &created);
        OleUninitialize();
        if (ok) std::wcout << created << L"\n";
        return ok ? 0 : 8;
    }

    if (argc == 5 && wcscmp(argv[1], L"--wheel") == 0) {
        SetPhysicalCursorPosition({ _wtoi(argv[2]), _wtoi(argv[3]) });
        SendMouseWheel(_wtoi(argv[4]));
        Sleep(500);
        return 0;
    }

    if (argc == 4 && wcscmp(argv[1], L"--desktop-hit") == 0) {
        if (FAILED(OleInitialize(nullptr))) return 3;
        const POINT point{ _wtoi(argv[2]), _wtoi(argv[3]) };
        const bool hit = DesktopShellService::IsPointOnDesktop(point);
        HWND window = WindowFromPhysicalPoint(point);
        wchar_t className[128]{};
        if (window) GetClassNameW(window, className, 128);
        OleUninitialize();
        std::wcout << L"desktop-hit=" << (hit ? 1 : 0)
                   << L" point=" << point.x << L"," << point.y
                   << L" window-class=" << className << L"\n";
        return hit ? 0 : 7;
    }

    if (argc == 5 && wcscmp(argv[1], L"--text") == 0) {
        SetPhysicalCursorPosition({ _wtoi(argv[2]), _wtoi(argv[3]) });
        SendLeftButton(MOUSEEVENTF_LEFTDOWN);
        SendLeftButton(MOUSEEVENTF_LEFTUP);
        Sleep(150);
        SendUnicodeText(argv[4]);
        Sleep(500);
        return 0;
    }

    if (argc == 4 && (wcscmp(argv[1], L"--click") == 0 ||
                      wcscmp(argv[1], L"--right-click") == 0 ||
                      wcscmp(argv[1], L"--classic-menu") == 0 ||
                      wcscmp(argv[1], L"--double-click") == 0)) {
        const POINT point{ _wtoi(argv[2]), _wtoi(argv[3]) };
        SetPhysicalCursorPosition(point);
        if (wcscmp(argv[1], L"--classic-menu") == 0) {
            SendLeftButton(MOUSEEVENTF_LEFTDOWN);
            SendLeftButton(MOUSEEVENTF_LEFTUP);
            Sleep(100);
            SendKey(VK_SHIFT, 0);
            SendKey(VK_F10, 0);
            SendKey(VK_F10, KEYEVENTF_KEYUP);
            SendKey(VK_SHIFT, KEYEVENTF_KEYUP);
            Sleep(3000);
            return 0;
        }
        const bool rightClick = wcscmp(argv[1], L"--right-click") == 0;
        SendLeftButton(rightClick ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
        Sleep(80);
        SendLeftButton(rightClick ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
        if (wcscmp(argv[1], L"--double-click") == 0) {
            Sleep(80);
            SendLeftButton(MOUSEEVENTF_LEFTDOWN);
            Sleep(80);
            SendLeftButton(MOUSEEVENTF_LEFTUP);
        }
        Sleep(500);
        return 0;
    }

    if (argc == 3 && wcscmp(argv[1], L"--position") == 0) {
        if (FAILED(OleInitialize(nullptr))) return 3;
        POINT position{};
        const bool found = DesktopShellService::GetPathPositionOnDesktop(argv[2], position);
        OleUninitialize();
        if (!found) return 6;
        std::wcout << position.x << L"," << position.y << L"\n";
        return 0;
    }

    if (argc != 6) {
        std::wcerr << L"usage: DeskTodoDesktopDragProbe <path|--points> <source-x> <source-y> <target-x> <target-y>\n"
                   << L"       DeskTodoDesktopDragProbe --refresh-desktop\n"
                   << L"       DeskTodoDesktopDragProbe --restore-path <path> <x> <y>\n"
                   << L"       DeskTodoDesktopDragProbe --suppress-path <path>\n"
                   << L"       DeskTodoDesktopDragProbe --verify-refresh-hidden <path>\n"
                   << L"       DeskTodoDesktopDragProbe --verify-refresh-hidden-key <path>\n"
                   << L"       DeskTodoDesktopDragProbe --verify-refresh-pixels <x> <y> <width> <height>\n"
                   << L"       DeskTodoDesktopDragProbe --verify-fence-refresh-pixels <menu-x> <menu-y> <x> <y> <width> <height>\n"
                   << L"       DeskTodoDesktopDragProbe --notify-created <path>\n"
                   << L"       DeskTodoDesktopDragProbe --position <path>\n"
                   << L"       DeskTodoDesktopDragProbe --diagnose-shell <path>\n"
                   << L"       DeskTodoDesktopDragProbe --desktop-hit <x> <y>\n"
                   << L"       DeskTodoDesktopDragProbe <--click|--right-click|--classic-menu|--double-click> <x> <y>\n"
                   << L"       DeskTodoDesktopDragProbe --cursor <x> <y>\n"
                   << L"       DeskTodoDesktopDragProbe --fence-menu-dismiss <menu-x> <menu-y> <outside-x> <outside-y> <collapse-x> <collapse-y>\n"
                   << L"       DeskTodoDesktopDragProbe --tooltip-click-away <item-x> <item-y> <desktop-x> <desktop-y>\n"
                   << L"       DeskTodoDesktopDragProbe --wheel <x> <y> <delta>\n";
        return 2;
    }

    if (FAILED(OleInitialize(nullptr))) return 3;

    if (wcscmp(argv[1], L"--points") == 0) {
        const POINT source{ _wtoi(argv[2]), _wtoi(argv[3]) };
        const POINT target{ _wtoi(argv[4]), _wtoi(argv[5]) };
        const POINT actualSource = SetPhysicalCursorPosition(source);
        SendLeftButton(MOUSEEVENTF_LEFTDOWN);
        Sleep(250);

        constexpr int steps = 40;
        for (int step = 1; step <= steps; ++step) {
            const int x = source.x + (target.x - source.x) * step / steps;
            const int y = source.y + (target.y - source.y) * step / steps;
            SetPhysicalCursorPosition({ x, y });
            Sleep(18);
        }

        Sleep(300);
        SendLeftButton(MOUSEEVENTF_LEFTUP);
        Sleep(1000);
        OleUninitialize();
        std::wcout << L"point-drag source=" << actualSource.x << L"," << actualSource.y
                   << L" target=" << target.x << L"," << target.y << L"\n";
        return 0;
    }

    const std::wstring path = argv[1];
    const POINT desiredSource{ _wtoi(argv[2]), _wtoi(argv[3]) };
    const POINT target{ _wtoi(argv[4]), _wtoi(argv[5]) };

    HWND overlay = FindWindowW(L"DeskTodoNext_Overlay", nullptr);
    if (overlay) {
        RECT overlayRect{};
        GetWindowRect(overlay, &overlayRect);
        const DPI_AWARENESS_CONTEXT awareness = GetWindowDpiAwarenessContext(overlay);
        std::wcout << L"overlay=" << overlayRect.left << L"," << overlayRect.top
                   << L"," << overlayRect.right << L"," << overlayRect.bottom
                   << L" dpi=" << GetDpiForWindow(overlay)
                   << L" per-monitor-v2="
                   << AreDpiAwarenessContextsEqual(
                          awareness, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
                   << L"\n";

        POINT physicalTarget = SetPhysicalCursorPosition(target);
        POINT overlayLogicalTarget = physicalTarget;
        PhysicalToLogicalPointForPerMonitorDPI(overlay, &overlayLogicalTarget);
        std::wcout << L"requested-target=" << target.x << L"," << target.y
                   << L" physical-target=" << physicalTarget.x << L"," << physicalTarget.y
                   << L" overlay-logical-target=" << overlayLogicalTarget.x << L","
                   << overlayLogicalTarget.y
                   << L"\n";
        const LRESULT hit = SendMessageW(
            overlay, WM_NCHITTEST, 0, MAKELPARAM(target.x, target.y));
        std::wcout << L"target-hit=" << hit << L"\n";
    }
    if (!DesktopShellService::PositionPathOnDesktop(path, desiredSource)) {
        std::wcerr << L"failed to position source desktop icon\n";
        OleUninitialize();
        return 4;
    }

    Sleep(700);
    POINT source{};
    if (!DesktopShellService::GetPathPositionOnDesktop(path, source)) {
        std::wcerr << L"failed to read source desktop icon position\n";
        OleUninitialize();
        return 5;
    }

    SetPhysicalCursorPosition(source);
    SendLeftButton(MOUSEEVENTF_LEFTDOWN);
    Sleep(250);

    constexpr int steps = 40;
    for (int step = 1; step <= steps; ++step) {
        const int x = source.x + (target.x - source.x) * step / steps;
        const int y = source.y + (target.y - source.y) * step / steps;
        SetPhysicalCursorPosition({ x, y });
        Sleep(18);
    }

    Sleep(300);
    SendLeftButton(MOUSEEVENTF_LEFTUP);
    Sleep(1000);
    OleUninitialize();

    std::wcout << L"source=" << source.x << L"," << source.y
               << L" target=" << target.x << L"," << target.y << L"\n";
    return 0;
}
