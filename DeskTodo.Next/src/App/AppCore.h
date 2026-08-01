#pragma once
#include <Windows.h>
#include <memory>
#include <string>

class FenceManager;
class DesktopOverlayWindow;

class AppCore {
public:
    AppCore();
    ~AppCore();

    bool Initialize(HINSTANCE hInst);
    void ExecuteCommand(const std::wstring& command);
    void Run();
    void Shutdown();

private:
    HINSTANCE hInst = nullptr;
    std::unique_ptr<FenceManager> fenceManager;
    std::unique_ptr<DesktopOverlayWindow> overlayWindow;
};
