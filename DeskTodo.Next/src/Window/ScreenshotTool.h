#pragma once

#include <Windows.h>
#include <string>
#include <vector>

#include "Models/CaptureToolbar.h"

class ScreenshotTool {
public:
    explicit ScreenshotTool(HINSTANCE instance);
    ~ScreenshotTool();

    bool StartCapture();
    void Destroy();

private:
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
    struct PinnedImage {
        HWND window = nullptr;
        HBITMAP bitmap = nullptr;
        SIZE sourceSize{};
        double scale = 1.0;
        std::wstring savedPath;
        bool alwaysOnTop = true;
        bool locked = false;
        bool dragging = false;
        POINT dragStart{};
        RECT dragRect{};
    };
    HINSTANCE instance_ = nullptr;
    HWND captureWindow_ = nullptr;
    HBITMAP screenBitmap_ = nullptr;
    HDC captureBufferDc_ = nullptr;
    HBITMAP captureBufferBitmap_ = nullptr;
    HGDIOBJ captureBufferOld_ = nullptr;
    SIZE captureBufferSize_{};
    RECT virtualScreen_{};
    RECT selection_{};
    POINT selectionStart_{};
    bool selecting_ = false;
    bool selectionReady_ = false;
    std::vector<PinnedImage> pinnedImages_;
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
    void PaintAnnotations(HDC dc, POINT origin = {});
    void PaintPin(HWND window, HDC dc);
    HBITMAP CreateSelectionBitmap();
    void ExecuteCaptureAction(CaptureAction action);
    bool HandlePropertyClick(POINT screenPoint);
    bool IsStyleToolActive() const;
    bool CopyToClipboard(HBITMAP bitmap, HWND owner = nullptr);
    bool SavePng(HBITMAP bitmap, const std::wstring& path);
    bool SaveBitmapAs(HBITMAP bitmap, HWND owner);
    bool SavePinnedAs(HWND window);
    std::wstring CreateSavePath() const;
    void ShowPinnedBitmap(HBITMAP bitmap, const RECT& sourceRegion);
    void ResizePin(HWND window, double scale);
    PinnedImage* FindPinnedImage(HWND window);
    void SetPinAlwaysOnTop(HWND window, bool enabled);
    void SetPinLocked(HWND window, bool locked);

    static LRESULT CALLBACK CaptureWindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PinWindowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnCaptureMessage(UINT, WPARAM, LPARAM);
    LRESULT OnPinMessage(HWND window, UINT, WPARAM, LPARAM);
};
