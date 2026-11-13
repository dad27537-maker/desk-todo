#include "Window/OcrResultWindow.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    const DWORD uiThread = GetCurrentThreadId();
    std::atomic<bool> found{false};
    for (bool quit : {false, true}) {
        found = false;
        std::thread driver([&] {
            HWND result = nullptr;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!result && std::chrono::steady_clock::now() < deadline) {
                EnumThreadWindows(uiThread, [](HWND window, LPARAM data) -> BOOL {
                    if (GetDlgItem(window, 5101) && IsWindowVisible(window)) {
                        *reinterpret_cast<HWND*>(data) = window; return FALSE;
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&result));
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            found = result != nullptr;
            if (quit || !result) PostThreadMessageW(uiThread, WM_QUIT, 7, 0);
            else PostMessageW(result, WM_CLOSE, 0, 0);
        });
        OcrResultWindow::Show(nullptr, L"DESKTODO OCR 12345\r\n文字识别测试");
        driver.join();
        if (!found || FindWindowW(L"DeskTodoOcrResult", nullptr)) return 1;
        if (quit) {
            MSG message{};
            if (!PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) || message.wParam != 7) return 1;
        }
    }
    std::cout << "OCR result window: normal close and process quit preserve window lifetime and quit message.\n";
    return 0;
}
