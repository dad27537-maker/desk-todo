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
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\DeskTodo.Next.SingleInstance");
    if (!singleInstance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        AppCommand::SendToRunningInstance(command);
        CloseHandle(singleInstance);
        return 0;
    }

    HRESULT hr = OleInitialize(nullptr);
    if (FAILED(hr)) {
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
        return 1;
    }

    AppCore app;
    bool ok = app.Initialize(hInstance);
    if (ok && command != AppCommand::Show) {
        app.ExecuteCommand(command);
    }
    if (ok) {
        app.Run();
    }
    app.Shutdown();

    OleUninitialize();
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    return ok ? 0 : 1;
}
