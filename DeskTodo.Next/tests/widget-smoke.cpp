#include "Window/TodoWidgetWindow.h"
#include "Window/PomodoroWidgetWindow.h"
#include <commctrl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void Check(bool condition, const char* text) { ++checks; if (!condition) throw std::runtime_error(text); }
}
struct WidgetTestAccess {
    static void Todo() {
        {
            TodoWidgetWindow widget(GetModuleHandleW(nullptr));
            widget.savedRect_ = {20000, 100, 20500, 500};
            Check(widget.EnsureWindow(), "create Todo on removed monitor");
            RECT rect{}; GetWindowRect(widget.window_, &rect);
            Check(rect.right - rect.left == 500 && rect.bottom - rect.top == 400, "Todo keeps dimensions after monitor removal");
            SetWindowPos(widget.window_, nullptr, 0, 0, 600, 450, SWP_NOMOVE | SWP_NOZORDER);
            GetClientRect(widget.window_, &rect);
            Check(rect.right == 600 && rect.bottom == 450, "Todo client area follows proposed resize");
            Check(TodoList::Add(widget.items_, L"测试 task", TodoPriority::High, L"2028-02-29"), "add Todo");
            widget.SaveState(false);
        }
        TodoWidgetWindow loaded(GetModuleHandleW(nullptr));
        Check(loaded.items_.size() == 1 && loaded.items_[0].text == L"测试 task" &&
            loaded.items_[0].dueDate == L"2028-02-29" && loaded.items_[0].priority == TodoPriority::High,
            "Todo durable persistence roundtrip");
    }
    static void Timer() {
        {
            PomodoroWidgetWindow widget(GetModuleHandleW(nullptr));
            Check(widget.EnsureWindow(), "create timer");
            SetWindowPos(widget.window_, nullptr, 0, 0, 600, 400, SWP_NOMOVE | SWP_NOZORDER);
            RECT rect{}; GetClientRect(widget.window_, &rect);
            Check(rect.right == 600 && rect.bottom == 400, "timer client area follows resize");
            widget.timer_.running = true;
            widget.timer_.remainingSeconds = 100;
            widget.lastTickMs_ = GetTickCount64() - 5100;
            widget.AdvanceTimer();
            Check(widget.timer_.remainingSeconds == 95, "delayed timer message consumes elapsed seconds");
            widget.ToggleTimer();
            Check(!widget.timer_.running && widget.timer_.remainingSeconds == 95, "pause commits elapsed time");
            widget.SaveState(false);
        }
        PomodoroWidgetWindow loaded(GetModuleHandleW(nullptr));
        Check(!loaded.timer_.running && loaded.timer_.remainingSeconds == 95, "timer durable persistence roundtrip");
    }
};
int main() {
    const auto root = std::filesystem::current_path() / ("widget-test-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    SetEnvironmentVariableW(L"DESKTODO_DATA_ROOT", root.c_str());
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_DATE_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int result = 0;
    try { WidgetTestAccess::Todo(); WidgetTestAccess::Timer(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    std::filesystem::remove_all(root);
    std::cout << "Widget checks: " << checks << ", failures: " << result << '\n';
    return result;
}
