#include "SettingsWindow.h"

#include "Managers/FenceManager.h"
#include "Services/LayoutBackupDialog.h"
#include "Window/AppWindowMessages.h"
#include "Window/BlackHoleSettingsWindow.h"
#include "Window/SettingsWindowLayout.h"
#include "Window/ScreenshotTool.h"

#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int StartupId = 4101;
constexpr int BackupId = 4102;
constexpr int RestoreId = 4103;
constexpr int SearchEditId = 4104;
constexpr int SearchButtonId = 4105;
constexpr int AboutId = 4106;
constexpr int PomodoroId = 4107;
constexpr int TodoId = 4108;
constexpr int ClockFenceId = 4112;
constexpr int ScreenshotId = 4109;
constexpr int BlackHoleId = 4110;
constexpr int LanTransferId = 4111;
constexpr int ScreenshotHotkeyId = 4113;
constexpr int ScreenshotHotkeyResetId = 4114;
constexpr int NavBaseId = 4200;
constexpr int PageCount = 7;

// iOS-inspired settings palette: grouped background, white surfaces and
// system blue actions.  Keeping these in one place makes the native controls
// and the custom-painted surfaces feel like one design system.
constexpr COLORREF WindowColor = RGB(242, 242, 247);       // systemGroupedBackground
constexpr COLORREF SidebarColor = RGB(242, 242, 247);
constexpr COLORREF ContentColor = RGB(242, 242, 247);
constexpr COLORREF CardColor = RGB(255, 255, 255);
constexpr COLORREF SelectedColor = RGB(224, 236, 255);
constexpr COLORREF ButtonColor = RGB(0, 122, 255);
constexpr COLORREF AccentColor = RGB(0, 122, 255);
constexpr COLORREF AccentSoftColor = RGB(0, 105, 225);
constexpr COLORREF TextColor = RGB(28, 28, 30);
constexpr COLORREF MutedTextColor = RGB(99, 99, 102);
constexpr COLORREF DividerColor = RGB(209, 209, 214);
constexpr COLORREF OutlineColor = RGB(229, 229, 234);
constexpr COLORREF HoverColor = RGB(232, 241, 255);

HWND activeSettingsWindow = nullptr;

struct Context {
    FenceManager* manager = nullptr;
    HWND owner = nullptr;
    HWND searchEdit = nullptr;
    HWND screenshotHotkeyEdit = nullptr;
    HWND navButtons[PageCount]{};
    std::vector<HWND> pageControls[PageCount];
    int activePage = 0;
    HBRUSH contentBrush = nullptr;
    HBRUSH editBrush = nullptr;
    HFONT bodyFont = nullptr;
    HFONT headingFont = nullptr;
    struct ControlPlacement { HWND window = nullptr; RECT base{}; int page = -1; };
    std::vector<ControlPlacement> controlPlacements;
    float layoutScale = 1.0f;
    HMONITOR monitor = nullptr;
    int scrollOffset = 0;
    int scrollMax = 0;
    bool updatingScreenshotHotkey = false;
};

RECT InflateRectCopy(RECT rect, int dx, int dy) {
    rect.left += dx;
    rect.top += dy;
    rect.right -= dx;
    rect.bottom -= dy;
    return rect;
}

void SetControlFonts(Context* context);

void RegisterPlacement(Context* context, HWND control, int x, int y,
                       int width, int height, int page = -1) {
    if (context && control) {
        context->controlPlacements.push_back({ control,
            { x, y, x + width, y + height }, page });
    }
}

void RecreateFonts(Context* context) {
    if (!context) return;
    if (context->bodyFont) DeleteObject(context->bodyFont);
    if (context->headingFont) DeleteObject(context->headingFont);
    context->bodyFont = CreateFontW(-SettingsWindowLayout::Scale(15, context->layoutScale),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Text");
    context->headingFont = CreateFontW(-SettingsWindowLayout::Scale(22, context->layoutScale),
        0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Display");
}

void LayoutControls(Context* context) {
    if (!context) return;
    for (const auto& placement : context->controlPlacements) {
        const RECT& base = placement.base;
        const bool activePageControl = std::find(
            std::begin(context->pageControls[context->activePage]),
            std::end(context->pageControls[context->activePage]),
            placement.window) != std::end(context->pageControls[context->activePage]);
        const int scaledY = SettingsWindowLayout::Scale(base.top, context->layoutScale);
        const int yOffset = activePageControl ? context->scrollOffset : 0;
        SetWindowPos(placement.window, nullptr,
            SettingsWindowLayout::Scale(base.left, context->layoutScale),
            scaledY - yOffset,
            SettingsWindowLayout::Scale(base.right - base.left, context->layoutScale),
            SettingsWindowLayout::Scale(base.bottom - base.top, context->layoutScale),
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RecreateFonts(context);
    SetControlFonts(context);
}

void UpdateScrollRange(HWND window, Context* context) {
    if (!context) return;
    RECT client{};
    GetClientRect(window, &client);
    const int contentTop = SettingsWindowLayout::Scale(84, context->layoutScale);
    int maxBottom = contentTop;
    for (HWND control : context->pageControls[context->activePage]) {
        for (const auto& placement : context->controlPlacements) {
            if (placement.window != control) continue;
            maxBottom = (std::max)(maxBottom,
                SettingsWindowLayout::Scale(placement.base.bottom, context->layoutScale));
            break;
        }
    }
    const int visibleBottom = client.bottom - SettingsWindowLayout::Scale(24, context->layoutScale);
    context->scrollMax = (std::max)(0, maxBottom - visibleBottom);
    context->scrollOffset = (std::clamp)(context->scrollOffset, 0, context->scrollMax);
    SCROLLINFO si{ sizeof(si) };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = context->scrollMax;
    si.nPage = (visibleBottom > contentTop) ? static_cast<UINT>(visibleBottom - contentTop) : 0;
    si.nPos = context->scrollOffset;
    SetScrollInfo(window, SB_VERT, &si, TRUE);
    ShowScrollBar(window, SB_VERT, context->scrollMax > 0);
}

std::wstring ExecutablePath() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return length > 0 && length < std::size(path) ? std::wstring(path, length) : L"";
}

bool StartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE,
        &key) != ERROR_SUCCESS) return false;
    wchar_t value[32768]{};
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LONG result = RegQueryValueExW(key, L"DeskTodo", nullptr, &type,
        reinterpret_cast<BYTE*>(value), &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0';
}

bool SetStartup(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + ExecutablePath() + L"\"";
        result = RegSetValueExW(key, L"DeskTodo", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"DeskTodo");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

ScreenshotHotkeyConfig ReadScreenshotHotkeyControl(HWND control) {
    ScreenshotHotkeyConfig config;
    if (!control) return config;
    const WORD packed = static_cast<WORD>(SendMessageW(control, HKM_GETHOTKEY, 0, 0));
    config.virtualKey = LOBYTE(packed);
    config.flags = HIBYTE(packed);
    return config;
}

void SetScreenshotHotkeyControl(HWND control, const ScreenshotHotkeyConfig& config) {
    if (!control) return;
    const WORD packed = MAKEWORD(config.virtualKey, config.flags);
    SendMessageW(control, HKM_SETHOTKEY, packed, 0);
}

bool ApplyScreenshotHotkey(Context* context, HWND control,
                           const ScreenshotHotkeyConfig& requested) {
    const ScreenshotHotkeyConfig previous = LoadScreenshotHotkeyConfig();
    if (!context || !context->owner || !SaveScreenshotHotkeyConfig(requested)) {
        MessageBoxW(control, L"无法保存截图快捷键设置。", L"DeskTodo",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    if (SendMessageW(context->owner, WM_DESKTODO_RELOAD_SCREENSHOT_HOTKEY, 0, 0) != 0) {
        return true;
    }

    SaveScreenshotHotkeyConfig(previous);
    SendMessageW(context->owner, WM_DESKTODO_RELOAD_SCREENSHOT_HOTKEY, 0, 0);
    context->updatingScreenshotHotkey = true;
    SetScreenshotHotkeyControl(control, previous);
    context->updatingScreenshotHotkey = false;
    MessageBoxW(control, L"该快捷键已被其他程序占用，请换一个组合键。", L"DeskTodo",
                MB_OK | MB_ICONWARNING);
    return false;
}

std::wstring FindEverything() {
    const wchar_t* candidates[] = {
        L"E:\\Everything\\Everything.exe",
        L"C:\\Dpan\\Everything\\Everything.exe",
        L"C:\\Program Files\\Everything\\Everything.exe",
        L"C:\\Program Files (x86)\\Everything\\Everything.exe"
    };
    for (const wchar_t* candidate : candidates) {
        if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES) return candidate;
    }

    wchar_t path[32768]{};
    if (SearchPathW(nullptr, L"Everything.exe", nullptr,
                    static_cast<DWORD>(std::size(path)), path, nullptr) > 0) return path;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Everything.exe",
        0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        wchar_t value[32768]{};
        DWORD type = 0;
        DWORD bytes = sizeof(value);
        const LONG result = RegQueryValueExW(key, nullptr, nullptr, &type,
            reinterpret_cast<BYTE*>(value), &bytes);
        RegCloseKey(key);
        if (result == ERROR_SUCCESS && type == REG_SZ &&
            GetFileAttributesW(value) != INVALID_FILE_ATTRIBUTES) return value;
    }
    return L"";
}

void SearchEverything(HWND owner, const std::wstring& query) {
    if (query.empty()) {
        MessageBoxW(owner, L"\u8bf7\u8f93\u5165\u6587\u4ef6\u540d\u6216\u8def\u5f84\u3002", L"\u684c\u9762\u641c\u7d22",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring executable = FindEverything();
    if (!executable.empty()) {
        std::wstring command = L"Everything.exe -search \"" + query + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        bool launched = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                                       FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr,
                                       &startup, &process) == TRUE;
        if (!launched && GetLastError() == ERROR_ACCESS_DENIED) {
            launched = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                                      FALSE, 0, nullptr, nullptr, &startup, &process) == TRUE;
        }
        if (launched) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return;
        }
        const DWORD launchError = GetLastError();
        const std::wstring detail = L"Everything 启动失败，Windows 错误码：" +
            std::to_wstring(launchError) + L"\u3002\u5c06\u6539\u7528 Windows \u641c\u7d22\u3002";
        MessageBoxW(owner, detail.c_str(), L"桌面搜索", MB_OK | MB_ICONWARNING);
    }

    const std::wstring uri = L"search-ms:query=" + query;
    SHELLEXECUTEINFOW launch{ sizeof(launch) };
    launch.fMask = SEE_MASK_ASYNCOK | SEE_MASK_FLAG_NO_UI;
    launch.hwnd = owner;
    launch.lpVerb = L"open";
    launch.lpFile = uri.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
        MessageBoxW(owner, L"\u65e0\u6cd5\u542f\u52a8 Everything \u6216 Windows \u641c\u7d22\u3002",
                    L"桌面搜索", MB_OK | MB_ICONERROR);
        return;
    }
}

void ApplyDarkTheme(HWND window) {
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    HMODULE theme = LoadLibraryW(L"uxtheme.dll");
    if (theme) {
        auto setTheme = reinterpret_cast<SetWindowThemeFn>(
            GetProcAddress(theme, "SetWindowTheme"));
        if (setTheme) setTheme(window, L"Explorer", nullptr);
        FreeLibrary(theme);
    }
    if (GetAncestor(window, GA_ROOT) == window) {
        using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            auto setAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
                GetProcAddress(dwm, "DwmSetWindowAttribute"));
            if (setAttribute) {
                BOOL enabled = FALSE;
                if (FAILED(setAttribute(window, 20, &enabled, sizeof(enabled)))) {
                    setAttribute(window, 19, &enabled, sizeof(enabled));
                }
            }
            FreeLibrary(dwm);
        }
    }
}

HWND AddControl(Context* context, int page, const wchar_t* className,
                const wchar_t* text, DWORD style, int x, int y, int width,
                int height, HWND parent, int id = 0, DWORD exStyle = 0) {
    HWND control = CreateWindowExW(exStyle, className, text, WS_CHILD | style,
        x, y, width, height, parent,
        id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
        nullptr, nullptr);
    if (control) {
        context->pageControls[page].push_back(control);
        RegisterPlacement(context, control, x, y, width, height, page);
        ApplyDarkTheme(control);
    }
    return control;
}

void ShowPage(Context* context, int page) {
    if (!context || page < 0 || page >= PageCount) return;
    context->activePage = page;
    context->scrollOffset = 0;
    for (int index = 0; index < PageCount; ++index) {
        const int command = index == page ? SW_SHOW : SW_HIDE;
        for (HWND control : context->pageControls[index]) ShowWindow(control, command);
        InvalidateRect(context->navButtons[index], nullptr, TRUE);
    }
    HWND settings = GetParent(context->navButtons[0]);
    if (settings) UpdateScrollRange(settings, context);
    LayoutControls(context);
    InvalidateRect(settings, nullptr, TRUE);
    UpdateWindow(settings);
}

void SetControlFonts(Context* context) {
    for (int page = 0; page < PageCount; ++page) {
        for (size_t index = 0; index < context->pageControls[page].size(); ++index) {
            HWND control = context->pageControls[page][index];
            const bool heading = index == 0;
            SendMessageW(control, WM_SETFONT,
                reinterpret_cast<WPARAM>(heading ? context->headingFont : context->bodyFont), TRUE);
        }
        SendMessageW(context->navButtons[page], WM_SETFONT,
            reinterpret_cast<WPARAM>(context->bodyFont), TRUE);
    }
}

void DrawOwnerButton(const DRAWITEMSTRUCT* draw, const Context* context) {
    const int id = static_cast<int>(draw->CtlID);
    const bool isNav = id >= NavBaseId && id < NavBaseId + PageCount;
    const bool selected = isNav && context && id - NavBaseId == context->activePage;
    const bool hovered = (draw->itemState & ODS_HOTLIGHT) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    COLORREF background = isNav ? (selected ? SelectedColor : SidebarColor) : ButtonColor;
    if (hovered && !isNav) background = RGB(0, 112, 235);
    if (hovered && isNav && !selected) background = HoverColor;
    if (pressed && !isNav) background = AccentSoftColor;

    HBRUSH brush = CreateSolidBrush(background);
    const int radius = isNav ? 11 : 12;
    HGDIOBJ oldBrush = SelectObject(draw->hDC, brush);
    HPEN noPen = CreatePen(PS_NULL, 0, background);
    HGDIOBJ oldPen = SelectObject(draw->hDC, noPen);
    RoundRect(draw->hDC, draw->rcItem.left, draw->rcItem.top,
        draw->rcItem.right, draw->rcItem.bottom, radius, radius);
    SelectObject(draw->hDC, oldPen);
    SelectObject(draw->hDC, oldBrush);
    DeleteObject(noPen);
    DeleteObject(brush);

    wchar_t text[128]{};
    GetWindowTextW(draw->hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, !isNav ? RGB(255, 255, 255) :
        (selected ? AccentColor : TextColor));
    HFONT oldFont = static_cast<HFONT>(SelectObject(draw->hDC, context->bodyFont));
    RECT textRect = draw->rcItem;
    textRect.left += isNav ? 18 : 0;
    DrawTextW(draw->hDC, text, -1, &textRect,
        (isNav ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE);
    SelectObject(draw->hDC, oldFont);
}

void DrawCard(HDC dc, const RECT& rect, int radius) {
    HBRUSH fill = CreateSolidBrush(CardColor);
    HPEN border = CreatePen(PS_SOLID, 1, OutlineColor);
    HGDIOBJ oldBrush = SelectObject(dc, fill);
    HGDIOBJ oldPen = SelectObject(dc, border);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(fill);
    DeleteObject(border);

}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }

    switch (message) {
    case WM_CREATE: {
        context->contentBrush = CreateSolidBrush(ContentColor);
        context->editBrush = CreateSolidBrush(RGB(244, 251, 248));
        RecreateFonts(context);
        INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_HOTKEY_CLASS };
        InitCommonControlsEx(&commonControls);

        const wchar_t* navLabels[PageCount] = {
            L"基础设置", L"桌面整理", L"桌面搜索", L"组件",
            L"截图与贴图", L"局域网快传", L"关于"
        };
        for (int index = 0; index < PageCount; ++index) {
            context->navButtons[index] = CreateWindowW(L"BUTTON", navLabels[index],
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                16, 88 + index * 48, 140, 38, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(NavBaseId + index)),
                nullptr, nullptr);
            RegisterPlacement(context, context->navButtons[index],
                              16, 88 + index * 48, 140, 38);
        }

        AddControl(context, 0, L"STATIC", L"基础设置", WS_VISIBLE,
            208, 32, 470, 30, window);
        AddControl(context, 0, L"STATIC", L"\u542f\u52a8\u4e0e\u7cfb\u7edf\u884c\u4e3a", WS_VISIBLE,
            208, 72, 470, 24, window);
        HWND startup = AddControl(context, 0, L"BUTTON", L"\u5f00\u673a\u542f\u52a8\u684c\u9762\u6574\u7406",
            WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            226, 118, 360, 34, window, StartupId);
        SendMessageW(startup, BM_SETCHECK, StartupEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        AddControl(context, 1, L"STATIC", L"桌面整理", 0,
            208, 32, 470, 30, window);
        AddControl(context, 1, L"STATIC", L"保存当前布局、尺寸与文件排序", 0,
            208, 72, 470, 28, window);
        AddControl(context, 1, L"BUTTON", L"备份当前布局",
            BS_OWNERDRAW | WS_TABSTOP, 226, 118, 160, 42, window, BackupId);
        AddControl(context, 1, L"BUTTON", L"还原最近布局",
            BS_OWNERDRAW | WS_TABSTOP, 402, 118, 160, 42, window, RestoreId);

        AddControl(context, 2, L"STATIC", L"桌面搜索", 0,
            208, 32, 470, 30, window);
        AddControl(context, 2, L"STATIC", L"优先使用 Everything，未安装时使用 Windows 搜索", 0,
            208, 72, 470, 28, window);
        context->searchEdit = AddControl(context, 2, L"EDIT", L"",
            WS_TABSTOP | ES_AUTOHSCROLL, 226, 118, 350, 38, window,
            SearchEditId, WS_EX_CLIENTEDGE);
        SendMessageW(context->searchEdit, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"输入文件名或路径"));
        AddControl(context, 2, L"BUTTON", L"搜索",
            BS_OWNERDRAW | WS_TABSTOP, 590, 118, 100, 38, window, SearchButtonId);

        AddControl(context, 3, L"STATIC", L"组件", 0,
            208, 32, 470, 30, window);
        AddControl(context, 3, L"STATIC", L"把常用工具添加到桌面，重复添加不会创建副本", 0,
            208, 72, 470, 28, window);
        AddControl(context, 3, L"STATIC", L"番茄时钟", 0,
            226, 108, 180, 28, window);
        AddControl(context, 3, L"STATIC", L"25 分钟专注 / 5 分钟休息，支持暂停与重置", 0,
            226, 136, 310, 30, window);
        AddControl(context, 3, L"BUTTON", L"添加 / 显示",
            BS_OWNERDRAW | WS_TABSTOP, 540, 112, 150, 38, window, PomodoroId);
        AddControl(context, 3, L"STATIC", L"Todo List", 0,
            226, 220, 180, 28, window);
        AddControl(context, 3, L"STATIC", L"支持优先级、截止日期与排序", 0,
            226, 248, 310, 30, window);
        AddControl(context, 3, L"BUTTON", L"添加 / 显示",
            BS_OWNERDRAW | WS_TABSTOP, 540, 224, 150, 38, window, TodoId);
        AddControl(context, 3, L"STATIC", L"\u65f6\u95f4\u7ec4\u4ef6", 0,
            226, 332, 220, 28, window);
        AddControl(context, 3, L"STATIC",
            L"显示当前时间、秒数和日期，可移动缩放", 0,
            226, 360, 310, 30, window);
        AddControl(context, 3, L"BUTTON", L"\u6dfb\u52a0 / \u663e\u793a",
            BS_OWNERDRAW | WS_TABSTOP, 540, 336, 150, 38, window, ClockFenceId);
        AddControl(context, 3, L"STATIC", L"\u9ed1\u6d1e\u52a8\u6001\u80cc\u666f", 0,
            226, 444, 220, 28, window);
        AddControl(context, 3, L"STATIC", L"引力透镜、吸积盘与 HDR Bloom 背景", 0,
            226, 472, 310, 30, window);
        AddControl(context, 3, L"BUTTON", L"\u5f00\u542f / \u5173\u95ed",
            BS_OWNERDRAW | WS_TABSTOP, 540, 448, 150, 38, window, BlackHoleId);

        AddControl(context, 4, L"STATIC", L"\u622a\u56fe\u4e0e\u8d34\u56fe", 0,
            208, 32, 470, 30, window);
        AddControl(context, 4, L"STATIC", L"框选后可复制、保存 PNG、贴图到桌面，支持 Ctrl+Shift+A", 0,
            208, 78, 470, 36, window);
        AddControl(context, 4, L"STATIC", L"\u622a\u56fe\u5feb\u6377\u952e", 0,
            226, 150, 180, 32, window);
        context->screenshotHotkeyEdit = AddControl(context, 4, HOTKEY_CLASSW, L"",
            WS_TABSTOP, 456, 146, 234, 38, window, ScreenshotHotkeyId,
            WS_EX_CLIENTEDGE);
        context->updatingScreenshotHotkey = true;
        SetScreenshotHotkeyControl(context->screenshotHotkeyEdit,
                                   LoadScreenshotHotkeyConfig());
        context->updatingScreenshotHotkey = false;
        AddControl(context, 4, L"STATIC", L"\u6309\u4e0b\u7ec4\u5408\u952e\u540e\u7acb\u5373\u751f\u6548", 0,
            226, 190, 360, 28, window);
        AddControl(context, 4, L"BUTTON", L"\u5f00\u59cb\u622a\u56fe",
            BS_OWNERDRAW | WS_TABSTOP, 226, 232, 160, 42, window, ScreenshotId);
        AddControl(context, 4, L"BUTTON", L"\u6062\u590d\u9ed8\u8ba4",
            BS_OWNERDRAW | WS_TABSTOP, 402, 232, 160, 42, window, ScreenshotHotkeyResetId);

        AddControl(context, 5, L"STATIC", L"\u5c40\u57df\u7f51\u5feb\u4f20", 0,
            208, 32, 470, 30, window);
        AddControl(context, 5, L"STATIC", L"手机与电脑连接同一 Wi-Fi，打开后按提示传输文件", 0,
            208, 78, 470, 36, window);
        AddControl(context, 5, L"BUTTON", L"\u6253\u5f00\u5feb\u4f20",
            BS_OWNERDRAW | WS_TABSTOP, 226, 150, 160, 42, window, LanTransferId);

        AddControl(context, 6, L"STATIC", L"关于", 0,
            208, 32, 470, 30, window);
        AddControl(context, 6, L"STATIC", L"DeskTodo.Next 1.7", 0,
            226, 88, 420, 28, window);
        AddControl(context, 6, L"STATIC", L"原生 Windows 桌面整理、文件夹映射与桌面组件。", 0,
            226, 126, 450, 32, window);
        AddControl(context, 6, L"BUTTON", L"查看版本信息",
            BS_OWNERDRAW | WS_TABSTOP, 226, 178, 160, 42, window, AboutId);

        LayoutControls(context);
        ShowPage(context, 0);
        ApplyDarkTheme(window);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH content = CreateSolidBrush(WindowColor);
        HBRUSH sidebar = CreateSolidBrush(SidebarColor);
        FillRect(dc, &client, content);
        const float scale = context ? context->layoutScale : 1.0f;
        const int sidebarWidth = SettingsWindowLayout::Scale(180, scale);
        RECT sidebarRect{ 0, 0, sidebarWidth, client.bottom };
        FillRect(dc, &sidebarRect, sidebar);
        RECT contentRect{ sidebarWidth, 0, client.right, client.bottom };
        HBRUSH contentPanel = CreateSolidBrush(ContentColor);
        FillRect(dc, &contentRect, contentPanel);
        DeleteObject(content);
        DeleteObject(sidebar);
        DeleteObject(contentPanel);

        RECT headerStrip{ 0, 0, sidebarWidth, SettingsWindowLayout::Scale(64, scale) };
        HBRUSH headerBrush = CreateSolidBrush(CardColor);
        FillRect(dc, &headerStrip, headerBrush);
        DeleteObject(headerBrush);

        if (context) {
            RECT brandArea{ SettingsWindowLayout::Scale(18, scale), SettingsWindowLayout::Scale(16, scale),
                            sidebarWidth - SettingsWindowLayout::Scale(14, scale), SettingsWindowLayout::Scale(56, scale) };
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, TextColor);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dc, context->headingFont));
            DrawTextW(dc, L"DeskTodo", -1, &brandArea, DT_LEFT | DT_TOP | DT_SINGLELINE);
            RECT brandSubtitle = { SettingsWindowLayout::Scale(18, scale), SettingsWindowLayout::Scale(38, scale),
                                   sidebarWidth - SettingsWindowLayout::Scale(14, scale), SettingsWindowLayout::Scale(58, scale) };
            SetTextColor(dc, MutedTextColor);
            DrawTextW(dc, L"控制台", -1, &brandSubtitle, DT_LEFT | DT_TOP | DT_SINGLELINE);
            SelectObject(dc, oldFont);
        }

        if (context) {
            const int left = sidebarWidth + SettingsWindowLayout::Scale(20, scale);
            const int right = client.right - SettingsWindowLayout::Scale(20, scale);
            const int top = SettingsWindowLayout::Scale(84, scale);
            const int gap = SettingsWindowLayout::Scale(16, scale);

            const int scroll = context->scrollOffset;
            const int radius = SettingsWindowLayout::Scale(12, scale);
            auto card = [&](int x, int y, int w, int h) {
                RECT r{ x, y - scroll, x + w, y + h - scroll };
                if (r.bottom > top && r.top < client.bottom) DrawCard(dc, r, radius);
            };

            switch (context->activePage) {
            case 0:
                card(left, top, right - left, SettingsWindowLayout::Scale(96, scale));
                break;
            case 1:
                card(left, top, right - left, SettingsWindowLayout::Scale(104, scale));
                break;
            case 2:
                card(left, top, right - left, SettingsWindowLayout::Scale(104, scale));
                break;
            case 3: {
                const int row = SettingsWindowLayout::Scale(104, scale);
                const int step = SettingsWindowLayout::Scale(112, scale);
                card(left, top, right - left, row);
                card(left, top + step, right - left, row);
                card(left, top + step * 2, right - left, row);
                card(left, top + step * 3, right - left, row);
                break;
            }
            case 4:
                card(left, top, right - left, SettingsWindowLayout::Scale(210, scale));
                break;
            case 5:
                card(left, top, right - left, SettingsWindowLayout::Scale(122, scale));
                break;
            case 6:
                card(left, top, right - left, SettingsWindowLayout::Scale(148, scale));
                break;
            default:
                break;
            }
        }

        HPEN divider = CreatePen(PS_SOLID, 1, DividerColor);
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, divider));
        MoveToEx(dc, sidebarWidth, 0, nullptr); LineTo(dc, sidebarWidth, client.bottom);
        MoveToEx(dc, SettingsWindowLayout::Scale(216, scale),
                 SettingsWindowLayout::Scale(68, scale), nullptr);
        LineTo(dc, client.right - SettingsWindowLayout::Scale(30, scale),
               SettingsWindowLayout::Scale(68, scale));
        SelectObject(dc, oldPen);
        DeleteObject(divider);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DRAWITEM:
        DrawOwnerButton(reinterpret_cast<DRAWITEMSTRUCT*>(lp), context);
        return TRUE;
    case WM_COMMAND:
        if (!context) break;
        if (LOWORD(wp) >= NavBaseId && LOWORD(wp) < NavBaseId + PageCount) {
            ShowPage(context, LOWORD(wp) - NavBaseId);
            return 0;
        }
        switch (LOWORD(wp)) {
        case StartupId: {
            const bool enabled = SendDlgItemMessageW(
                window, StartupId, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!SetStartup(enabled)) {
                MessageBoxW(window, L"\u65e0\u6cd5\u66f4\u65b0\u5f00\u673a\u542f\u52a8\u8bbe\u7f6e\u3002", L"DeskTodo",
                            MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case BackupId: {
            const std::wstring path = LayoutBackupDialog::SelectSavePath(window);
            if (path.empty()) return 0;
            const bool ok = context->manager && context->manager->BackupLayout(path);
            MessageBoxW(window, ok ? L"\u5f53\u524d\u5e03\u5c40\u5df2\u5907\u4efd\u3002" : L"\u5e03\u5c40\u5907\u4efd\u5931\u8d25\u3002", L"DeskTodo",
                        MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
            return 0;
        }
        case RestoreId:
            {
            const std::wstring path = LayoutBackupDialog::SelectOpenPath(window);
            if (path.empty()) return 0;
            if (MessageBoxW(window, L"确定使用最近备份覆盖当前布局吗？", L"还原布局",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                const bool ok = context->manager && context->manager->RestoreLayout(path);
                MessageBoxW(window, ok ? L"\u5e03\u5c40\u5df2\u8fd8\u539f\u3002" : L"\u6ca1\u6709\u53ef\u7528\u7684\u5e03\u5c40\u5907\u4efd\u3002", L"DeskTodo",
                            MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
                InvalidateRect(context->owner, nullptr, FALSE);
            }
            return 0;
            }
        case SearchButtonId: {
            wchar_t query[2048]{};
            const int queryLength = GetDlgItemTextW(
                window, SearchEditId, query, static_cast<int>(std::size(query)));
            if (queryLength <= 0) {
                MessageBoxW(window, L"\u8bf7\u8f93\u5165\u6587\u4ef6\u540d\u6216\u8def\u5f84\u3002", L"\u684c\u9762\u641c\u7d22",
                            MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            const std::wstring searchQuery(query);
            std::thread([window, searchQuery]() {
                SearchEverything(window, searchQuery);
            }).detach();
            return 0;
        }
        case PomodoroId:
            PostMessageW(context->owner, WM_DESKTODO_SHOW_POMODORO, 0, 0);
            DestroyWindow(window);
            return 0;
        case TodoId:
            PostMessageW(context->owner, WM_DESKTODO_SHOW_TODO, 0, 0);
            DestroyWindow(window);
            return 0;
        case ClockFenceId:
            PostMessageW(context->owner, WM_DESKTODO_NEW_CLOCK_FENCE, 0, 0);
            DestroyWindow(window);
            return 0;
        case BlackHoleId:
            BlackHoleSettingsWindow::Show(context->owner);
            DestroyWindow(window);
            return 0;
        case ScreenshotHotkeyId:
            if (HIWORD(wp) == EN_CHANGE && !context->updatingScreenshotHotkey) {
                ApplyScreenshotHotkey(context, context->screenshotHotkeyEdit,
                                     ReadScreenshotHotkeyControl(context->screenshotHotkeyEdit));
            }
            return 0;
        case ScreenshotHotkeyResetId: {
            const ScreenshotHotkeyConfig defaults;
            if (ApplyScreenshotHotkey(context, context->screenshotHotkeyEdit, defaults)) {
                context->updatingScreenshotHotkey = true;
                SetScreenshotHotkeyControl(context->screenshotHotkeyEdit, defaults);
                context->updatingScreenshotHotkey = false;
            }
            return 0;
        }
        case ScreenshotId:
            PostMessageW(context->owner, WM_DESKTODO_START_CAPTURE, 0, 0);
            DestroyWindow(window);
            return 0;
        case LanTransferId:
            PostMessageW(context->owner, WM_DESKTODO_SHOW_LAN_TRANSFER, 0, 0);
            DestroyWindow(window);
            return 0;
        case AboutId:
            MessageBoxW(window,
                L"DeskTodo.Next 1.7\n\n\u539f\u751f Windows \u684c\u9762\u6574\u7406\u3001\u591a\u5c4f\u7a33\u5b9a\u5e03\u5c40\u3001\u6e38\u620f\u5bb9\u5668\u3001\u5f85\u529e\u4e8b\u9879\u4e0e\u684c\u9762\u7ec4\u4ef6\u3002",
                L"关于 DeskTodo", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wp), TextColor);
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        SetBkColor(reinterpret_cast<HDC>(wp), CardColor);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), TextColor);
        SetBkColor(reinterpret_cast<HDC>(wp), CardColor);
        return reinterpret_cast<LRESULT>(context ? context->editBrush : nullptr);
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH content = CreateSolidBrush(WindowColor);
        HBRUSH sidebar = CreateSolidBrush(SidebarColor);
        FillRect(dc, &client, content);
        const int sidebarWidth = SettingsWindowLayout::Scale(180,
            context ? context->layoutScale : 1.0f);
        RECT sidebarRect{ 0, 0, sidebarWidth, client.bottom };
        FillRect(dc, &sidebarRect, sidebar);
        RECT contentRect{ sidebarWidth, 0, client.right, client.bottom };
        HBRUSH contentPanel = CreateSolidBrush(ContentColor);
        FillRect(dc, &contentRect, contentPanel);
        DeleteObject(content);
        DeleteObject(sidebar);
        DeleteObject(contentPanel);
        return 1;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DPICHANGED:
    case WM_EXITSIZEMOVE: {
        if (!context) break;
        RECT current{};
        GetWindowRect(window, &current);
        HMONITOR monitor = MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(info) };
        if (!monitor || !GetMonitorInfoW(monitor, &info)) break;
        POINT center{ (current.left + current.right) / 2,
                      (current.top + current.bottom) / 2 };
        const auto placement = SettingsWindowLayout::FitToWorkArea(info.rcWork, center);
        context->monitor = monitor;
        context->layoutScale = placement.scale;
        SetWindowPos(window, nullptr, placement.rect.left, placement.rect.top,
            placement.rect.right - placement.rect.left,
            placement.rect.bottom - placement.rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutControls(context);
        UpdateScrollRange(window, context);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_VSCROLL: {
        if (!context) break;
        int action = LOWORD(wp);
        int next = context->scrollOffset;
        const int pageStep = SettingsWindowLayout::Scale(52, context->layoutScale);
        switch (action) {
        case SB_LINEUP: next -= SettingsWindowLayout::Scale(28, context->layoutScale); break;
        case SB_LINEDOWN: next += SettingsWindowLayout::Scale(28, context->layoutScale); break;
        case SB_PAGEUP: next -= pageStep; break;
        case SB_PAGEDOWN: next += pageStep; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{ sizeof(si) };
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(window, SB_VERT, &si);
            next = si.nTrackPos;
            break;
        }
        default:
            return 0;
        }
        next = (std::clamp)(next, 0, context->scrollMax);
        if (next != context->scrollOffset) {
            context->scrollOffset = next;
            LayoutControls(context);
            UpdateScrollRange(window, context);
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!context) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (delta == 0 || context->scrollMax <= 0) return 0;
        context->scrollOffset = (std::clamp)(
            context->scrollOffset - (delta / WHEEL_DELTA) * SettingsWindowLayout::Scale(36, context->layoutScale),
            0, context->scrollMax);
        LayoutControls(context);
        UpdateScrollRange(window, context);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_NCDESTROY:
        activeSettingsWindow = nullptr;
        if (context) {
            if (context->contentBrush) DeleteObject(context->contentBrush);
            if (context->editBrush) DeleteObject(context->editBrush);
            if (context->bodyFont) DeleteObject(context->bodyFont);
            if (context->headingFont) DeleteObject(context->headingFont);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

}

namespace SettingsWindow {

void Show(HWND owner, FenceManager* manager) {
    if (activeSettingsWindow && IsWindow(activeSettingsWindow)) {
        ShowWindow(activeSettingsWindow, SW_RESTORE);
        SetForegroundWindow(activeSettingsWindow);
        return;
    }

    static bool registered = false;
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSEXW cls{ sizeof(cls) };
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        cls.lpszClassName = L"DeskTodoSettingsWindow";
        registered = RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return;

    POINT cursor{};
    GetPhysicalCursorPos(&cursor);
    HMONITOR targetMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    GetMonitorInfoW(targetMonitor, &info);
    const auto placement = SettingsWindowLayout::FitToWorkArea(info.rcWork, cursor);
    const int width = placement.rect.right - placement.rect.left;
    const int height = placement.rect.bottom - placement.rect.top;
    const int x = placement.rect.left;
    const int y = placement.rect.top;
    Context context{ manager, owner };
    context.layoutScale = placement.scale;
    context.monitor = targetMonitor;
    HWND settings = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"DeskTodoSettingsWindow", L"桌面整理 - 设置中心",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height,
        owner, nullptr, instance, &context);
    if (!settings) return;
    activeSettingsWindow = settings;

    EnableWindow(owner, FALSE);
    ShowWindow(settings, SW_SHOW);
    UpdateWindow(settings);
    MSG message{};
    bool quitRequested = false;
    while (IsWindow(settings)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            quitRequested = result == 0;
            break;
        }
        if (!IsDialogMessageW(settings, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    if (quitRequested) {
        PostQuitMessage(static_cast<int>(message.wParam));
    } else {
        SetForegroundWindow(owner);
    }
}

}
