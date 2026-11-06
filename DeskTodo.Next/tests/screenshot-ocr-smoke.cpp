#include "Window/ScreenshotTool.h"
#include "Window/OcrResultWindow.h"
#include "Services/OcrService.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
std::atomic<int> calls{0};
int resultsShown = 0;
std::wstring shownText;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

// Deliberately slow backend: these checks exercise window/thread lifetime and
// cancellation independently of the real-model accuracy tests in ocr-smoke.
bool OcrService::RecognizeBitmap(HBITMAP, std::wstring& text, std::wstring&, Backend) {
    ++calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    text = L"文档";
    return true;
}
bool OcrService::RecognizeLatexBitmap(HBITMAP bitmap, std::wstring& text, std::wstring& error) {
    bool success = RecognizeBitmap(bitmap, text, error);
    text = L"\\[" + text + L"\\]";
    return success;
}
void OcrResultWindow::Show(HWND, const std::wstring& text, const std::wstring&) {
    ++resultsShown;
    shownText = text;
}

struct ScreenshotOcrTestAccess {
    static void Prepare(ScreenshotTool& tool) {
        Require(tool.RegisterClasses(), "register capture class");
        tool.virtualScreen_ = {0, 0, 65, 43};
        tool.selection_ = tool.virtualScreen_;
        tool.selectionReady_ = true;
        HDC screen = GetDC(nullptr);
        tool.screenBitmap_ = CreateCompatibleBitmap(screen, 65, 43);
        ReleaseDC(nullptr, screen);
        Require(tool.screenBitmap_ != nullptr, "create capture bitmap");
        tool.captureWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"DeskTodoCaptureOverlay", L"OCR smoke",
                                             WS_POPUP, 0, 0, 65, 43, nullptr, nullptr, tool.instance_, &tool);
        Require(tool.captureWindow_ != nullptr, "create hidden capture window");
    }
    static void Start(ScreenshotTool& tool, CaptureAction action = CaptureAction::Ocr) { tool.ExecuteCaptureAction(action); }
    static void Cancel(ScreenshotTool& tool) { tool.CancelCapture(); }
    static bool Pending(ScreenshotTool& tool) { return tool.ocrWork_ != nullptr; }
    static void Pump(ScreenshotTool& tool) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (Pending(tool) && std::chrono::steady_clock::now() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Require(!Pending(tool), "OCR completion timer did not deliver the result");
    }
};

int main() {
    try {
        {
            ScreenshotTool tool(GetModuleHandleW(nullptr));
            ScreenshotOcrTestAccess::Prepare(tool);
            auto started = std::chrono::steady_clock::now();
            ScreenshotOcrTestAccess::Start(tool);
            Require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100),
                    "OCR blocked the capture UI");
            ScreenshotOcrTestAccess::Start(tool);
            ScreenshotOcrTestAccess::Pump(tool);
            Require(calls == 1 && resultsShown == 1 && shownText == L"文档", "duplicate or missing OCR result");
        }
        {
            ScreenshotTool tool(GetModuleHandleW(nullptr));
            ScreenshotOcrTestAccess::Prepare(tool);
            ScreenshotOcrTestAccess::Start(tool);
            ScreenshotOcrTestAccess::Cancel(tool);
            // Destruction must join the worker before freeing its result.
        }
        Require(calls == 2 && resultsShown == 1, "cancelled OCR showed a result");
        {
            ScreenshotTool tool(GetModuleHandleW(nullptr));
            ScreenshotOcrTestAccess::Prepare(tool);
            ScreenshotOcrTestAccess::Start(tool, CaptureAction::LatexOcr);
            ScreenshotOcrTestAccess::Pump(tool);
            Require(resultsShown == 2 && shownText == L"\\[文档\\]", "LaTeX worker result");
        }
        std::cout << "Screenshot OCR passed: responsive UI, duplicate suppression, completion, cancellation, teardown, LaTeX.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
