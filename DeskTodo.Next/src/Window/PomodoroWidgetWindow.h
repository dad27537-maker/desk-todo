#pragma once

#include <Windows.h>

#include "Models/PomodoroTimer.h"

class PomodoroWidgetWindow {
public:
    explicit PomodoroWidgetWindow(HINSTANCE instance);
    ~PomodoroWidgetWindow();

    void RestoreIfVisible();
    void ShowOrCreate();
    void Destroy();
    HWND Handle() const { return window_; }

private:
    struct PersistedState {
        DWORD magic = 0x504F4D4F;
        DWORD version = 1;
        BOOL visible = FALSE;
        RECT rect{};
        PomodoroTimerState timer{};
    };

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    PomodoroTimerState timer_{};
    RECT savedRect_{};
    bool shouldRestore_ = false;
    bool dragging_ = false;
    POINT dragStart_{};
    RECT dragWindowStart_{};

    bool EnsureWindow();
    void LoadState();
    void SaveState(bool visible);
    void ToggleTimer();
    float UiScale() const;
    int Scale(int value) const;
    void Paint(HDC dc);
    RECT StartButtonRect() const;
    RECT ResetButtonRect() const;
    RECT CloseButtonRect() const;

    static LRESULT CALLBACK StaticWindowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WindowProc(UINT, WPARAM, LPARAM);
};
