#include "AppCore.h"
#include "Managers/FenceManager.h"
#include "Window/DesktopOverlayWindow.h"
#include "Services/ShellIntegrationService.h"
#include "Services/DesktopShellService.h"

AppCore::AppCore() {}
AppCore::~AppCore() {}

bool AppCore::Initialize(HINSTANCE hInst) {
    this->hInst = hInst;

    // Recover Explorer if an older build was terminated while its native
    // desktop icon view was temporarily hidden.
    DesktopShellService::RestoreNativeDesktopView();
    DesktopShellService::BeginNativeDesktopUpdate();

    // Always remove legacy desktop context-menu entries before any UI or
    // storage initialization that could fail or return early.
    ShellIntegrationService::EnsureDesktopContextMenu(L"");

    fenceManager = std::make_unique<FenceManager>();
    if (!fenceManager->Initialize(hInst)) {
        DesktopShellService::EndNativeDesktopUpdate();
        return false;
    }

    overlayWindow = std::make_unique<DesktopOverlayWindow>(fenceManager.get());
    if (!overlayWindow->Create(hInst)) {
        DesktopShellService::EndNativeDesktopUpdate();
        return false;
    }

    // Commit every path-filter change before allowing Explorer to repaint.
    for (const auto& fence : fenceManager->GetAllFencesRef()) {
        fenceManager->SyncLogicalDesktopPositions(fence.id);
    }
    DesktopShellService::ReconcileSuppressedDesktopPaths();
    DesktopShellService::EndNativeDesktopUpdate();

    // Keep the desktop launcher synchronized with the running executable so
    // Windows picks up the bundled application icon after an upgrade.
    wchar_t executablePath[32768]{};
    const DWORD executableLength = GetModuleFileNameW(
        hInst, executablePath, static_cast<DWORD>(std::size(executablePath)));
    if (executableLength > 0 &&
        executableLength < std::size(executablePath)) {
        ShellIntegrationService::EnsureDesktopShortcut(
            std::wstring(executablePath, executableLength));
    }

    return true;
}

void AppCore::ExecuteCommand(const std::wstring& command) {
    if (overlayWindow) overlayWindow->ExecuteCommand(command);
}

void AppCore::Run() {
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void AppCore::Shutdown() {
    // 退出前恢复逻辑桌面成员的原生图标显示；真实文件路径保持不变。
    if (fenceManager) {
        fenceManager->ReleaseFenceItemsToDesktop();
    }
    if (overlayWindow) overlayWindow.reset();
    if (fenceManager) {
        fenceManager->SaveConfig();
        fenceManager.reset();
    }
    // 退出后刷新桌面，确保 Explorer 正确反映文件状态。
    DesktopShellService::RefreshDesktop();
}
