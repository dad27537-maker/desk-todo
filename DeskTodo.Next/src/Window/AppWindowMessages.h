#pragma once

#include <Windows.h>

constexpr UINT WM_DESKTODO_SHOW_POMODORO = WM_APP + 20;
constexpr UINT WM_DESKTODO_SHOW_TODO = WM_APP + 21;
constexpr UINT WM_DESKTODO_START_CAPTURE = WM_APP + 22;
constexpr UINT WM_DESKTODO_TOGGLE_BLACK_HOLE_BG = WM_APP + 23;
constexpr UINT WM_DESKTODO_SHOW_LAN_TRANSFER = WM_APP + 24;
constexpr UINT WM_DESKTODO_APPLY_BLACK_HOLE_SETTINGS = WM_APP + 25;
constexpr UINT WM_DESKTODO_NEW_GAME_FENCE = WM_APP + 26;
constexpr UINT WM_DESKTODO_NEW_CLOCK_FENCE = WM_APP + 27;
// wParam carries the RegisterHotKey modifier flags and lParam carries the
// virtual-key code. The synchronous result reports whether the new shortcut
// was registered successfully.
constexpr UINT WM_DESKTODO_RELOAD_SCREENSHOT_HOTKEY = WM_APP + 28;
