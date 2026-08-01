#include "PomodoroWidgetWindow.h"

#include "Services/DesktopPathService.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <windowsx.h>

namespace {

constexpr wchar_t WindowClass[] = L"DeskTodoPomodoroWidget";
constexpr int WindowWidth = 420;
constexpr int WindowHeight = 260;
constexpr int MinimumWindowWidth = 320;
constexpr int MinimumWindowHeight = 204;
constexpr UINT_PTR TimerId = 1;

void ApplyCardShape(HWND window) {
    RECT client{};
    if (!window || !GetClientRect(window, &client)) return;
    HRGN region = CreateRoundRectRgn(0, 0, client.right,
                                     client.bottom, 28, 28);
    if (region) SetWindowRgn(window, region, TRUE);
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawLabel(HDC dc, const std::wstring& text, RECT rect, HFONT font,
               COLORREF color, UINT flags) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, oldFont);
}

}

PomodoroWidgetWindow::PomodoroWidgetWindow(HINSTANCE instance) : instance_(instance) {
    LoadState();
}

PomodoroWidgetWindow::~PomodoroWidgetWindow() {
    Destroy();
}

void PomodoroWidgetWindow::RestoreIfVisible() {
    if (shouldRestore_) ShowOrCreate();
}

void PomodoroWidgetWindow::ShowOrCreate() {
    if (!EnsureWindow()) return;
    ShowWindow(window_, SW_SHOWNORMAL);
    SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
    shouldRestore_ = true;
    SaveState(true);
}

void PomodoroWidgetWindow::Destroy() {
    if (!window_) return;
    SaveState(IsWindowVisible(window_) == TRUE);
    KillTimer(window_, TimerId);
    DestroyWindow(window_);
    window_ = nullptr;
}

bool PomodoroWidgetWindow::EnsureWindow() {
    if (window_ && IsWindow(window_)) return true;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{ sizeof(cls) };
        cls.lpfnWndProc = StaticWindowProc;
        cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = nullptr;
        cls.lpszClassName = WindowClass;
        registered = RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return false;

    RECT position = savedRect_;
    if (position.right <= position.left || position.bottom <= position.top) {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        position.left = monitor.rcWork.right - WindowWidth - 28;
        position.top = monitor.rcWork.bottom - WindowHeight - 28;
        position.right = position.left + WindowWidth;
        position.bottom = position.top + WindowHeight;
    }

    const int savedWidth = position.right - position.left;
    const int savedHeight = position.bottom - position.top;
    const int windowWidth = savedWidth >= MinimumWindowWidth
        ? savedWidth : WindowWidth;
    const int windowHeight = savedHeight >= MinimumWindowHeight
        ? savedHeight : WindowHeight;

    HMONITOR monitorHandle = MonitorFromRect(&position, MONITOR_DEFAULTTONULL);
    if (!monitorHandle) {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &monitor);
        position.left = monitor.rcWork.right - windowWidth - 28;
        position.top = monitor.rcWork.bottom - windowHeight - 28;
    }

    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        WindowClass, L"番茄时钟", WS_POPUP | WS_THICKFRAME,
        position.left, position.top, windowWidth, windowHeight,
        nullptr, nullptr, instance_, this);
    if (!window_) return false;
    SetLayeredWindowAttributes(window_, 0, 226, LWA_ALPHA);
    ApplyCardShape(window_);
    if (timer_.running) SetTimer(window_, TimerId, 1000, nullptr);
    return true;
}

void PomodoroWidgetWindow::LoadState() {
    std::ifstream stream(std::filesystem::path(GetAppDataPath()) / L"pomodoro-widget.dat",
                         std::ios::binary);
    PersistedState persisted{};
    if (!stream.read(reinterpret_cast<char*>(&persisted), sizeof(persisted)) ||
        persisted.magic != 0x504F4D4F || persisted.version != 1) return;
    savedRect_ = persisted.rect;
    timer_ = persisted.timer;
    shouldRestore_ = persisted.visible == TRUE;
    if (timer_.remainingSeconds <= 0 ||
        timer_.remainingSeconds > PomodoroTimer::PhaseDuration(timer_)) {
        PomodoroTimer::Reset(timer_);
    }
}

void PomodoroWidgetWindow::SaveState(bool visible) {
    EnsureDirectoryExists(GetAppDataPath());
    PersistedState persisted{};
    persisted.visible = visible ? TRUE : FALSE;
    persisted.timer = timer_;
    if (window_ && IsWindow(window_)) GetWindowRect(window_, &persisted.rect);
    else persisted.rect = savedRect_;
    savedRect_ = persisted.rect;
    std::ofstream stream(std::filesystem::path(GetAppDataPath()) / L"pomodoro-widget.dat",
                         std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&persisted), sizeof(persisted));
}

void PomodoroWidgetWindow::ToggleTimer() {
    timer_.running = !timer_.running;
    if (timer_.running) SetTimer(window_, TimerId, 1000, nullptr);
    else KillTimer(window_, TimerId);
    SaveState(true);
    InvalidateRect(window_, nullptr, FALSE);
}

RECT PomodoroWidgetWindow::StartButtonRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return { Scale(36), client.bottom - Scale(56),
             client.right / 2 - Scale(6), client.bottom - Scale(20) };
}
RECT PomodoroWidgetWindow::ResetButtonRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return { client.right / 2 + Scale(6), client.bottom - Scale(56),
             client.right - Scale(36), client.bottom - Scale(20) };
}
RECT PomodoroWidgetWindow::CloseButtonRect() const {
    RECT client{};
    GetClientRect(window_, &client);
    return { client.right - Scale(42), Scale(12),
             client.right - Scale(14), Scale(40) };
}

float PomodoroWidgetWindow::UiScale() const {
    RECT client{};
    if (!window_ || !GetClientRect(window_, &client) ||
        client.right <= 0 || client.bottom <= 0) {
        return 1.0f;
    }
    const float widthScale = static_cast<float>(client.right) /
        static_cast<float>(WindowWidth);
    const float heightScale = static_cast<float>(client.bottom) /
        static_cast<float>(WindowHeight);
    return (std::clamp)((std::max)(widthScale, heightScale), 0.92f, 1.35f);
}

int PomodoroWidgetWindow::Scale(int value) const {
    return (std::max)(1, static_cast<int>(std::lround(
        static_cast<float>(value) * UiScale())));
}

void PomodoroWidgetWindow::Paint(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    FillRounded(dc, client, 28, RGB(238, 234, 236));

    HFONT titleFont = CreateFontW(-Scale(17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Display");
    HFONT timerFont = CreateFontW(-Scale(48), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Display");
    HFONT bodyFont = CreateFontW(-Scale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI Variable Text");

    DrawLabel(dc, L"专注计时", { Scale(20), Scale(13),
        client.right - Scale(80), Scale(38) }, titleFont,
        RGB(45, 42, 47), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawLabel(dc, L"×", CloseButtonRect(), titleFont,
              RGB(112, 107, 114), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    wchar_t timeText[16]{};
    swprintf_s(timeText, L"%02d:%02d", timer_.remainingSeconds / 60,
               timer_.remainingSeconds % 60);
    DrawLabel(dc, timeText, { Scale(34), Scale(50),
        client.right - Scale(34), Scale(112) }, timerFont,
              RGB(28, 27, 30), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const std::wstring phase = timer_.focusPhase ? L"专注" : L"短休息";
    const std::wstring subtitle = phase + L" · 已完成 " +
        std::to_wstring(timer_.completedFocusSessions) + L" 轮";
    DrawLabel(dc, subtitle, { Scale(20), Scale(116),
        client.right - Scale(20), Scale(138) }, bodyFont,
        RGB(112, 107, 114), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT progressTrack{ Scale(36), Scale(158),
                        client.right - Scale(36), Scale(164) };
    FillRounded(dc, progressTrack, Scale(6), RGB(216, 211, 216));
    const int duration = PomodoroTimer::PhaseDuration(timer_);
    const double completed = 1.0 - static_cast<double>(timer_.remainingSeconds) / duration;
    RECT progress = progressTrack;
    progress.right = progress.left + static_cast<int>((progressTrack.right - progressTrack.left) *
        std::clamp(completed, 0.0, 1.0));
    if (progress.right > progress.left) {
        FillRounded(dc, progress, Scale(6), RGB(151, 140, 153));
    }

    FillRounded(dc, StartButtonRect(), Scale(12), RGB(91, 84, 94));
    FillRounded(dc, ResetButtonRect(), Scale(12), RGB(220, 215, 220));
    DrawLabel(dc, timer_.running ? L"暂停" : L"开始", StartButtonRect(), bodyFont,
              RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawLabel(dc, L"重置", ResetButtonRect(), bodyFont,
              RGB(65, 61, 68), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DeleteObject(titleFont);
    DeleteObject(timerFont);
    DeleteObject(bodyFont);
}

LRESULT CALLBACK PomodoroWidgetWindow::StaticWindowProc(HWND window, UINT message,
                                                         WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PomodoroWidgetWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<PomodoroWidgetWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->WindowProc(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}

LRESULT PomodoroWidgetWindow::WindowProc(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = { MinimumWindowWidth, MinimumWindowHeight };
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wp) {
            auto* size = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            size->rgrc[0] = size->rgrc[1];
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(window_, &point);
        RECT client{};
        GetClientRect(window_, &client);
        constexpr int edge = 8;
        const bool left = point.x < client.left + edge;
        const bool right = point.x >= client.right - edge;
        const bool top = point.y < client.top + edge;
        const bool bottom = point.y >= client.bottom - edge;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }
    case WM_SIZE:
        ApplyCardShape(window_);
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
        Paint(memory);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT clientPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const RECT closeButton = CloseButtonRect();
        const RECT startButton = StartButtonRect();
        const RECT resetButton = ResetButtonRect();
        if (PtInRect(&closeButton, clientPoint)) {
            ShowWindow(window_, SW_HIDE);
            timer_.running = false;
            KillTimer(window_, TimerId);
            shouldRestore_ = false;
            SaveState(false);
            return 0;
        }
        if (PtInRect(&startButton, clientPoint)) {
            ToggleTimer();
            return 0;
        }
        if (PtInRect(&resetButton, clientPoint)) {
            PomodoroTimer::Reset(timer_);
            KillTimer(window_, TimerId);
            SaveState(true);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (clientPoint.y < 46) {
            dragging_ = true;
            GetPhysicalCursorPos(&dragStart_);
            GetWindowRect(window_, &dragWindowStart_);
            SetCapture(window_);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dragging_) {
            POINT cursor{};
            GetPhysicalCursorPos(&cursor);
            SetWindowPos(window_, nullptr,
                dragWindowStart_.left + cursor.x - dragStart_.x,
                dragWindowStart_.top + cursor.y - dragStart_.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            SaveState(true);
        }
        return 0;
    case WM_EXITSIZEMOVE:
        RedrawWindow(window_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
        SaveState(true);
        return 0;
    case WM_CAPTURECHANGED:
        dragging_ = false;
        return 0;
    case WM_TIMER:
        if (wp == TimerId) {
            const bool phaseChanged = PomodoroTimer::Tick(timer_);
            if (phaseChanged) MessageBeep(MB_ICONINFORMATION);
            if (timer_.remainingSeconds % 10 == 0 || phaseChanged) SaveState(true);
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(window_, SW_HIDE);
        timer_.running = false;
        KillTimer(window_, TimerId);
        shouldRestore_ = false;
        SaveState(false);
        return 0;
    case WM_DESTROY:
        KillTimer(window_, TimerId);
        return 0;
    }
    return DefWindowProcW(window_, message, wp, lp);
}
