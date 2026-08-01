#include "AppCommand.h"

#include <Windows.h>
#include <shellapi.h>
#include <vector>

namespace {

constexpr wchar_t OverlayWindowClass[] = L"DeskTodoNext_Overlay";

HWND FindOverlayWindow() {
    if (HWND direct = FindWindowW(
            OverlayWindowClass, nullptr)) {
        return direct;
    }
    struct SearchState {
        HWND result = nullptr;
    } state;
    EnumChildWindows(
        GetDesktopWindow(),
        [](HWND candidate, LPARAM parameter) -> BOOL {
            auto* search =
                reinterpret_cast<SearchState*>(parameter);
            wchar_t className[64]{};
            if (GetClassNameW(
                    candidate, className,
                    static_cast<int>(_countof(className))) &&
                _wcsicmp(
                    className, OverlayWindowClass) == 0) {
                search->result = candidate;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));
    return state.result;
}

std::wstring ReadUserObjectName(HANDLE object) {
    if (!object) return {};
    DWORD required = 0;
    GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &required);
    if (!required) return {};
    std::vector<wchar_t> buffer(
        required / sizeof(wchar_t) + 1, L'\0');
    if (!GetUserObjectInformationW(
            object, UOI_NAME, buffer.data(),
            static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
            &required)) {
        return {};
    }
    return buffer.data();
}

std::wstring CurrentDesktopScope() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    std::wstring scope =
        std::to_wstring(sessionId) + L"." +
        ReadUserObjectName(GetProcessWindowStation()) + L"." +
        ReadUserObjectName(GetThreadDesktop(GetCurrentThreadId()));
    for (wchar_t& value : scope) {
        const bool safe =
            (value >= L'a' && value <= L'z') ||
            (value >= L'A' && value <= L'Z') ||
            (value >= L'0' && value <= L'9') ||
            value == L'-' || value == L'_';
        if (!safe) value = L'_';
    }
    return scope;
}

}

namespace AppCommand {

std::wstring ParseCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return Show;

    std::wstring command = Show;
    if (argc >= 2) {
        command = argv[1];
        while (!command.empty() && command.front() == L'-') command.erase(command.begin());
    }
    LocalFree(argv);
    return command.empty() ? Show : command;
}

std::wstring SingleInstanceMutexName() {
    return L"Local\\DeskTodo.Next.SingleInstance." +
        CurrentDesktopScope();
}

std::wstring ShowEventName() {
    return L"Local\\DeskTodo.Next.ShowRunningInstance." +
        CurrentDesktopScope();
}

bool SendToRunningInstance(const std::wstring& command) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        // A named event is independent of the window station/desktop that
        // launched the second process. This keeps normal double-click
        // activation reliable while Explorer is rebuilding its desktop view
        // or while the settings window owns a nested message loop.
        if (_wcsicmp(command.c_str(), Show) == 0 ||
            _wcsicmp(command.c_str(), Settings) == 0) {
            const std::wstring showEventName = ShowEventName();
            HANDLE showEvent = OpenEventW(
                EVENT_MODIFY_STATE, FALSE, showEventName.c_str());
            if (showEvent) {
                const bool signaled = SetEvent(showEvent) == TRUE;
                CloseHandle(showEvent);
                if (signaled) return true;
            }
        }

        HWND window = FindOverlayWindow();
        if (window) {
            COPYDATASTRUCT data{};
            data.dwData = 1;
            data.cbData = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
            data.lpData = const_cast<wchar_t*>(command.c_str());
            DWORD_PTR result = 0;
            return SendMessageTimeoutW(
                       window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                       SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result) != 0;
        }
        Sleep(100);
    }
    return false;
}

}
