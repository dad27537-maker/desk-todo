#include <Windows.h>
#include <objbase.h>
#include <cstring>
#include "App/AppCore.h"
#include "App/AppCommand.h"

int GhosttyBlackHoleMain(int argc, char* argv[]);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    const char* rawCommand = GetCommandLineA();
    if (strstr(rawCommand, "--blackhole-render") != nullptr ||
        strstr(rawCommand, " --render") != nullptr) {
        char app[] = "DeskTodo.Next.exe";
        char mode[] = "--render";
        char* argv[] = { app, mode, nullptr };
        return GhosttyBlackHoleMain(2, argv);
    }
    if (strstr(rawCommand, "--blackhole-monitor") != nullptr) {
        char app[] = "DeskTodo.Next.exe";
        char mode[] = "--monitor";
        char* argv[] = { app, mode, nullptr };
        return GhosttyBlackHoleMain(2, argv);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::wstring command = AppCommand::ParseCommandLine();
    const std::wstring singleInstanceMutexName =
        AppCommand::SingleInstanceMutexName();
    HANDLE singleInstance = CreateMutexW(
        nullptr, TRUE, singleInstanceMutexName.c_str());
    if (!singleInstance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const bool delivered =
            AppCommand::SendToRunningInstance(command);
        if (!delivered) {
            MessageBoxW(
                nullptr,
                L"DeskTodo 已在运行，但窗口暂时无法响应。\n"
                L"请稍后重试；如果问题持续，请在任务管理器中结束旧实例。",
                L"DeskTodo 启动失败",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        CloseHandle(singleInstance);
        return delivered ? 0 : 2;
    }
    const std::wstring showEventName = AppCommand::ShowEventName();
    HANDLE showRunningInstanceEvent = CreateEventW(
        nullptr, FALSE, FALSE, showEventName.c_str());

    HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr)) {
        if (showRunningInstanceEvent) CloseHandle(showRunningInstanceEvent);
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
        return 1;
    }

    AppCore app;
    bool ok = app.Initialize(hInstance);
    if (ok) {
        // "show" is a real user-facing launch action. Executing it on the
        // first instance as well as forwarding it to an existing instance
        // prevents a normal double-click from appearing to do nothing.
        app.ExecuteCommand(command);
    } else {
        MessageBoxW(
            nullptr,
            L"DeskTodo 初始化失败，无法创建桌面窗口。\n"
            L"请确认资源管理器正在运行，然后重新启动 DeskTodo。",
            L"DeskTodo 启动失败",
            MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    if (ok) app.Run();
    app.Shutdown();

    OleUninitialize();
    if (showRunningInstanceEvent) CloseHandle(showRunningInstanceEvent);
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    return ok ? 0 : 1;
}
