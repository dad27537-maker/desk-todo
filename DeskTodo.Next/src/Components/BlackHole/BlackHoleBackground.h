#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <vector>

#include "Components/BlackHole/BlackHoleTypes.h"

// Process controller for the directly integrated ghostty-blackhole renderer.
// The child is the same DeskTodo.Next.exe in --blackhole-render mode.
class BlackHoleBackground {
public:
    BlackHoleBackground();
    ~BlackHoleBackground();
    BlackHoleBackground(const BlackHoleBackground&) = delete;
    BlackHoleBackground& operator=(const BlackHoleBackground&) = delete;

    bool Start(uint32_t width, uint32_t height, float dpiScale);
    void Stop();
    void RequestResize(uint32_t width, uint32_t height, float dpiScale);
    void UpdateSettings(const BlackHole::Settings& settings);
    BlackHole::Settings CurrentSettings() const;
    void Suspend();
    void Resume();
    bool TryGetFrame(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);
    bool IsRunning() const;

private:
    HANDLE process_ = nullptr;
    DWORD processId_ = 0;
    std::atomic<bool> running_{false};
    BlackHole::Settings settings_{};
};
