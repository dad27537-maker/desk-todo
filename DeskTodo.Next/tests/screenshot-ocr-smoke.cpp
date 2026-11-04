#include "Window/ScreenshotTool.h"
#include "Window/OcrResultWindow.h"
#include "Window/TranslationWindow.h"
#include "Services/OcrService.h"
#include "Models/CaptureRegion.h"
#include <commdlg.h>
#include <dlgs.h>
#include <objbase.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
std::atomic<int> calls{0};
int resultsShown = 0;
std::wstring shownText;
int translationsShown = 0;
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
void TranslationWindow::Show(HWND, HBITMAP bitmap) {
    BITMAP details{};
    Require(GetObjectW(bitmap, sizeof(details), &details) && details.bmWidth == 65 &&
            details.bmHeight == 43, "translation receives the selected screenshot");
    Require(FindWindowW(L"DeskTodoCaptureOverlay", nullptr) == nullptr,
            "capture overlay closes before the translation window opens");
    DeleteObject(bitmap);
    ++translationsShown;
}

struct ScreenshotOcrTestAccess {
    static void Prepare(ScreenshotTool& tool, RECT bounds = {0, 0, 65, 43}) {
        Require(tool.RegisterClasses(), "register capture class");
        tool.virtualScreen_ = bounds;
        tool.selection_ = tool.virtualScreen_;
        tool.selectionReady_ = true;
        HDC screen = GetDC(nullptr);
        tool.screenBitmap_ = CreateCompatibleBitmap(screen, bounds.right - bounds.left, bounds.bottom - bounds.top);
        ReleaseDC(nullptr, screen);
        Require(tool.screenBitmap_ != nullptr, "create capture bitmap");
        tool.captureWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"DeskTodoCaptureOverlay", L"OCR smoke",
                                             WS_POPUP, bounds.left, bounds.top, bounds.right - bounds.left,
                                             bounds.bottom - bounds.top, nullptr, nullptr, tool.instance_, &tool);
        Require(tool.captureWindow_ != nullptr, "create hidden capture window");
    }
    static void Start(ScreenshotTool& tool, CaptureAction action = CaptureAction::Ocr) { tool.ExecuteCaptureAction(action); }
    static void Cancel(ScreenshotTool& tool) { tool.CancelCapture(); }
    static bool Pending(ScreenshotTool& tool) { return tool.ocrWork_ != nullptr; }
    static void Mouse(ScreenshotTool& tool, UINT message, LONG x, LONG y) {
        SendMessageW(tool.captureWindow_, message, 0,
            MAKELPARAM(x - tool.virtualScreen_.left, y - tool.virtualScreen_.top));
    }
    static void CheckInitialSelection() {
        ScreenshotTool tool(GetModuleHandleW(nullptr));
        Prepare(tool, {-500, -200, 1000, 800});
        tool.selection_ = {};
        tool.selectionReady_ = false;
        // StartCapture already owns capture before the first mouse press.
        SetCapture(tool.captureWindow_);
        Mouse(tool, WM_LBUTTONDOWN, 100, 100);
        Require(tool.selecting_ && !tool.selectionReady_, "first press must start selection while capture is already owned");
        Mouse(tool, WM_MOUSEMOVE, 400, 300);
        Require(tool.selecting_ && tool.selection_.right == 400 && tool.selection_.bottom == 300,
                "first drag must update the initial region");
        Mouse(tool, WM_LBUTTONUP, 450, 350);
        Require(tool.selectionReady_ && !tool.selecting_ && tool.selection_.left == 100 &&
                tool.selection_.top == 100 && tool.selection_.right == 450 && tool.selection_.bottom == 350,
                "first release must finish selection without requiring a second drag");
    }
    static void CheckResize() {
        ScreenshotTool tool(GetModuleHandleW(nullptr));
        Prepare(tool, {-500, -200, 1000, 800});
        const RECT initial{100, 100, 400, 300};
        const POINT handles[] = {{100, 100}, {250, 100}, {400, 100}, {100, 200},
                                 {400, 200}, {100, 300}, {250, 300}, {400, 300}};
        const unsigned edges[] = {5, 4, 6, 1, 2, 9, 8, 10};
        for (size_t index = 0; index < std::size(handles); ++index) {
            tool.selection_ = initial;
            tool.activeTool_ = CaptureAction::Pen;
            const auto point = handles[index];
            Mouse(tool, WM_LBUTTONDOWN, point.x, point.y);
            Require(tool.resizingEdges_ == edges[index] && !tool.annotating_, "edge drag starts resize in annotation mode");
            Mouse(tool, WM_MOUSEMOVE, point.x + 30, point.y + 20);
            const RECT expected = CaptureRegion::Resize(initial, edges[index], {30, 20}, tool.virtualScreen_);
            Require(EqualRect(&tool.selection_, &expected), "edge drag updates the correct sides");
            Mouse(tool, WM_LBUTTONUP, point.x + 40, point.y + 25);
            const RECT released = CaptureRegion::Resize(initial, edges[index], {40, 25}, tool.virtualScreen_);
            Require(EqualRect(&tool.selection_, &released) && tool.selectionReady_ && !tool.resizingEdges_,
                    "release uses final position and preserves a ready selection");
        }
        tool.selection_ = initial;
        Mouse(tool, WM_LBUTTONDOWN, 100, 100);
        Mouse(tool, WM_MOUSEMOVE, -2000, -2000);
        Require(tool.selection_.left == -500 && tool.selection_.top == -200, "resize clamps to negative monitor bounds");
        Mouse(tool, WM_MOUSEMOVE, 1000, 1000);
        Require(tool.selection_.left == 394 && tool.selection_.top == 294, "resize cannot invert or shrink below minimum");
        ReleaseCapture();
        Require(!tool.resizingEdges_ && tool.selectionReady_, "lost mouse capture ends resize");

        tool.selection_ = initial;
        tool.activeTool_ = CaptureAction::Move;
        tool.texts_.push_back({{150, 150}, L"keep annotation"});
        Mouse(tool, WM_LBUTTONDOWN, 200, 200);
        Mouse(tool, WM_MOUSEMOVE, 220, 230);
        Mouse(tool, WM_LBUTTONUP, 220, 230);
        Require(tool.selection_.left == 120 && tool.selection_.top == 130 &&
                tool.texts_.front().position.x == 170 && tool.texts_.front().position.y == 180,
                "moving selection still moves annotations");
        Mouse(tool, WM_LBUTTONDOWN, 420, 330);
        Mouse(tool, WM_LBUTTONUP, 450, 360);
        Require(tool.texts_.front().position.x == 170 && tool.texts_.front().position.y == 180,
                "resize keeps annotation coordinates unchanged");
        HBITMAP cropped = tool.CreateSelectionBitmap();
        BITMAP details{};
        Require(cropped && GetObjectW(cropped, sizeof(details), &details) &&
                details.bmWidth == 330 && details.bmHeight == 230, "export uses resized selection dimensions");
        DeleteObject(cropped);
        tool.selection_ = initial;
        tool.texts_.clear();
        tool.texts_.push_back({{150, 150}, L"release position"});
        Mouse(tool, WM_LBUTTONDOWN, 200, 200);
        Mouse(tool, WM_MOUSEMOVE, 220, 230);
        Mouse(tool, WM_LBUTTONUP, 240, 250);
        Require(tool.selection_.left == 140 && tool.selection_.top == 150 &&
                tool.texts_.front().position.x == 190 && tool.texts_.front().position.y == 200,
                "move release commits the latest pointer position and annotation offset");
    }
    static void CheckSaveDialog(const std::filesystem::path& root) {
        ScreenshotTool tool(GetModuleHandleW(nullptr));
        Prepare(tool, {0, 0, 1000, 800});
        tool.selection_ = {100, 100, 400, 300};
        tool.texts_.push_back({{150, 150}, L"keep annotation"});
        const DWORD uiThread = GetCurrentThreadId();
        for (bool save : {false, true}) {
            const auto target = root / L"chosen folder" / L"自选截图.png";
            std::filesystem::create_directories(target.parent_path());
            std::atomic<bool> finished{false}, found{false}, releasedCapture{false};
            std::thread driver([&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!finished && std::chrono::steady_clock::now() < deadline) {
                    HWND dialog = nullptr;
                    EnumThreadWindows(uiThread, [](HWND window, LPARAM data) -> BOOL {
                        wchar_t title[80]{};
                        GetWindowTextW(window, title, 80);
                        if (wcscmp(title, L"保存截图") == 0 && IsWindowVisible(window)) {
                            *reinterpret_cast<HWND*>(data) = window;
                            return FALSE;
                        }
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(&dialog));
                    if (dialog) {
                        found = true;
                        GUITHREADINFO info{sizeof(info)};
                        releasedCapture = GetGUIThreadInfo(uiThread, &info) && !info.hwndCapture;
                        if (save) {
                            HWND filename = GetDlgItem(dialog, cmb13);
                            if (!filename) filename = GetDlgItem(dialog, edt1);
                            if (!filename) {
                                EnumChildWindows(dialog, [](HWND child, LPARAM data) -> BOOL {
                                    wchar_t name[32]{};
                                    GetClassNameW(child, name, 32);
                                    if (wcscmp(name, L"Edit") == 0 && GetDlgCtrlID(child) == 1001) {
                                        *reinterpret_cast<HWND*>(data) = child;
                                        return FALSE;
                                    }
                                    return TRUE;
                                }, reinterpret_cast<LPARAM>(&filename));
                            }
                            SendMessageW(filename, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(target.c_str()));
                            PostMessageW(dialog, WM_COMMAND, IDOK, 0);
                        } else PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                PostMessageW(tool.captureWindow_, WM_CANCELMODE, 0, 0);
            });
            if (!save) {
                Mouse(tool, WM_LBUTTONDOWN, 400, 300);
                Mouse(tool, WM_MOUSEMOVE, 420, 320);
            } else SetCapture(tool.captureWindow_);
            Start(tool, CaptureAction::Save);
            finished = true;
            driver.join();
            Require(found && releasedCapture, "save action opens a location picker without mouse capture");
            if (!save) {
                Require(tool.captureWindow_ && tool.selectionReady_ && tool.texts_.size() == 1 && !tool.saving_,
                        "cancelling save preserves selection and annotations");
                Require(!std::filesystem::exists(target), "cancel does not save a file");
                Require(!tool.resizingEdges_ && !tool.movingSelection_ && !tool.annotating_,
                        "saving during a drag ends manipulation before entering the dialog");
                Mouse(tool, WM_MOUSEMOVE, 480, 380);
                Require(tool.selection_.right == 420 && tool.selection_.bottom == 320,
                        "cancelled save does not leave selection stuck to the pointer");
            } else {
                Require(!tool.captureWindow_ && tool.lastSaveSucceeded_ && tool.lastSavedPath_ == target &&
                        tool.lastSaveDirectory_ == target.parent_path() && std::filesystem::file_size(target) > 0,
                        "save writes PNG to chosen Unicode path and remembers the directory");
            }
        }
    }
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
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize COM for PNG save");
        const auto root = std::filesystem::current_path() / "DeskTodo.Next" / "build" / "runtime-screenshot" /
            (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        SetEnvironmentVariableW(L"DESKTODO_CAPTURE_ROOT", root.c_str());
        ScreenshotOcrTestAccess::CheckInitialSelection();
        ScreenshotOcrTestAccess::CheckResize();
        ScreenshotOcrTestAccess::CheckSaveDialog(root);
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
        {
            ScreenshotTool tool(GetModuleHandleW(nullptr));
            ScreenshotOcrTestAccess::Prepare(tool);
            ScreenshotOcrTestAccess::Start(tool, CaptureAction::Translate);
            Require(translationsShown == 1 && calls == 3, "translation opens independently of local OCR");
        }
        CoUninitialize();
        std::cout << "Screenshot passed: first-drag selection, eight resize handles, bounds, annotation preservation, resized export, save picker, cancel, chosen Unicode path, OCR, LaTeX, translation handoff.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
