#include "BlackHoleSettingsWindow.h"

#include "Window/AppWindowMessages.h"
#include "Window/SettingsWindowLayout.h"

#include <array>
#include <commctrl.h>
#include <cwchar>
#include <string>

namespace BlackHoleSettingsWindow {
namespace {

constexpr wchar_t ClassName[] = L"DeskTodo.BlackHoleSettings";
constexpr wchar_t RegistryPath[] = L"Software\\DeskTodo\\BlackHole";
constexpr int PresetId = 7000;
constexpr int SliderBase = 7100;
constexpr int ApplyId = 7200;
constexpr int CancelId = 7201;
constexpr int ModeId = 7202;
constexpr int IdleSecondsId = 7203;
constexpr int SaveOnlyId = 7204;
constexpr int DesignWidth = 920;
constexpr int DesignHeight = 640;
constexpr COLORREF GroupedBackground = RGB(242, 242, 247);
constexpr COLORREF Surface = RGB(255, 255, 255);
constexpr COLORREF TextColor = RGB(28, 28, 30);

struct Preset { const wchar_t* name; std::array<float, 14> v; };
constexpr Preset Presets[] = {
    {L"Inferno", {5500,1.50f,.35f,1.8f,8,.90f,.60f,2.5f,2.2f,1.6f,7,5,1.40f,0}},
    {L"Gargantua", {4500,1.52f,.10f,2.2f,7,.85f,.35f,2,1.4f,.5f,7,5,1.20f,0}},
    {L"M87* Donut", {3800,.55f,-.30f,2.2f,6,.45f,.90f,3.5f,1.6f,.4f,3,2.5f,1.10f,0}},
    {L"Face-on Ember", {6500,.30f,0,3,10,.50f,.80f,2.5f,1,1.1f,7,5,1,0}},
    {L"Quasar", {15000,1.30f,.35f,3,14,.35f,1,4,1.2f,1.3f,8,5,.80f,0}},
    {L"Blazar", {18000,1.05f,.55f,3,16,.30f,1,5,1,1.5f,9,6,.75f,0}},
    {L"Pure Lens", {5500,1.50f,.35f,1.8f,8,0,1,2.5f,0,1.6f,7,5,1,.6f}},
    {L"Inferno 2", {5500,1.50f,.35f,1.8f,8,.90f,.60f,2.5f,2.2f,1.6f,7,5,1.40f,0}},
    {L"Crimson Vortex", {3200,1.45f,.60f,2,9,.95f,.20f,1.5f,3,2,5,4,1.60f,0}},
    {L"Azure Spiral", {8000,1.20f,-.50f,2.5f,7,.70f,.75f,2.8f,1.8f,1.4f,8,5.5f,1.30f,0}},
    {L"Ruby Ring", {2500,1.55f,.20f,1.6f,6,.60f,.10f,1.2f,2.6f,1.8f,4,3,1.50f,0}},
    {L"Ghost Halo", {12000,.80f,.45f,2.8f,12,.40f,.95f,3.5f,1.5f,1.2f,8.5f,5,.90f,0}},
    {L"Top-down Galaxy", {5000,.10f,0,2,9,.85f,.50f,2,1.3f,1.5f,6,4.5f,1.10f,0}},
    {L"White Dwarf Beam", {22000,1.40f,.70f,3.5f,18,.25f,1,4.5f,.9f,1.7f,10,6.5f,.70f,0}},
    {L"Solar Forge", {4200,1.48f,.15f,1.9f,7.5f,.80f,.45f,2.2f,2,.8f,6.5f,4.8f,1.25f,0}},
    {L"Obsidian Eye", {9000,.45f,-.15f,2.6f,11,.55f,.85f,3,1.1f,1.3f,7.5f,5.2f,1.05f,0}},
};

struct Spec { const wchar_t* label; float min, max; };
constexpr Spec Specs[] = {
    {L"色温 (K)",1000,30000},{L"盘面倾角",0,3},{L"盘面旋转",-1,1},
    {L"内半径",.5f,10},{L"外半径",1,30},{L"不透明度",0,1},
    {L"多普勒",0,1.5f},{L"光束指数",.5f,10},{L"亮度增益",0,5},
    {L"条纹对比度",.1f,5},{L"缠绕紧度",1,20},{L"旋转速度",.5f,15},
    {L"曝光度",.1f,5},{L"星空亮度",0,2}
};

const wchar_t* ValueNames[] = {L"temperature",L"inclination",L"roll",L"inner",L"outer",L"opacity",L"doppler",L"beam",L"gain",L"contrast",L"winding",L"speed",L"exposure",L"stars"};

std::array<float,14> ToArray(const BlackHole::Settings& s) {
    return {s.diskTemperature,s.diskInclination,s.diskRoll,s.diskInnerRadius,
        s.diskOuterRadius,s.diskOpacity,s.dopplerMix,s.beamExponent,s.diskGain,
        s.streakContrast,s.winding,s.diskRotationSpeed,s.diskExposure,s.starBrightness};
}

void FromArray(BlackHole::Settings& s, const std::array<float,14>& v) {
    s.diskTemperature=v[0]; s.diskInclination=v[1]; s.diskRoll=v[2];
    s.diskInnerRadius=v[3]; s.diskOuterRadius=v[4]; s.diskOpacity=v[5];
    s.dopplerMix=v[6]; s.beamExponent=v[7]; s.diskGain=v[8];
    s.streakContrast=v[9]; s.winding=v[10]; s.diskRotationSpeed=v[11];
    s.diskExposure=v[12]; s.starBrightness=v[13];
}

void Save(const BlackHole::Settings& settings, int mode, int idleSeconds) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RegistryPath, 0, nullptr, 0, KEY_SET_VALUE,
        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const auto values = ToArray(settings);
    for (size_t i = 0; i < values.size(); ++i) {
        RegSetValueExW(key, ValueNames[i], 0, REG_BINARY,
            reinterpret_cast<const BYTE*>(&values[i]), sizeof(float));
    }
    DWORD modeValue = static_cast<DWORD>((std::clamp)(mode, 0, 1));
    DWORD idleValue = static_cast<DWORD>((std::clamp)(idleSeconds, 10, 86400));
    RegSetValueExW(key, L"displayMode", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&modeValue), sizeof(modeValue));
    RegSetValueExW(key, L"idleSeconds", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&idleValue), sizeof(idleValue));
    RegCloseKey(key);

    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length > 0 && length < std::size(executable)) {
        wchar_t* slash = wcsrchr(executable, L'\\');
        if (slash) {
            wcscpy_s(slash + 1, std::size(executable) - (slash + 1 - executable), L"blackhole_presets.txt");
            FILE* file = nullptr;
            if (_wfopen_s(&file, executable, L"w, ccs=UTF-8") == 0 && file) {
                // DXGI desktop duplication avoids recursively capturing the effect window.
                fwprintf(file, L"# Blackhole Presets v4\n%d %d 5.250 1 0 0 1 1.000 1 2\n1\nDeskTodo Custom\n",
                    static_cast<int>(modeValue), static_cast<int>(idleValue));
                fwprintf(file, L"%.0f %.2f %.2f %.1f %.1f %.2f %.2f %.1f %.2f %.2f %.1f %.1f %.2f %.3f\n",
                    settings.diskTemperature, settings.diskInclination, settings.diskRoll,
                    settings.diskInnerRadius, settings.diskOuterRadius, settings.diskOpacity,
                    settings.dopplerMix, settings.beamExponent, settings.diskGain,
                    settings.streakContrast, settings.winding, settings.diskRotationSpeed,
                    settings.diskExposure, settings.starBrightness);
                fclose(file);
            }
        }
    }
}

struct Context {
    HWND owner{};
    BlackHole::Settings settings{};
    HWND sliders[14]{};
    HWND values[14]{};
    HWND modeCombo{};
    HWND idleEdit{};
    int mode = 0;
    int idleSeconds = 300;
    float layoutScale = 1.0f;
    HMONITOR monitor{};
    HBRUSH backgroundBrush{};
    HBRUSH fieldBrush{};
    HFONT bodyFont{};
};

int Position(float value, const Spec& s) {
    return static_cast<int>((value - s.min) / (s.max - s.min) * 1000 + 0.5f);
}

float SliderValue(HWND h, const Spec& s) {
    return s.min + (s.max - s.min) * SendMessageW(h, TBM_GETPOS, 0, 0) / 1000.0f;
}

int S(Context* c, int value) {
    return SettingsWindowLayout::Scale(value, c ? c->layoutScale : 1.0f);
}

void Refresh(Context* c) {
    auto v = ToArray(c->settings);
    for (int i = 0; i < 14; ++i) {
        SendMessageW(c->sliders[i], TBM_SETPOS, TRUE, Position(v[i], Specs[i]));
        wchar_t text[32]{};
        if (i == 0) swprintf_s(text, L"%.0f", v[i]);
        else swprintf_s(text, L"%.2f", v[i]);
        SetWindowTextW(c->values[i], text);
    }
}

void ReadSliders(Context* c) {
    auto v = ToArray(c->settings);
    for (int i = 0; i < 14; ++i) v[i] = SliderValue(c->sliders[i], Specs[i]);
    FromArray(c->settings, v);
    c->settings = BlackHole::Settings::Sanitize(c->settings);
}

void SaveFromControls(Context* c) {
    ReadSliders(c);
    c->mode = static_cast<int>(SendMessageW(c->modeCombo, CB_GETCURSEL, 0, 0));
    wchar_t idleText[16]{};
    GetWindowTextW(c->idleEdit, idleText, static_cast<int>(std::size(idleText)));
    c->idleSeconds = (std::clamp)(_wtoi(idleText), 10, 86400);
    const std::wstring normalizedIdle = std::to_wstring(c->idleSeconds);
    SetWindowTextW(c->idleEdit, normalizedIdle.c_str());
    Save(c->settings, c->mode, c->idleSeconds);
}

LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    auto* c = reinterpret_cast<Context*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        c = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
    }
    switch (m) {
    case WM_CREATE: {
        c->backgroundBrush = CreateSolidBrush(GroupedBackground);
        c->fieldBrush = CreateSolidBrush(Surface);
        c->bodyFont = CreateFontW(-S(c, 16), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
        CreateWindowW(L"STATIC", L"黑洞动态背景设置", WS_CHILD | WS_VISIBLE,
            S(c, 24), S(c, 18), S(c, 300), S(c, 28), w, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"显示模式", WS_CHILD | WS_VISIBLE,
            S(c, 245), S(c, 18), S(c, 70), S(c, 24), w, nullptr, nullptr, nullptr);
        c->modeCombo = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            S(c, 315), S(c, 16), S(c, 150), S(c, 180), w, reinterpret_cast<HMENU>(ModeId), nullptr, nullptr);
        SendMessageW(c->modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"始终显示"));
        SendMessageW(c->modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"空闲检测"));
        SendMessageW(c->modeCombo, CB_SETCURSEL, c->mode, 0);
        CreateWindowW(L"STATIC", L"空闲秒数", WS_CHILD | WS_VISIBLE,
            S(c, 480), S(c, 18), S(c, 78), S(c, 24), w, nullptr, nullptr, nullptr);
        wchar_t idleText[16]{};
        swprintf_s(idleText, L"%d", c->idleSeconds);
        c->idleEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", idleText,
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
            S(c, 558), S(c, 15), S(c, 65), S(c, 26), w,
            reinterpret_cast<HMENU>(IdleSecondsId), nullptr, nullptr);
        HWND combo = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            S(c, 640), S(c, 16), S(c, 250), S(c, 300), w, reinterpret_cast<HMENU>(PresetId), nullptr, nullptr);
        for (const auto& p : Presets) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.name));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);

        for (int i = 0; i < 14; ++i) {
            int col = i / 7;
            int row = i % 7;
            int x = 24 + col * 430;
            int y = 72 + row * 66;
            CreateWindowW(L"STATIC", Specs[i].label, WS_CHILD | WS_VISIBLE,
                S(c, x), S(c, y), S(c, 120), S(c, 22), w, nullptr, nullptr, nullptr);
            c->values[i] = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                S(c, x + 330), S(c, y), S(c, 70), S(c, 22), w, nullptr, nullptr, nullptr);
            c->sliders[i] = CreateWindowW(TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ,
                S(c, x), S(c, y + 22), S(c, 400), S(c, 36), w,
                reinterpret_cast<HMENU>(SliderBase + i), nullptr, nullptr);
            SendMessageW(c->sliders[i], TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
        }

        CreateWindowW(L"BUTTON", L"保存设置", WS_CHILD | WS_VISIBLE,
            S(c, 410), S(c, 548), S(c, 140), S(c, 42), w,
            reinterpret_cast<HMENU>(SaveOnlyId), nullptr, nullptr);

        CreateWindowW(L"BUTTON", L"应用并启动", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            S(c, 570), S(c, 548), S(c, 140), S(c, 42), w, reinterpret_cast<HMENU>(ApplyId), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
            S(c, 730), S(c, 548), S(c, 120), S(c, 42), w, reinterpret_cast<HMENU>(CancelId), nullptr, nullptr);
        Refresh(c);
        EnumChildWindows(w, [](HWND child, LPARAM value) -> BOOL {
            auto* context = reinterpret_cast<Context*>(value);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(context->bodyFont), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(c));
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wp), TextColor);
        SetBkColor(reinterpret_cast<HDC>(wp), Surface);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor(reinterpret_cast<HDC>(wp), Surface);
        SetTextColor(reinterpret_cast<HDC>(wp), TextColor);
        return reinterpret_cast<LRESULT>(c ? c->fieldBrush : nullptr);
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(w, &client);
        FillRect(reinterpret_cast<HDC>(wp), &client, c->backgroundBrush);
        return 1;
    }
    case WM_HSCROLL:
        ReadSliders(c);
        Refresh(c);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == PresetId && HIWORD(wp) == CBN_SELCHANGE) {
            int idx = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lp), CB_GETCURSEL, 0, 0));
            if (idx >= 0 && idx < static_cast<int>(std::size(Presets))) {
                FromArray(c->settings, Presets[idx].v);
                Refresh(c);
            }
            return 0;
        }
        if (LOWORD(wp) == ApplyId) {
            SaveFromControls(c);
            PostMessageW(c->owner, WM_DESKTODO_APPLY_BLACK_HOLE_SETTINGS, 0, 0);
            DestroyWindow(w);
            return 0;
        }
        if (LOWORD(wp) == SaveOnlyId) {
            SaveFromControls(c);
            MessageBoxW(w, L"黑洞设置已保存。", L"DeskTodo",
                MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wp) == CancelId) {
            DestroyWindow(w);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DPICHANGED: {
        RECT current{};
        GetWindowRect(w, &current);
        HMONITOR monitor = MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(info) };
        if (monitor && GetMonitorInfoW(monitor, &info)) {
            const POINT center{ (current.left + current.right) / 2,
                                (current.top + current.bottom) / 2 };
            const auto placement = SettingsWindowLayout::FitToWorkAreaForBase(
                info.rcWork, center, DesignWidth, DesignHeight);
            c->layoutScale = placement.scale;
            c->monitor = monitor;
            SetWindowPos(w, nullptr, placement.rect.left, placement.rect.top,
                placement.rect.right - placement.rect.left,
                placement.rect.bottom - placement.rect.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_DESTROY:
        EnableWindow(c->owner, TRUE);
        SetForegroundWindow(c->owner);
        if (c->bodyFont) DeleteObject(c->bodyFont);
        if (c->fieldBrush) DeleteObject(c->fieldBrush);
        if (c->backgroundBrush) DeleteObject(c->backgroundBrush);
        delete c;
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
}

BlackHole::Settings Load() {
    BlackHole::Settings s;
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return s;
    auto v = ToArray(s);
    for (size_t i = 0; i < v.size(); ++i) {
        DWORD type = 0;
        DWORD bytes = sizeof(float);
        RegQueryValueExW(key, ValueNames[i], nullptr, &type, reinterpret_cast<BYTE*>(&v[i]), &bytes);
    }
    RegCloseKey(key);
    FromArray(s, v);
    return BlackHole::Settings::Sanitize(s);
}

void Show(HWND owner) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = ClassName;
    RegisterClassExW(&wc);

    auto* c = new Context;
    c->owner = owner;
    c->settings = Load();
    HKEY configKey{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RegistryPath, 0, KEY_QUERY_VALUE, &configKey) == ERROR_SUCCESS) {
        DWORD type = 0, bytes = sizeof(DWORD), value = 0;
        if (RegQueryValueExW(configKey, L"displayMode", nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS && type == REG_DWORD)
            c->mode = (std::clamp)(static_cast<int>(value), 0, 1);
        value = 300; bytes = sizeof(DWORD); type = 0;
        if (RegQueryValueExW(configKey, L"idleSeconds", nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS && type == REG_DWORD)
            c->idleSeconds = (std::clamp)(static_cast<int>(value), 10, 86400);
        RegCloseKey(configKey);
    }

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    HMONITOR monitor = MonitorFromRect(&ownerRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    RECT workArea{};
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        workArea = info.rcWork;
    } else {
        workArea.left = 0;
        workArea.top = 0;
        workArea.right = GetSystemMetrics(SM_CXSCREEN);
        workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const auto placement = SettingsWindowLayout::FitToWorkAreaForBase(
        workArea,
        POINT{ (ownerRect.left + ownerRect.right) / 2, (ownerRect.top + ownerRect.bottom) / 2 },
        DesignWidth, DesignHeight);
    c->layoutScale = placement.scale;
    c->monitor = monitor;

    EnableWindow(owner, FALSE);
    HWND w = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        ClassName,
        L"DeskTodo - 黑洞设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        placement.rect.left,
        placement.rect.top,
        placement.rect.right - placement.rect.left,
        placement.rect.bottom - placement.rect.top,
        owner,
        nullptr,
        wc.hInstance,
        c);
    SetWindowPos(w, HWND_TOP, placement.rect.left, placement.rect.top,
        placement.rect.right - placement.rect.left,
        placement.rect.bottom - placement.rect.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
}
