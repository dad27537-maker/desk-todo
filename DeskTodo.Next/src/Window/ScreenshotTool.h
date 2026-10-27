#pragma once

#include <Windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <thread>

#include "Models/CaptureToolbar.h"

struct ScreenshotHotkeyConfig {
    UINT virtualKey = 'A';
    BYTE flags = HOTKEYF_CONTROL | HOTKEYF_SHIFT;
};

ScreenshotHotkeyConfig LoadScreenshotHotkeyConfig();
bool SaveScreenshotHotkeyConfig(const ScreenshotHotkeyConfig& config);
UINT ScreenshotHotkeyModifiers(const ScreenshotHotkeyConfig& config);

class ScreenshotTool {
public:
    explicit ScreenshotTool(HINSTANCE instance);
    ~ScreenshotTool();

    bool StartCapture();
    void Destroy();

private:
    friend struct ScreenshotOcrTestAccess;
    struct OcrWork {
        std::atomic<bool> complete{false};
        bool latex = false;
        bool success = false;
        std::wstring text, error;
    };
    std::unique_ptr<OcrWork> ocrWork_;
    std::thread ocrThread_;
    void FinishOcr();
    struct Stroke { std::vector<POINT> points; COLORREF color{}; int width = 3; };
    struct ArrowMark { POINT start{}; POINT end{}; COLORREF color{}; int width = 3; };
    struct TextMark {
        POINT position{};
        std::wstring text;
        std::wstring fontName = L"Microsoft YaHei UI";
        COLORREF color{};
        int size = 24;
    };
    struct RectangleMark { POINT start{}; POINT end{}; COLORREF color{}; int width = 3; };
    HINSTANCE instance_ = nullptr;
    HWND captureWindow_ = nullptr;
    HWND pinWindow_ = nullptr;
    HBITMAP screenBitmap_ = nullptr;
    HBITMAP pinnedBitmap_ = nullptr;
    HDC captureBufferDc_ = nullptr;
    HBITMAP captureBufferBitmap_ = nullptr;
    HGDIOBJ captureBufferOld_ = nullptr;
    SIZE captureBufferSize_{};
    RECT virtualScreen_{};
    RECT selection_{};
    POINT selectionStart_{};
    POINT cursorPosition_{};
    bool cursorPositionValid_ = false;
    bool selecting_ = false;
    bool selectionReady_ = false;
    bool pinDragging_ = false;
    POINT pinDragStart_{};
    RECT pinDragRect_{};
    SIZE pinnedSourceSize_{};
    double pinScale_ = 1.0;
    std::wstring lastSavedPath_;
    bool lastSaveSucceeded_ = false;
    CaptureAction activeTool_ = CaptureAction::Move;
    std::vector<Stroke> strokes_;
    std::vector<ArrowMark> arrows_;
    std::vector<TextMark> texts_;
    std::vector<RectangleMark> rectangles_;
    bool annotating_ = false;
    bool movingSelection_ = false;
    POINT actionStart_{};
    RECT moveStartSelection_{};
    std::wstring pendingText_;
    POINT pendingTextPosition_{};
    COLORREF penColor_ = RGB(234, 82, 92);
    int penWidth_ = 4;
    COLORREF arrowColor_ = RGB(234, 82, 92);
    int arrowWidth_ = 4;
    COLORREF rectangleColor_ = RGB(234, 82, 92);
    int rectangleWidth_ = 4;
    COLORREF textColor_ = RGB(234, 82, 92);
    int textSize_ = 24;
    int textFontIndex_ = 0;
    CaptureAction clickedHintAction_ = CaptureAction::None;
    CaptureAction hoveredHintAction_ = CaptureAction::None;

    bool RegisterClasses();
    bool CaptureVirtualScreen();
    bool EnsureCaptureBuffer(HDC reference, int width, int height);
    void DestroyCaptureBuffer();
    void CancelCapture();
    void CompleteCapture();
    void PaintCapture(HDC dc);
    void PaintCaptureToolbar(HDC dc);
    void PaintCaptureProperties(HDC dc);
    void PaintCaptureHint(HDC dc);
    void PaintMagnifier(HDC dc);
    void PaintAnnotations(HDC dc, POINT origin = {});
    void PaintPin(HDC dc);
    HBITMAP CreateSelectionBitmap();
    void ExecuteCaptureAction(CaptureAction action);
    bool HandlePropertyClick(POINT screenPoint);
    bool IsStyleToolActive() const;
    bool CopyToClipboard(HBITMAP bitmap);
    bool SavePng(HBITMAP bitmap, const std::wstring& path);
    bool SaveBitmapAs(HBITMAP bitmap, HWND owner);
    bool SavePinnedAs();
    std::wstring CreateSavePath() const;
    void ShowPinnedBitmap(HBITMAP bitmap, const RECT& sourceRegion);
    void ResizePin(double scale);

    static LRESULT CALLBACK CaptureWindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PinWindowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnCaptureMessage(UINT, WPARAM, LPARAM);
    LRESULT OnPinMessage(UINT, WPARAM, LPARAM);
};
