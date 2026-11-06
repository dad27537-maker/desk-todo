#include "BlackHoleBackground.h"

#include <string>

BlackHoleBackground::BlackHoleBackground() = default;
BlackHoleBackground::~BlackHoleBackground() { Stop(); }

bool BlackHoleBackground::Start(uint32_t, uint32_t, float) {
    if (IsRunning()) return true;
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) return false;
    std::wstring command = L"\"" + std::wstring(executable, length) + L"\" --blackhole-monitor";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &startup, &process)) {
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
            0, nullptr, nullptr, &startup, &process)) return false;
    }
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    processId_ = process.dwProcessId;
    running_.store(true);
    return true;
}

void BlackHoleBackground::Stop() {
    if (!process_) { running_.store(false); return; }
    struct CloseData { DWORD pid; } data{processId_};
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        DWORD pid=0; GetWindowThreadProcessId(window,&pid);
        const auto* close=reinterpret_cast<const CloseData*>(value);
        if(pid==close->pid) PostMessageW(window,WM_CLOSE,0,0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    if (WaitForSingleObject(process_, 2500) == WAIT_TIMEOUT) TerminateProcess(process_, 0);
    CloseHandle(process_); process_=nullptr; processId_=0; running_.store(false);
}

void BlackHoleBackground::RequestResize(uint32_t, uint32_t, float) {}
void BlackHoleBackground::UpdateSettings(const BlackHole::Settings& settings) { settings_=settings; }
BlackHole::Settings BlackHoleBackground::CurrentSettings() const { return settings_; }
void BlackHoleBackground::Suspend() { Stop(); }
void BlackHoleBackground::Resume() {}
bool BlackHoleBackground::TryGetFrame(std::vector<uint8_t>&, uint32_t&, uint32_t&) { return false; }

bool BlackHoleBackground::IsRunning() const {
    if (!process_) return false;
    DWORD code=0;
    return GetExitCodeProcess(process_,&code) && code==STILL_ACTIVE;
}
