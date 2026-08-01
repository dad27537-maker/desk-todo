#pragma once

#include <string>

namespace AppCommand {

inline constexpr wchar_t Show[] = L"show";
inline constexpr wchar_t NewFence[] = L"new-fence";
inline constexpr wchar_t NewGameFence[] = L"new-game-fence";
inline constexpr wchar_t NewClockFence[] = L"new-clock-fence";
inline constexpr wchar_t MapFolder[] = L"map-folder";
inline constexpr wchar_t Refresh[] = L"refresh";
inline constexpr wchar_t Arrange[] = L"arrange";
inline constexpr wchar_t AutoOrganize[] = L"auto-organize";
inline constexpr wchar_t ToggleVisibility[] = L"toggle-visibility";
inline constexpr wchar_t Settings[] = L"settings";
inline constexpr wchar_t OpenStorage[] = L"open-storage";
inline constexpr wchar_t ToggleLock[] = L"toggle-lock";
inline constexpr wchar_t Version[] = L"version";
inline constexpr wchar_t BackupLayout[] = L"backup-layout";
inline constexpr wchar_t RestoreLayout[] = L"restore-layout";
inline constexpr wchar_t Screenshot[] = L"screenshot";
inline constexpr wchar_t LanTransfer[] = L"lan-transfer";
inline constexpr wchar_t ToggleBlackHoleBg[] = L"toggle-blackhole-bg";
inline constexpr wchar_t Exit[] = L"exit";

std::wstring ParseCommandLine();
std::wstring SingleInstanceMutexName();
std::wstring ShowEventName();
bool SendToRunningInstance(const std::wstring& command);

}
