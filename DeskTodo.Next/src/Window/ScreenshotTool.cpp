#include "ScreenshotTool.h"

#include "Models/CaptureRegion.h"
#include "Services/DesktopPathService.h"
#include "Services/OcrService.h"
#include "Window/OcrResultWindow.h"

#include <ShlObj.h>
#include <algorithm>
#include <commdlg.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <wincodec.h>
#include <windowsx.h>
#include <shellapi.h>

namespace {

constexpr wchar_t CaptureClass[] = L"DeskTodoCaptureOverlay";
constexpr wchar_t PinClass[] = L"DeskTodoPinnedCapture";

RECT ToClientRect(const RECT& screenRect, const RECT& virtualScreen) {
    return { screenRect.left - virtualScreen.left, screenRect.top - virtualScreen.top,
             screenRect.right - virtualScreen.left, screenRect.bottom - virtualScreen.top };
}

void FillArea(HDC dc, const RECT& rect, COLORREF color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void ShadeArea(HDC dc, const RECT& rect, BYTE opacity) {
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    HDC layer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    if (!layer || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (layer) DeleteDC(layer);
        return;
    }
    HGDIOBJ old = SelectObject(layer, bitmap);
    RECT area{ 0, 0, width, height };
    FillArea(layer, area, RGB(16, 20, 28));
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, opacity, 0 };
    AlphaBlend(dc, rect.left, rect.top, width, height,
               layer, 0, 0, width, height, blend);
    SelectObject(layer, old);
    DeleteObject(bitmap);
    DeleteDC(layer);
}

void CaptureDiag(const wchar_t* message) {
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        L"DESKTODO_CAPTURE_DIAG", path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return;
    std::wofstream stream(path, std::ios::app);
    if (stream) stream << message << L"\n";
}

void CaptureDiag(const std::wstring& message) {
    CaptureDiag(message.c_str());
}

bool IsControlPressed() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

constexpr COLORREF AnnotationColors[] = {
    RGB(234, 82, 92), RGB(220, 55, 55), RGB(236, 181, 48),
    RGB(59, 166, 97), RGB(55, 145, 220), RGB(39, 51, 67), RGB(250, 250, 250)
};
constexpr int AnnotationWidths[] = { 2, 4, 6, 8, 12 };
constexpr int AnnotationTextSizes[] = { 14, 18, 24, 32, 48 };
constexpr const wchar_t* AnnotationFonts[] = {
    L"Microsoft YaHei UI", L"SimSun", L"Arial", L"Consolas"
};
constexpr UINT CaptureHintTimerId = 0xD710;

constexpr CaptureAction ToolbarActions[] = {
    CaptureAction::Cancel, CaptureAction::Pin, CaptureAction::Save,
    CaptureAction::Copy, CaptureAction::Ocr, CaptureAction::LatexOcr,
    CaptureAction::Move, CaptureAction::Pen, CaptureAction::Arrow,
    CaptureAction::Text, CaptureAction::Rectangle, CaptureAction::Clear
};

const wchar_t* CaptureActionHint(CaptureAction action) {
    switch (action) {
    case CaptureAction::Cancel: return L"取消";
    case CaptureAction::Pin: return L"贴图";
    case CaptureAction::Save: return L"保存";
    case CaptureAction::Copy: return L"复制";
    case CaptureAction::Ocr: return L"文字识别";
    case CaptureAction::LatexOcr: return L"LaTeX 识别";
    case CaptureAction::Move: return L"移动选区";
    case CaptureAction::Pen: return L"画笔";
    case CaptureAction::Arrow: return L"箭头";
    case CaptureAction::Text: return L"文字";
    case CaptureAction::Rectangle: return L"矩形";
    case CaptureAction::Clear: return L"清除标注";
    default: return L"";
    }
}

void DrawCaptureIcon(HDC dc, RECT area, CaptureAction action, COLORREF color) {
    const int centerX = (area.left + area.right) / 2;
    const int centerY = (area.top + area.bottom) / 2;
    const int left = centerX - 10;
    const int top = centerY - 10;
    const int right = centerX + 10;
    const int bottom = centerY + 10;
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    auto line = [&](int x1, int y1, int x2, int y2) {
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
    };
    switch (action) {
    case CaptureAction::Cancel:
        line(left + 3, top + 3, right - 3, bottom - 3);
        line(right - 3, top + 3, left + 3, bottom - 3);
        break;
    case CaptureAction::Pin:
        Ellipse(dc, centerX - 5, top + 1, centerX + 6, top + 9);
        line(centerX, top + 9, centerX, bottom - 2);
        line(centerX - 4, bottom - 2, centerX + 4, bottom - 2);
        line(centerX, bottom - 2, centerX - 3, bottom + 3);
        break;
    case CaptureAction::Save:
        Rectangle(dc, left + 2, top + 1, right - 2, bottom - 1);
        Rectangle(dc, left + 6, top + 2, right - 6, top + 8);
        Rectangle(dc, left + 6, centerY + 2, right - 6, bottom - 2);
        break;
    case CaptureAction::Copy:
        Rectangle(dc, left + 1, top + 5, right - 5, bottom - 1);
        Rectangle(dc, left + 5, top + 1, right - 1, bottom - 5);
        break;
    case CaptureAction::Ocr:
        line(left, top + 6, left, top); line(left, top, left + 6, top);
        line(right - 6, top, right, top); line(right, top, right, top + 6);
        line(left, bottom - 6, left, bottom); line(left, bottom, left + 6, bottom);
        line(right - 6, bottom, right, bottom); line(right, bottom, right, bottom - 6);
        line(centerX - 4, centerY + 5, centerX, centerY - 5);
        line(centerX, centerY - 5, centerX + 4, centerY + 5);
        line(centerX - 2, centerY + 1, centerX + 2, centerY + 1);
        break;
    case CaptureAction::LatexOcr: {
        HFONT symbol = CreateFontW(-23, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Cambria Math");
        HGDIOBJ oldFont = SelectObject(dc, symbol);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, L"∑", -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        DeleteObject(symbol);
        break;
    }
    case CaptureAction::Move:
        line(centerX, top, centerX, bottom);
        line(left, centerY, right, centerY);
        line(centerX, top, centerX - 3, top + 4);
        line(centerX, top, centerX + 3, top + 4);
        line(centerX, bottom, centerX - 3, bottom - 4);
        line(centerX, bottom, centerX + 3, bottom - 4);
        line(left, centerY, left + 4, centerY - 3);
        line(left, centerY, left + 4, centerY + 3);
        line(right, centerY, right - 4, centerY - 3);
        line(right, centerY, right - 4, centerY + 3);
        break;
    case CaptureAction::Pen:
        line(left + 2, bottom - 2, right - 4, top + 4);
        line(left + 2, bottom - 2, left + 7, bottom - 3);
        line(right - 4, top + 4, right - 1, top + 1);
        line(right - 1, top + 1, right + 2, top + 4);
        line(right + 2, top + 4, right - 1, top + 7);
        break;
    case CaptureAction::Arrow:
        line(left + 1, bottom - 1, right - 2, top + 2);
        line(right - 2, top + 2, right - 3, top + 9);
        line(right - 2, top + 2, right - 9, top + 3);
        break;
    case CaptureAction::Text:
        line(left + 2, top + 2, right - 2, top + 2);
        line(centerX, top + 2, centerX, bottom - 2);
        line(centerX - 5, bottom - 2, centerX + 5, bottom - 2);
        break;
    case CaptureAction::Rectangle:
        Rectangle(dc, left + 1, top + 2, right - 1, bottom - 2);
        break;
    case CaptureAction::Clear:
        line(left + 4, top + 5, right - 4, top + 5);
        line(left + 7, top + 2, right - 7, top + 2);
        Rectangle(dc, left + 6, top + 7, right - 6, bottom - 1);
        line(centerX - 2, top + 9, centerX - 2, bottom - 3);
        line(centerX + 2, top + 9, centerX + 2, bottom - 3);
        break;
    default:
        break;
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

}

ScreenshotTool::ScreenshotTool(HINSTANCE instance) : instance_(instance) {}
ScreenshotTool::~ScreenshotTool() { Destroy(); }

bool ScreenshotTool::RegisterClasses() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW capture{ sizeof(capture) };
    capture.lpfnWndProc = CaptureWindowProc;
    capture.hInstance = instance_;
    capture.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    capture.hbrBackground = nullptr;
    capture.lpszClassName = CaptureClass;
    registered = RegisterClassExW(&capture) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    if (!registered) return false;
    WNDCLASSEXW pin{ sizeof(pin) };
    pin.lpfnWndProc = PinWindowProc;
    pin.hInstance = instance_;
    pin.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    pin.hbrBackground = nullptr;
    pin.lpszClassName = PinClass;
    pin.style = CS_DBLCLKS;
    return RegisterClassExW(&pin) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool ScreenshotTool::CaptureVirtualScreen() {
    CaptureDiag(L"CaptureVirtualScreen: begin");
    if (screenBitmap_) { DeleteObject(screenBitmap_); screenBitmap_ = nullptr; }
    virtualScreen_ = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    const int width = virtualScreen_.right - virtualScreen_.left;
    const int height = virtualScreen_.bottom - virtualScreen_.top;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    screenBitmap_ = CreateCompatibleBitmap(screen, width, height);
    if (!screen) CaptureDiag(L"CaptureVirtualScreen: GetDC failed");
    if (!memory) CaptureDiag(L"CaptureVirtualScreen: CreateCompatibleDC failed");
    if (!screenBitmap_) CaptureDiag(L"CaptureVirtualScreen: CreateCompatibleBitmap failed");
    HGDIOBJ old = SelectObject(memory, screenBitmap_);
    // CAPTUREBLT includes DeskTodo's full-screen layered overlay and can turn the
    // composed result black on some GPU/driver combinations. SRCCOPY captures
    // the visible desktop below that overlay reliably.
    const bool ok = BitBlt(memory, 0, 0, width, height, screen,
                           virtualScreen_.left, virtualScreen_.top, SRCCOPY) == TRUE;
    SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!ok) { DeleteObject(screenBitmap_); screenBitmap_ = nullptr; }
    CaptureDiag(ok ? L"CaptureVirtualScreen: ok" : L"CaptureVirtualScreen: BitBlt failed");
    return ok;
}

bool ScreenshotTool::StartCapture() {
    CaptureDiag(L"StartCapture: begin");
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    GdiFlush();
    if (!RegisterClasses()) {
        CaptureDiag(L"StartCapture: RegisterClasses failed");
        return false;
    }
    if (!CaptureVirtualScreen()) {
        CaptureDiag(L"StartCapture: CaptureVirtualScreen failed");
        return false;
    }
    if (captureWindow_) DestroyWindow(captureWindow_);
    selection_ = {};
    selecting_ = false;
    selectionReady_ = false;
    activeTool_ = CaptureAction::Move;
    strokes_.clear();
    arrows_.clear();
    texts_.clear();
    rectangles_.clear();
    pendingText_.clear();
    clickedHintAction_ = CaptureAction::None;
    hoveredHintAction_ = CaptureAction::None;
    captureWindow_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CaptureClass, L"DeskTodo 截图", WS_POPUP,
        virtualScreen_.left, virtualScreen_.top,
        virtualScreen_.right - virtualScreen_.left,
        virtualScreen_.bottom - virtualScreen_.top,
        nullptr, nullptr, instance_, this);
    if (!captureWindow_) return false;
    ShowWindow(captureWindow_, SW_SHOW);
    SetForegroundWindow(captureWindow_);
    SetCapture(captureWindow_);
    CaptureDiag(L"StartCapture: window shown");
    return true;
}

bool ScreenshotTool::EnsureCaptureBuffer(HDC reference, int width, int height) {
    if (captureBufferDc_ && captureBufferBitmap_ &&
        captureBufferSize_.cx == width && captureBufferSize_.cy == height) return true;
    DestroyCaptureBuffer();
    captureBufferDc_ = CreateCompatibleDC(reference);
    captureBufferBitmap_ = CreateCompatibleBitmap(reference, width, height);
    if (!captureBufferDc_ || !captureBufferBitmap_) {
        DestroyCaptureBuffer();
        return false;
    }
    captureBufferOld_ = SelectObject(captureBufferDc_, captureBufferBitmap_);
    captureBufferSize_ = { width, height };
    return true;
}

void ScreenshotTool::DestroyCaptureBuffer() {
    if (captureBufferDc_ && captureBufferOld_) {
        SelectObject(captureBufferDc_, captureBufferOld_);
    }
    if (captureBufferBitmap_) DeleteObject(captureBufferBitmap_);
    if (captureBufferDc_) DeleteDC(captureBufferDc_);
    captureBufferDc_ = nullptr;
    captureBufferBitmap_ = nullptr;
    captureBufferOld_ = nullptr;
    captureBufferSize_ = {};
}

void ScreenshotTool::Destroy() {
    CancelCapture();
    if (pinWindow_) { DestroyWindow(pinWindow_); pinWindow_ = nullptr; }
    if (pinnedBitmap_) { DeleteObject(pinnedBitmap_); pinnedBitmap_ = nullptr; }
}

void ScreenshotTool::CancelCapture() {
    if (captureWindow_) KillTimer(captureWindow_, CaptureHintTimerId);
    if (GetCapture() == captureWindow_) ReleaseCapture();
    if (captureWindow_) { DestroyWindow(captureWindow_); captureWindow_ = nullptr; }
    if (screenBitmap_) { DeleteObject(screenBitmap_); screenBitmap_ = nullptr; }
    DestroyCaptureBuffer();
    selecting_ = false;
    selectionReady_ = false;
}

void ScreenshotTool::PaintCapture(HDC dc) {
    RECT client{};
    GetClientRect(captureWindow_, &client);
    HDC output = dc;
    if (EnsureCaptureBuffer(dc, client.right, client.bottom)) dc = captureBufferDc_;
    HDC source = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(source, screenBitmap_);
    BitBlt(dc, 0, 0, client.right, client.bottom, source, 0, 0, SRCCOPY);
    RECT selected = ToClientRect(selection_, virtualScreen_);
    if (CaptureRegion::IsValid(selection_)) {
        ShadeArea(dc, { 0, 0, client.right, selected.top }, 150);
        ShadeArea(dc, { 0, selected.bottom, client.right, client.bottom }, 150);
        ShadeArea(dc, { 0, selected.top, selected.left, selected.bottom }, 150);
        ShadeArea(dc, { selected.right, selected.top, client.right, selected.bottom }, 150);
        BitBlt(dc, selected.left, selected.top,
               selected.right - selected.left, selected.bottom - selected.top,
               source, selected.left, selected.top, SRCCOPY);
        PaintAnnotations(dc, { virtualScreen_.left, virtualScreen_.top });
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(65, 203, 231));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, selected.left, selected.top, selected.right, selected.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        wchar_t dimensions[64]{};
        swprintf_s(dimensions, L"%ld × %ld", selection_.right - selection_.left,
                   selection_.bottom - selection_.top);
        RECT label{ selected.left, (std::max)(0L, selected.top - 28), selected.left + 130, selected.top };
        FillArea(dc, label, RGB(26, 29, 36));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 239, 244));
        DrawTextW(dc, dimensions, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        ShadeArea(dc, client, 105);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 242, 246));
        RECT hint{ 0, 20, client.right, 62 };
        DrawTextW(dc, L"拖动选择截图区域 · Esc 或右键取消", -1, &hint,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (selectionReady_) PaintCaptureToolbar(dc);
    SelectObject(source, old);
    DeleteDC(source);
    if (dc != output) {
        BitBlt(output, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    }
}

void ScreenshotTool::PaintCaptureToolbar(HDC dc) {
    const CaptureToolbarLayout layout = CaptureToolbar::Build(selection_, virtualScreen_);
    RECT bar = ToClientRect(layout.bounds, virtualScreen_);
    HBRUSH background = CreateSolidBrush(RGB(250, 251, 252));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(203, 210, 220));
    HGDIOBJ oldBrush = SelectObject(dc, background);
    HGDIOBJ oldPen = SelectObject(dc, border);
    RoundRect(dc, bar.left, bar.top, bar.right, bar.bottom, 12, 12);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(border);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    for (size_t index = 0; index < layout.buttons.size(); ++index) {
        RECT button = ToClientRect(layout.buttons[index], virtualScreen_);
        const bool selectedAction = ToolbarActions[index] == activeTool_;
        if (selectedAction) {
            RECT highlight{ button.left + 4, button.top + 5, button.right - 4, button.bottom - 5 };
            HBRUSH selected = CreateSolidBrush(RGB(214, 243, 235));
            HGDIOBJ previousBrush = SelectObject(dc, selected);
            HPEN noBorder = CreatePen(PS_NULL, 0, RGB(214, 243, 235));
            HGDIOBJ previousPen = SelectObject(dc, noBorder);
            RoundRect(dc, highlight.left, highlight.top, highlight.right, highlight.bottom, 9, 9);
            SelectObject(dc, previousPen); SelectObject(dc, previousBrush);
            DeleteObject(noBorder); DeleteObject(selected);
        }
        if (index > 0) {
            HPEN separator = CreatePen(PS_SOLID, 1, RGB(218, 221, 226));
            HGDIOBJ previous = SelectObject(dc, separator);
            MoveToEx(dc, button.left, button.top + 9, nullptr);
            LineTo(dc, button.left, button.bottom - 9);
            SelectObject(dc, previous);
            DeleteObject(separator);
        }
        DrawCaptureIcon(dc, button, ToolbarActions[index],
                        selectedAction ? RGB(12, 116, 94) : RGB(55, 65, 81));
    }
    if (IsStyleToolActive()) PaintCaptureProperties(dc);
    PaintCaptureHint(dc);
}

void ScreenshotTool::PaintCaptureHint(HDC dc) {
    const CaptureAction hintAction = hoveredHintAction_ != CaptureAction::None
        ? hoveredHintAction_ : clickedHintAction_;
    if (hintAction == CaptureAction::None) return;
    const auto toolbar = CaptureToolbar::Build(selection_, virtualScreen_);
    size_t index = 0;
    for (; index < std::size(ToolbarActions); ++index) {
        if (ToolbarActions[index] == hintAction) break;
    }
    if (index == std::size(ToolbarActions)) return;

    const RECT button = toolbar.buttons[index];
    const wchar_t* label = CaptureActionHint(hintAction);
    const int width = hintAction == CaptureAction::LatexOcr ? 124 : 92;
    LONG left = (button.left + button.right - width) / 2;
    LONG top = toolbar.bounds.top - 40;
    if (top < virtualScreen_.top + 4) top = toolbar.bounds.bottom + 8;
    if (left < virtualScreen_.left + 4) left = virtualScreen_.left + 4;
    if (left + width > virtualScreen_.right - 4)
        left = virtualScreen_.right - width - 4;
    RECT hint = ToClientRect({ left, top, left + width, top + 32 }, virtualScreen_);

    HBRUSH brush = CreateSolidBrush(RGB(36, 43, 55));
    HPEN pen = CreatePen(PS_NULL, 0, RGB(36, 43, 55));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, hint.left, hint.top, hint.right, hint.bottom, 9, 9);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(250, 251, 252));
    DrawTextW(dc, label, -1, &hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

bool ScreenshotTool::IsStyleToolActive() const {
    return activeTool_ == CaptureAction::Pen ||
           activeTool_ == CaptureAction::Arrow ||
           activeTool_ == CaptureAction::Text ||
           activeTool_ == CaptureAction::Rectangle;
}

void ScreenshotTool::PaintCaptureProperties(HDC dc) {
    const bool textMode = activeTool_ == CaptureAction::Text;
    const auto toolbar = CaptureToolbar::Build(selection_, virtualScreen_);
    const auto layout = CaptureToolbar::BuildProperty(toolbar, virtualScreen_, textMode);
    RECT panel = ToClientRect(layout.bounds, virtualScreen_);

    RECT shadow = panel;
    OffsetRect(&shadow, 0, 3);
    HBRUSH shadowBrush = CreateSolidBrush(RGB(98, 105, 116));
    HPEN shadowPen = CreatePen(PS_NULL, 0, RGB(98, 105, 116));
    HGDIOBJ oldBrush = SelectObject(dc, shadowBrush);
    HGDIOBJ oldPen = SelectObject(dc, shadowPen);
    RoundRect(dc, shadow.left, shadow.top, shadow.right, shadow.bottom, 12, 12);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(shadowPen); DeleteObject(shadowBrush);

    HBRUSH background = CreateSolidBrush(RGB(250, 251, 252));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(203, 210, 220));
    oldBrush = SelectObject(dc, background);
    oldPen = SelectObject(dc, border);
    RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, 12, 12);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(border); DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(dc, font);

    if (textMode) {
        RECT fontButton = ToClientRect(layout.fontButton, virtualScreen_);
        FillArea(dc, fontButton, RGB(239, 242, 246));
        SetTextColor(dc, RGB(35, 43, 56));
        std::wstring label = AnnotationFonts[textFontIndex_ % std::size(AnnotationFonts)];
        label += L"  \u2304";
        DrawTextW(dc, label.c_str(), -1, &fontButton,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    int activeWidth = penWidth_;
    if (activeTool_ == CaptureAction::Arrow) activeWidth = arrowWidth_;
    if (activeTool_ == CaptureAction::Rectangle) activeWidth = rectangleWidth_;
    if (textMode) activeWidth = textSize_;
    for (size_t index = 0; index < layout.widthButtons.size(); ++index) {
        RECT button = ToClientRect(layout.widthButtons[index], virtualScreen_);
        const int value = textMode ? AnnotationTextSizes[index] : AnnotationWidths[index];
        const bool selected = value == activeWidth;
        FillArea(dc, button, selected ? RGB(214, 243, 235) : RGB(246, 247, 249));
        SetTextColor(dc, selected ? RGB(12, 116, 94) : RGB(55, 65, 81));
        const std::wstring valueText = std::to_wstring(value);
        DrawTextW(dc, valueText.c_str(), -1, &button,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    COLORREF activeColor = penColor_;
    if (activeTool_ == CaptureAction::Arrow) activeColor = arrowColor_;
    if (activeTool_ == CaptureAction::Rectangle) activeColor = rectangleColor_;
    if (textMode) activeColor = textColor_;
    for (size_t index = 0; index < layout.colorButtons.size(); ++index) {
        RECT swatch = ToClientRect(layout.colorButtons[index], virtualScreen_);
        HBRUSH colorBrush = CreateSolidBrush(AnnotationColors[index]);
        const bool selected = AnnotationColors[index] == activeColor;
        HPEN colorBorder = CreatePen(PS_SOLID, selected ? 3 : 1,
            selected ? RGB(14, 165, 133) : RGB(190, 196, 205));
        oldBrush = SelectObject(dc, colorBrush);
        oldPen = SelectObject(dc, colorBorder);
        Ellipse(dc, swatch.left, swatch.top, swatch.right, swatch.bottom);
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
        DeleteObject(colorBorder); DeleteObject(colorBrush);
    }

    SelectObject(dc, oldFont);
    DeleteObject(font);
}

bool ScreenshotTool::HandlePropertyClick(POINT screenPoint) {
    if (!IsStyleToolActive()) return false;
    const bool textMode = activeTool_ == CaptureAction::Text;
    const auto toolbar = CaptureToolbar::Build(selection_, virtualScreen_);
    const auto layout = CaptureToolbar::BuildProperty(toolbar, virtualScreen_, textMode);
    if (!PtInRect(&layout.bounds, screenPoint)) return false;

    if (textMode && PtInRect(&layout.fontButton, screenPoint)) {
        textFontIndex_ = (textFontIndex_ + 1) % static_cast<int>(std::size(AnnotationFonts));
        InvalidateRect(captureWindow_, nullptr, FALSE);
        return true;
    }
    for (size_t index = 0; index < layout.widthButtons.size(); ++index) {
        if (!PtInRect(&layout.widthButtons[index], screenPoint)) continue;
        const int value = textMode ? AnnotationTextSizes[index] : AnnotationWidths[index];
        if (textMode) textSize_ = value;
        else if (activeTool_ == CaptureAction::Pen) penWidth_ = value;
        else if (activeTool_ == CaptureAction::Arrow) arrowWidth_ = value;
        else rectangleWidth_ = value;
        InvalidateRect(captureWindow_, nullptr, FALSE);
        return true;
    }
    for (size_t index = 0; index < layout.colorButtons.size(); ++index) {
        if (!PtInRect(&layout.colorButtons[index], screenPoint)) continue;
        const COLORREF value = AnnotationColors[index];
        if (textMode) textColor_ = value;
        else if (activeTool_ == CaptureAction::Pen) penColor_ = value;
        else if (activeTool_ == CaptureAction::Arrow) arrowColor_ = value;
        else rectangleColor_ = value;
        InvalidateRect(captureWindow_, nullptr, FALSE);
        return true;
    }
    return true;
}

bool CopyText(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { CloseClipboard(); return false; }
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); CloseClipboard(); return false; }
    memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    const bool ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    if (!ok) GlobalFree(memory);
    CloseClipboard();
    return ok;
}

void ScreenshotTool::PaintAnnotations(HDC dc, POINT origin) {
    auto local = [&](POINT p) { return POINT{ p.x - origin.x, p.y - origin.y }; };
    for (const auto& stroke : strokes_) {
        if (stroke.points.size() < 2) continue;
        HPEN pen = CreatePen(PS_SOLID, stroke.width, stroke.color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        POINT first = local(stroke.points.front());
        MoveToEx(dc, first.x, first.y, nullptr);
        for (size_t i = 1; i < stroke.points.size(); ++i) {
            POINT p = local(stroke.points[i]); LineTo(dc, p.x, p.y);
        }
        SelectObject(dc, oldPen); DeleteObject(pen);
    }
    for (const auto& arrow : arrows_) {
        POINT a = local(arrow.start), b = local(arrow.end);
        HPEN pen = CreatePen(PS_SOLID, arrow.width, arrow.color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, a.x, a.y, nullptr); LineTo(dc, b.x, b.y);
        const double angle = std::atan2(static_cast<double>(b.y - a.y),
                                        static_cast<double>(b.x - a.x));
        constexpr double spread = 0.55;
        const double length = 15.0 + arrow.width * 2.0;
        POINT p1{ b.x - static_cast<LONG>(std::cos(angle - spread) * length),
                  b.y - static_cast<LONG>(std::sin(angle - spread) * length) };
        POINT p2{ b.x - static_cast<LONG>(std::cos(angle + spread) * length),
                  b.y - static_cast<LONG>(std::sin(angle + spread) * length) };
        MoveToEx(dc, b.x, b.y, nullptr); LineTo(dc, p1.x, p1.y);
        MoveToEx(dc, b.x, b.y, nullptr); LineTo(dc, p2.x, p2.y);
        SelectObject(dc, oldPen); DeleteObject(pen);
    }
    for (const auto& mark : rectangles_) {
        POINT a = local(mark.start), b = local(mark.end);
        RECT rectangle{
            (std::min)(a.x, b.x), (std::min)(a.y, b.y),
            (std::max)(a.x, b.x), (std::max)(a.y, b.y)
        };
        HPEN pen = CreatePen(PS_SOLID, mark.width, mark.color);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
    }
    for (const auto& mark : texts_) {
        POINT p = local(mark.position);
        HFONT font = CreateFontW(-mark.size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, mark.fontName.c_str());
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, mark.color);
        RECT area{ p.x, p.y, p.x + 600, p.y + mark.size * 2 };
        DrawTextW(dc, mark.text.c_str(), -1, &area, DT_LEFT | DT_TOP | DT_SINGLELINE);
        SelectObject(dc, oldFont); DeleteObject(font);
    }
    if (!pendingText_.empty()) {
        POINT p = local(pendingTextPosition_);
        HFONT font = CreateFontW(-textSize_, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH,
            AnnotationFonts[textFontIndex_ % std::size(AnnotationFonts)]);
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, textColor_);
        RECT area{ p.x, p.y, p.x + 600, p.y + textSize_ * 2 };
        std::wstring preview = pendingText_ + L"|";
        DrawTextW(dc, preview.c_str(), -1, &area, DT_LEFT | DT_TOP | DT_SINGLELINE);
        SelectObject(dc, oldFont); DeleteObject(font);
    }
}

std::wstring ScreenshotTool::CreateSavePath() const {
    wchar_t overridePath[32768]{};
    DWORD length = GetEnvironmentVariableW(L"DESKTODO_CAPTURE_ROOT", overridePath,
                                             static_cast<DWORD>(std::size(overridePath)));
    std::filesystem::path folder;
    if (length > 0 && length < std::size(overridePath)) {
        folder = overridePath;
    } else {
        PWSTR pictures = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) {
            folder = pictures;
            CoTaskMemFree(pictures);
        } else {
            folder = GetAppDataPath();
        }
        folder /= L"DeskTodo Screenshots";
    }
    auto ensureFolder = [](const std::filesystem::path& candidate) {
        std::error_code error;
        std::filesystem::create_directories(candidate, error);
        return !error && std::filesystem::is_directory(candidate, error) && !error;
    };
    if (!ensureFolder(folder)) {
        folder = std::filesystem::path(GetAppDataPath()) / L"Screenshots";
    }
    if (!ensureFolder(folder)) {
        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        folder = std::filesystem::path(temporary) / L"DeskTodo Screenshots";
        ensureFolder(folder);
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[128]{};
    swprintf_s(name, L"Screenshot_%04u%02u%02u_%02u%02u%02u_%03u.png",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds);
    return (folder / name).wstring();
}

bool ScreenshotTool::SavePng(HBITMAP bitmap, const std::wstring& path) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* source = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateBitmapFromHBITMAP(bitmap, nullptr,
        WICBitmapIgnoreAlpha, &source);
    if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result)) result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(result)) result = frame->Initialize(properties);
    if (SUCCEEDED(result)) result = frame->WriteSource(source, nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    if (properties) properties->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (source) source->Release();
    if (factory) factory->Release();
    return SUCCEEDED(result);
}

bool ScreenshotTool::CopyToClipboard(HBITMAP bitmap) {
    HBITMAP copy = static_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, 0, 0,
                                                  LR_CREATEDIBSECTION));
    HWND owner = captureWindow_ ? captureWindow_ : pinWindow_;
    if (!copy || !OpenClipboard(owner)) {
        if (copy) DeleteObject(copy);
        return false;
    }
    EmptyClipboard();
    const bool bitmapSet = SetClipboardData(CF_BITMAP, copy) != nullptr;
    HGLOBAL dibData = nullptr;
    BITMAP details{};
    if (GetObjectW(bitmap, sizeof(details), &details) &&
        details.bmWidth > 0 && details.bmHeight > 0) {
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = details.bmWidth;
        header.biHeight = details.bmHeight;
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        const DWORD stride = static_cast<DWORD>(details.bmWidth) * 4;
        const DWORD imageSize = stride * static_cast<DWORD>(details.bmHeight);
        header.biSizeImage = imageSize;
        dibData = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imageSize);
        if (dibData) {
            void* memory = GlobalLock(dibData);
            if (memory) {
                memcpy(memory, &header, sizeof(header));
                BITMAPINFO info{};
                info.bmiHeader = header;
                HDC dc = GetDC(nullptr);
                const int copied = dc ? GetDIBits(dc, bitmap, 0,
                    static_cast<UINT>(details.bmHeight),
                    static_cast<BYTE*>(memory) + sizeof(BITMAPINFOHEADER),
                    &info, DIB_RGB_COLORS) : 0;
                if (dc) ReleaseDC(nullptr, dc);
                GlobalUnlock(dibData);
                if (copied != details.bmHeight) {
                    GlobalFree(dibData);
                    dibData = nullptr;
                }
            } else {
                GlobalFree(dibData);
                dibData = nullptr;
            }
        }
    }
    if (dibData) {
        const bool dibSet = SetClipboardData(CF_DIB, dibData) != nullptr;
        if (!dibSet) GlobalFree(dibData);
        CloseClipboard();
        if (!bitmapSet) DeleteObject(copy);
        return bitmapSet || dibSet;
    }
    CloseClipboard();
    if (!bitmapSet) DeleteObject(copy);
    return bitmapSet;
}

bool ScreenshotTool::SavePinnedAs() {
    if (!pinnedBitmap_) return false;
    std::wstring suggested = lastSavedPath_.empty() ? CreateSavePath() : lastSavedPath_;
    wchar_t path[32768]{};
    wcsncpy_s(path, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{ sizeof(dialog) };
    dialog.hwndOwner = pinWindow_;
    dialog.lpstrFilter = L"PNG 图像 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return CommDlgExtendedError() == 0;
    if (!SavePng(pinnedBitmap_, path)) {
        MessageBoxW(pinWindow_, L"无法保存截图，请选择其他文件夹后重试。",
                    L"DeskTodo 截图", MB_OK | MB_ICONERROR);
        return false;
    }
    lastSavedPath_ = path;
    lastSaveSucceeded_ = true;
    SetWindowTextW(pinWindow_, lastSavedPath_.c_str());
    return true;
}

bool ScreenshotTool::SaveBitmapAs(HBITMAP bitmap, HWND owner) {
    if (!bitmap) return false;
    const std::wstring suggested = CreateSavePath();
    wchar_t path[32768]{};
    wcsncpy_s(path, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{ sizeof(dialog) };
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"PNG 图像 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return false;
    if (!SavePng(bitmap, path)) {
        MessageBoxW(owner, L"无法保存截图，请选择其他文件夹后重试。",
                    L"DeskTodo 截图", MB_OK | MB_ICONERROR);
        return false;
    }
    lastSavedPath_ = path;
    lastSaveSucceeded_ = true;
    return true;
}

HBITMAP ScreenshotTool::CreateSelectionBitmap() {
    if (!CaptureRegion::IsValid(selection_) || !screenBitmap_) return nullptr;
    const RECT region = ToClientRect(selection_, virtualScreen_);
    const int width = region.right - region.left;
    const int height = region.bottom - region.top;
    HDC screen = GetDC(nullptr);
    HDC source = screen ? CreateCompatibleDC(screen) : nullptr;
    HDC target = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = screen ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    if (!screen || !source || !target || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (source) DeleteDC(source);
        if (target) DeleteDC(target);
        if (screen) ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HGDIOBJ oldSource = SelectObject(source, screenBitmap_);
    HGDIOBJ oldTarget = SelectObject(target, bitmap);
    const bool copied = BitBlt(target, 0, 0, width, height, source,
        region.left, region.top, SRCCOPY) == TRUE;
    if (copied) PaintAnnotations(target, { selection_.left, selection_.top });
    SelectObject(source, oldSource);
    SelectObject(target, oldTarget);
    DeleteDC(source);
    DeleteDC(target);
    ReleaseDC(nullptr, screen);
    if (!copied) { DeleteObject(bitmap); return nullptr; }
    return bitmap;
}

void ScreenshotTool::ExecuteCaptureAction(CaptureAction action) {
    if (action == CaptureAction::None) return;
    if (action == CaptureAction::Cancel) { CancelCapture(); return; }
    if (action == CaptureAction::Move || action == CaptureAction::Arrow ||
        action == CaptureAction::Text || action == CaptureAction::Rectangle) {
        activeTool_ = action;
        pendingText_.clear();
        InvalidateRect(captureWindow_, nullptr, FALSE);
        return;
    }
    if (action == CaptureAction::Pen) {
        activeTool_ = CaptureAction::Pen;
        InvalidateRect(captureWindow_, nullptr, FALSE);
        return;
    }
    if (action == CaptureAction::Clear) {
        strokes_.clear(); arrows_.clear(); texts_.clear(); rectangles_.clear();
        pendingText_.clear();
        InvalidateRect(captureWindow_, nullptr, FALSE);
        return;
    }
    HBITMAP bitmap = CreateSelectionBitmap();
    if (!bitmap) {
        MessageBoxW(captureWindow_, L"无法读取选区内容，请重新选择。",
                    L"DeskTodo 截图", MB_OK | MB_ICONERROR);
        return;
    }
    const RECT sourceRegion = selection_;
    if (action == CaptureAction::Pin) {
        lastSavedPath_.clear();
        lastSaveSucceeded_ = false;
        CancelCapture();
        ShowPinnedBitmap(bitmap, sourceRegion);
        return;
    }
    if (action == CaptureAction::Save) {
        lastSavedPath_ = CreateSavePath();
        lastSaveSucceeded_ = SavePng(bitmap, lastSavedPath_);
        DeleteObject(bitmap);
        if (lastSaveSucceeded_) CancelCapture();
        else MessageBoxW(captureWindow_, L"无法保存截图，请检查图片文件夹权限。",
                         L"DeskTodo 截图", MB_OK | MB_ICONERROR);
        return;
    }
    if (action == CaptureAction::Copy) {
        const bool copied = CopyToClipboard(bitmap);
        DeleteObject(bitmap);
        if (copied) CancelCapture();
        else MessageBoxW(captureWindow_, L"无法复制截图到剪贴板。",
                         L"DeskTodo 截图", MB_OK | MB_ICONERROR);
        return;
    }
    if (action == CaptureAction::Ocr || action == CaptureAction::LatexOcr) {
        std::wstring text;
        std::wstring error;
        const bool latex = action == CaptureAction::LatexOcr;
        const bool recognized = latex
            ? OcrService::RecognizeLatexBitmap(bitmap, text, error)
            : OcrService::RecognizeBitmap(bitmap, text, error);
        DeleteObject(bitmap);
        if (!recognized) {
            MessageBoxW(captureWindow_, error.c_str(),
                        latex ? L"DeskTodo LaTeX OCR" : L"DeskTodo OCR",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        CopyText(captureWindow_, text);
        CancelCapture();
        OcrResultWindow::Show(nullptr, text,
            latex ? L"LaTeX 识别结果" : L"OCR 识别结果");
    }
}

void ScreenshotTool::CompleteCapture() {
    if (!CaptureRegion::IsValid(selection_)) { CancelCapture(); return; }
    const RECT clientRegion = ToClientRect(selection_, virtualScreen_);
    const int width = clientRegion.right - clientRegion.left;
    const int height = clientRegion.bottom - clientRegion.top;
    HDC screen = GetDC(nullptr);
    HDC source = CreateCompatibleDC(screen);
    HDC target = CreateCompatibleDC(screen);
    HBITMAP cropped = CreateCompatibleBitmap(screen, width, height);
    if (!screen || !source || !target || !cropped) {
        if (cropped) DeleteObject(cropped);
        if (source) DeleteDC(source);
        if (target) DeleteDC(target);
        if (screen) ReleaseDC(nullptr, screen);
        CancelCapture();
        MessageBoxW(nullptr, L"无法创建截图位图，请重试。", L"DeskTodo 截图",
                    MB_OK | MB_ICONERROR);
        return;
    }
    HGDIOBJ oldSource = SelectObject(source, screenBitmap_);
    HGDIOBJ oldTarget = SelectObject(target, cropped);
    const bool copied = BitBlt(target, 0, 0, width, height, source,
        clientRegion.left, clientRegion.top, SRCCOPY) == TRUE;
    SelectObject(source, oldSource);
    SelectObject(target, oldTarget);
    DeleteDC(source);
    DeleteDC(target);
    ReleaseDC(nullptr, screen);
    if (!copied) {
        DeleteObject(cropped);
        CancelCapture();
        MessageBoxW(nullptr, L"读取屏幕内容失败，请重试。", L"DeskTodo 截图",
                    MB_OK | MB_ICONERROR);
        return;
    }
    CopyToClipboard(cropped);
    lastSavedPath_ = CreateSavePath();
    lastSaveSucceeded_ = SavePng(cropped, lastSavedPath_);
    const RECT sourceRegion = selection_;
    CancelCapture();
    ShowPinnedBitmap(cropped, sourceRegion);
    if (!lastSaveSucceeded_) {
        MessageBoxW(pinWindow_, L"自动保存失败，截图仍保留在贴图中。\n右键贴图可选择“另存为”。",
                    L"DeskTodo 截图", MB_OK | MB_ICONWARNING);
    }
}

void ScreenshotTool::ShowPinnedBitmap(HBITMAP bitmap, const RECT& sourceRegion) {
    if (pinWindow_) DestroyWindow(pinWindow_);
    if (pinnedBitmap_) DeleteObject(pinnedBitmap_);
    pinnedBitmap_ = bitmap;
    BITMAP details{};
    GetObjectW(bitmap, sizeof(details), &details);
    pinnedSourceSize_ = { details.bmWidth, details.bmHeight };
    const SIZE preview = CaptureRegion::FitPreview(
        { 0, 0, details.bmWidth, details.bmHeight });
    pinScale_ = static_cast<double>(preview.cx) / details.bmWidth;
    const wchar_t* title = lastSaveSucceeded_
        ? lastSavedPath_.c_str() : L"截图贴图（未保存，右键另存为）";
    pinWindow_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        PinClass, title, WS_POPUP,
        sourceRegion.left, sourceRegion.top, preview.cx, preview.cy,
        nullptr, nullptr, instance_, this);
    if (pinWindow_) {
        ShowWindow(pinWindow_, SW_SHOW);
        SetForegroundWindow(pinWindow_);
    }
}

void ScreenshotTool::PaintPin(HDC dc) {
    RECT client{};
    GetClientRect(pinWindow_, &client);
    HDC source = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(source, pinnedBitmap_);
    SetStretchBltMode(dc, HALFTONE);
    StretchBlt(dc, 0, 0, client.right, client.bottom, source,
               0, 0, pinnedSourceSize_.cx, pinnedSourceSize_.cy, SRCCOPY);
    SelectObject(source, old);
    DeleteDC(source);
    RECT close{ client.right - 26, 6, client.right - 6, 26 };
    FillArea(dc, close, RGB(30, 33, 41));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(245, 246, 248));
    DrawTextW(dc, L"×", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void ScreenshotTool::ResizePin(double scale) {
    if (!pinWindow_ || !pinnedBitmap_ || pinnedSourceSize_.cx <= 0 ||
        pinnedSourceSize_.cy <= 0) {
        return;
    }
    pinScale_ = (std::clamp)(scale, 0.2, 3.0);
    const int width = (std::max)(80, static_cast<int>(pinnedSourceSize_.cx * pinScale_));
    const int height = (std::max)(60, static_cast<int>(pinnedSourceSize_.cy * pinScale_));
    SetWindowPos(pinWindow_, HWND_TOPMOST, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOACTIVATE);
    InvalidateRect(pinWindow_, nullptr, FALSE);
}

LRESULT CALLBACK ScreenshotTool::CaptureWindowProc(HWND window, UINT message,
                                                    WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ScreenshotTool*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ScreenshotTool*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->captureWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->OnCaptureMessage(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}

LRESULT ScreenshotTool::OnCaptureMessage(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, selectionReady_ && !selecting_ ? IDC_ARROW : IDC_CROSS));
        return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(captureWindow_, &paint);
        PaintCapture(dc);
        EndPaint(captureWindow_, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT screenPoint{ GET_X_LPARAM(lp) + virtualScreen_.left,
                           GET_Y_LPARAM(lp) + virtualScreen_.top };
        {
            std::wstringstream stream;
            stream << L"CaptureMouseDown: x=" << screenPoint.x << L" y=" << screenPoint.y
                   << L" ready=" << (selectionReady_ ? 1 : 0);
            CaptureDiag(stream.str());
        }
        if (selectionReady_) {
            if (HandlePropertyClick(screenPoint)) return 0;
            const CaptureAction action = CaptureToolbar::HitTest(
                CaptureToolbar::Build(selection_, virtualScreen_), screenPoint);
            {
                std::wstringstream stream;
                stream << L"CaptureToolbarHit: action=" << static_cast<int>(action);
                CaptureDiag(stream.str());
            }
            if (action != CaptureAction::None) {
                clickedHintAction_ = action;
                KillTimer(captureWindow_, CaptureHintTimerId);
                SetTimer(captureWindow_, CaptureHintTimerId, 1400, nullptr);
                InvalidateRect(captureWindow_, nullptr, FALSE);
                ExecuteCaptureAction(action);
                return 0;
            }
            if (PtInRect(&selection_, screenPoint)) {
                actionStart_ = screenPoint;
                if (activeTool_ == CaptureAction::Move) {
                    movingSelection_ = true;
                    moveStartSelection_ = selection_;
                    SetCapture(captureWindow_);
                    return 0;
                }
                if (activeTool_ == CaptureAction::Pen) {
                    strokes_.push_back({ { screenPoint }, penColor_, penWidth_ });
                    annotating_ = true; SetCapture(captureWindow_); return 0;
                }
                if (activeTool_ == CaptureAction::Arrow) {
                    arrows_.push_back({ screenPoint, screenPoint, arrowColor_, arrowWidth_ });
                    annotating_ = true; SetCapture(captureWindow_); return 0;
                }
                if (activeTool_ == CaptureAction::Rectangle) {
                    rectangles_.push_back({ screenPoint, screenPoint,
                                            rectangleColor_, rectangleWidth_ });
                    annotating_ = true; SetCapture(captureWindow_); return 0;
                }
                if (activeTool_ == CaptureAction::Text) {
                    pendingTextPosition_ = screenPoint;
                    pendingText_.clear();
                    SetFocus(captureWindow_);
                    return 0;
                }
            }
        }
        selecting_ = true;
        selectionReady_ = false;
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        SetCapture(captureWindow_);
        selectionStart_ = screenPoint;
        selection_ = { selectionStart_.x, selectionStart_.y,
                       selectionStart_.x, selectionStart_.y };
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT hoverPoint{ GET_X_LPARAM(lp) + virtualScreen_.left,
                          GET_Y_LPARAM(lp) + virtualScreen_.top };
        CaptureAction hoverAction = CaptureAction::None;
        if (selectionReady_ && !selecting_ && !movingSelection_ && !annotating_) {
            hoverAction = CaptureToolbar::HitTest(
                CaptureToolbar::Build(selection_, virtualScreen_), hoverPoint);
        }
        if (hoverAction != hoveredHintAction_) {
            hoveredHintAction_ = hoverAction;
            InvalidateRect(captureWindow_, nullptr, FALSE);
        }
        if (movingSelection_) {
            POINT current{ GET_X_LPARAM(lp) + virtualScreen_.left,
                           GET_Y_LPARAM(lp) + virtualScreen_.top };
            const LONG dx = current.x - actionStart_.x;
            const LONG dy = current.y - actionStart_.y;
            selection_ = moveStartSelection_;
            OffsetRect(&selection_, dx, dy);
            const LONG fixX = selection_.left < virtualScreen_.left ? virtualScreen_.left - selection_.left :
                (selection_.right > virtualScreen_.right ? virtualScreen_.right - selection_.right : 0);
            const LONG fixY = selection_.top < virtualScreen_.top ? virtualScreen_.top - selection_.top :
                (selection_.bottom > virtualScreen_.bottom ? virtualScreen_.bottom - selection_.bottom : 0);
            OffsetRect(&selection_, fixX, fixY);
            InvalidateRect(captureWindow_, nullptr, FALSE);
            return 0;
        }
        if (annotating_) {
            POINT current{ GET_X_LPARAM(lp) + virtualScreen_.left,
                           GET_Y_LPARAM(lp) + virtualScreen_.top };
            current.x = (std::clamp)(current.x, selection_.left, selection_.right - 1);
            current.y = (std::clamp)(current.y, selection_.top, selection_.bottom - 1);
            if (activeTool_ == CaptureAction::Pen && !strokes_.empty())
                strokes_.back().points.push_back(current);
            else if (activeTool_ == CaptureAction::Arrow && !arrows_.empty())
                arrows_.back().end = current;
            else if (activeTool_ == CaptureAction::Rectangle && !rectangles_.empty())
                rectangles_.back().end = current;
            InvalidateRect(captureWindow_, nullptr, FALSE);
            return 0;
        }
        if (selecting_) {
            POINT current{ GET_X_LPARAM(lp) + virtualScreen_.left,
                           GET_Y_LPARAM(lp) + virtualScreen_.top };
            selection_ = CaptureRegion::Normalize(selectionStart_, current);
            InvalidateRect(captureWindow_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (movingSelection_ || annotating_) {
            if (movingSelection_) {
                const LONG dx = selection_.left - moveStartSelection_.left;
                const LONG dy = selection_.top - moveStartSelection_.top;
                for (auto& stroke : strokes_) for (auto& p : stroke.points) { p.x += dx; p.y += dy; }
                for (auto& arrow : arrows_) {
                    arrow.start.x += dx; arrow.start.y += dy;
                    arrow.end.x += dx; arrow.end.y += dy;
                }
                for (auto& rectangle : rectangles_) {
                    rectangle.start.x += dx; rectangle.start.y += dy;
                    rectangle.end.x += dx; rectangle.end.y += dy;
                }
                for (auto& text : texts_) { text.position.x += dx; text.position.y += dy; }
            }
            movingSelection_ = false;
            annotating_ = false;
            if (GetCapture() == captureWindow_) ReleaseCapture();
            InvalidateRect(captureWindow_, nullptr, FALSE);
            return 0;
        }
        if (selecting_) {
            selecting_ = false;
            if (GetCapture() == captureWindow_) ReleaseCapture();
            POINT current{ GET_X_LPARAM(lp) + virtualScreen_.left,
                           GET_Y_LPARAM(lp) + virtualScreen_.top };
            selection_ = CaptureRegion::Normalize(selectionStart_, current);
            selectionReady_ = CaptureRegion::IsValid(selection_);
            if (selectionReady_) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            {
                std::wstringstream stream;
                stream << L"CaptureMouseUp: left=" << selection_.left
                       << L" top=" << selection_.top
                       << L" right=" << selection_.right
                       << L" bottom=" << selection_.bottom
                       << L" ready=" << (selectionReady_ ? 1 : 0);
                CaptureDiag(stream.str());
            }
            InvalidateRect(captureWindow_, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { CancelCapture(); return 0; }
        if (selectionReady_ && IsControlPressed()) {
            if (wp == 'C') { ExecuteCaptureAction(CaptureAction::Copy); return 0; }
            if (wp == 'V') { ExecuteCaptureAction(CaptureAction::Pin); return 0; }
            if (wp == 'S') { ExecuteCaptureAction(CaptureAction::Save); return 0; }
        }
        break;
    case WM_CHAR:
        if (activeTool_ == CaptureAction::Text && selectionReady_) {
            if (wp == VK_RETURN) {
                if (!pendingText_.empty()) {
                    texts_.push_back({ pendingTextPosition_, pendingText_,
                        AnnotationFonts[textFontIndex_ % std::size(AnnotationFonts)],
                        textColor_, textSize_ });
                    pendingText_.clear();
                }
            } else if (wp == VK_BACK) {
                if (!pendingText_.empty()) pendingText_.pop_back();
            } else if (wp >= 32) {
                pendingText_.push_back(static_cast<wchar_t>(wp));
            }
            InvalidateRect(captureWindow_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_RBUTTONUP:
        CancelCapture();
        return 0;
    case WM_TIMER:
        if (wp == CaptureHintTimerId) {
            KillTimer(captureWindow_, CaptureHintTimerId);
            clickedHintAction_ = CaptureAction::None;
            InvalidateRect(captureWindow_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_CLOSE:
        CancelCapture();
        return 0;
    case WM_DESTROY:
        captureWindow_ = nullptr;
        return 0;
    }
    return DefWindowProcW(captureWindow_, message, wp, lp);
}

LRESULT CALLBACK ScreenshotTool::PinWindowProc(HWND window, UINT message,
                                                WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ScreenshotTool*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ScreenshotTool*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->pinWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->OnPinMessage(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}

LRESULT ScreenshotTool::OnPinMessage(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(pinWindow_, &paint);
        PaintPin(dc);
        EndPaint(pinWindow_, &paint);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT client{};
        GetClientRect(pinWindow_, &client);
        POINT point{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (point.x >= client.right - 30 && point.y <= 30) {
            DestroyWindow(pinWindow_);
            return 0;
        }
        pinDragging_ = true;
        GetPhysicalCursorPos(&pinDragStart_);
        GetWindowRect(pinWindow_, &pinDragRect_);
        SetCapture(pinWindow_);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (pinDragging_) {
            POINT cursor{};
            GetPhysicalCursorPos(&cursor);
            SetWindowPos(pinWindow_, HWND_TOPMOST,
                pinDragRect_.left + cursor.x - pinDragStart_.x,
                pinDragRect_.top + cursor.y - pinDragStart_.y,
                0, 0, SWP_NOSIZE);
        }
        return 0;
    case WM_LBUTTONUP:
        pinDragging_ = false;
        if (GetCapture() == pinWindow_) ReleaseCapture();
        return 0;
    case WM_LBUTTONDBLCLK:
    case WM_CLOSE:
        DestroyWindow(pinWindow_);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(pinWindow_); return 0; }
        if (IsControlPressed()) {
            if (wp == 'C') { CopyToClipboard(pinnedBitmap_); return 0; }
            if (wp == 'S') { SavePinnedAs(); return 0; }
        }
        break;
    case WM_RBUTTONUP: {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"另存为...");
        AppendMenuW(menu, MF_STRING, 2, L"复制到剪贴板");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"关闭贴图");
        POINT point{};
        GetPhysicalCursorPos(&point);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                             point.x, point.y, 0, pinWindow_, nullptr);
        DestroyMenu(menu);
        if (command == 1) SavePinnedAs();
        else if (command == 2) CopyToClipboard(pinnedBitmap_);
        else if (command == 3) DestroyWindow(pinWindow_);
        return 0;
    }
    case WM_MOUSEWHEEL:
        ResizePin(pinScale_ * (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.1 : 0.9));
        return 0;
    case WM_DESTROY:
        pinWindow_ = nullptr;
        if (pinnedBitmap_) { DeleteObject(pinnedBitmap_); pinnedBitmap_ = nullptr; }
        return 0;
    }
    return DefWindowProcW(pinWindow_, message, wp, lp);
}
