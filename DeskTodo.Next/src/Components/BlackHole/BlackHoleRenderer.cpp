#include "BlackHoleRenderer.h"
#include "D3D11BlackHoleBackend.h"
#include "Window/BlackHoleSettingsWindow.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int Side = 384;
constexpr wchar_t ClassName[] = L"DeskTodo.BlackHoleRenderer";
struct Renderer {
    BlackHole::D3D11BlackHoleBackend backend;
    BlackHole::Settings settings;
    HDC output = nullptr, background = nullptr;
    HBITMAP outputBitmap = nullptr, backgroundBitmap = nullptr;
    HGDIOBJ oldOutput = nullptr, oldBackground = nullptr;
    BYTE* pixels = nullptr;
    BYTE* desktop = nullptr;
    std::vector<uint8_t> frame;
    ULONGLONG start = GetTickCount64();
    DWORD mode = 0, idleSeconds = 300;
    ~Renderer() {
        if (output) { if (oldOutput) SelectObject(output, oldOutput); DeleteDC(output); }
        if (background) { if (oldBackground) SelectObject(background, oldBackground); DeleteDC(background); }
        if (outputBitmap) DeleteObject(outputBitmap);
        if (backgroundBitmap) DeleteObject(backgroundBitmap);
    }
    bool Initialize() {
        settings = BlackHoleSettingsWindow::Load();
        auto readDword = [](const wchar_t* name, DWORD& value) {
            DWORD bytes = sizeof(value), loaded = value;
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\DeskTodo\\BlackHole", name,
                RRF_RT_REG_DWORD, nullptr, &loaded, &bytes) == ERROR_SUCCESS) value = loaded;
        };
        readDword(L"displayMode", mode); readDword(L"idleSeconds", idleSeconds);
        mode = (std::min)(mode, 1UL);
        idleSeconds = (std::clamp)(idleSeconds, 10UL, 86400UL);
        BlackHole::SurfaceDescriptor surface;
        surface.width = surface.height = Side;
        if (!backend.Initialize(surface)) return false;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), Side, -Side, 1, 32, BI_RGB};
        output = CreateCompatibleDC(nullptr); background = CreateCompatibleDC(nullptr);
        outputBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        backgroundBitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&desktop), nullptr, 0);
        if (!output || !background || !outputBitmap || !backgroundBitmap || !pixels || !desktop) return false;
        oldOutput = SelectObject(output, outputBitmap);
        oldBackground = SelectObject(background, backgroundBitmap);
        return true;
    }
    bool Draw(HWND window) {
        LASTINPUTINFO input{sizeof(input)};
        if (mode && GetLastInputInfo(&input) && DWORD(static_cast<DWORD>(GetTickCount64()) - input.dwTime) / 1000 < idleSeconds) {
            ShowWindow(window, SW_HIDE); return true;
        }
        const double t = (GetTickCount64() - start) / 1000.0;
        std::vector<RECT> monitors;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
            MONITORINFO info{sizeof(info)};
            if (GetMonitorInfoW(monitor, &info)) reinterpret_cast<std::vector<RECT>*>(data)->push_back(info.rcWork);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors));
        if (monitors.empty()) return true;
        const RECT work = monitors[(static_cast<size_t>(t / 40)) % monitors.size()];
        POINT position{
            work.left + static_cast<LONG>((std::max)(0L, work.right - work.left - Side) * (0.5 + 0.45 * sin(t * .071))),
            work.top + static_cast<LONG>((std::max)(0L, work.bottom - work.top - Side) * (0.5 + 0.45 * sin(t * .053 + 2)))};
        HDC screen = GetDC(nullptr);
        if (!screen) return true;
        const bool captured = BitBlt(background, 0, 0, Side, Side, screen, position.x, position.y, SRCCOPY) != FALSE;
        ReleaseDC(nullptr, screen);
        if (!captured) return true;
        GdiFlush();
        BlackHole::FrameContext context;
        context.monotonicSeconds = t;
        BlackHole::FrameStats stats;
        uint32_t width = 0, height = 0;
        if (!backend.Render(settings, context, stats) || !backend.CopyFramePixels(frame, width, height) ||
            width != Side || height != Side) return false;
        // Composite the ray-marched disk over a local desktop lens. The layered
        // window is excluded from capture and never receives pointer input.
        const double centerX = Side * (0.5 + .04 * sin(t * .21) + .015 * sin(t * .083));
        const double centerY = Side * (0.5 + .03 * sin(t * .157 + 2) + .015 * sin(t * .117));
        for (int y = 0; y < Side; ++y) for (int x = 0; x < Side; ++x) {
            const double dx = x - centerX, dy = y - centerY;
            const double radius = sqrt(dx * dx + dy * dy);
            const double alpha = (std::clamp)((Side * .46 - radius) / (Side * .12), 0.0, 1.0);
            const double warp = settings.gravatationalLensing ? 1.0 + 100.0 / (radius * radius + 80.0) : 1.0;
            const int sx = (std::clamp)(static_cast<int>(centerX + dx * warp), 0, Side - 1);
            const int sy = (std::clamp)(static_cast<int>(centerY + dy * warp), 0, Side - 1);
            const size_t index = (y * Side + x) * 4, source = (sy * Side + sx) * 4;
            const double coverage = frame[index + 3] / 255.0;
            for (int c = 0; c < 3; ++c) {
                const double value = frame[index + c] + desktop[source + c] * (1 - coverage);
                pixels[index + c] = static_cast<BYTE>((std::min)(255.0, value) * alpha);
            }
            pixels[index + 3] = static_cast<BYTE>(255 * alpha);
        }
        POINT origin{}; SIZE size{Side, Side}; BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window, nullptr, &position, &size, output, &origin, 0, &blend, ULW_ALPHA)) return false;
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }
};
LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        renderer = static_cast<Renderer*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_TIMER && renderer) {
        try { if (!renderer->Draw(window)) DestroyWindow(window); }
        catch (...) { DestroyWindow(window); }
        return 0;
    }
    if (message == WM_DESTROY) { KillTimer(window, 1); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
}

int RunBlackHoleRenderer() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Renderer renderer;
    if (!renderer.Initialize()) return 1;
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = Proc; cls.lpszClassName = ClassName;
    if (!RegisterClassExW(&cls)) return 1;
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        ClassName, L"DeskTodo 黑洞", WS_POPUP, 0, 0, Side, Side, nullptr, nullptr, cls.hInstance, &renderer);
    if (!window) return 1;
    SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
    if (!SetTimer(window, 1, 1000 / 24, nullptr)) { DestroyWindow(window); return 1; }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (IsWindow(window)) DestroyWindow(window);
    return 0;
}
