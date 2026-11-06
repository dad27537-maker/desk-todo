#pragma once

#include <Windows.h>
#include <string>
#include <vector>

enum class ItemContextMenuResult {
    Cancelled,
    ShellCommand,
    BackupLayout,
    RestoreLayout
};

class DesktopShellService {
public:
    static bool PositionPathOnDesktop(const std::wstring& path, POINT physicalScreenPoint);
    // Restores a previously captured desktop position without grid snapping or
    // nearest-empty-cell substitution. Supports negative multi-monitor coords.
    static bool RestorePathToDesktopPosition(const std::wstring& path,
                                             POINT physicalScreenPoint);
    // Performs one non-blocking positioning attempt. Topology recovery uses
    // this in small timer-driven batches so Explorer retries never stall the
    // overlay's pointer-message loop.
    static bool TryRestorePathToDesktopPosition(
        const std::wstring& path, POINT physicalScreenPoint);
    static bool GetPathPositionOnDesktop(const std::wstring& path, POINT& physicalScreenPoint);
    static std::vector<std::wstring> GetSelectedDesktopPaths();
    // Removes/restores only the desktop IShellView representation. The real
    // path and its attributes are never changed.
    static bool SetPathSuppressedOnDesktop(const std::wstring& path, bool suppressed);
    static bool IsPathSuppressedOnDesktop(const std::wstring& path);
    // Replaces the set of ordinary desktop paths whose saved monitor is
    // currently offline. This reason is independent from logical-fence
    // suppression, so reconnecting a monitor cannot expose fence members.
    static bool SetOfflineMonitorDesktopPaths(
        const std::vector<std::wstring>& paths);
    // Rebinds the view-scoped filter once and reconciles queued logical
    // members using exact Shell children only. The real filesystem paths remain
    // untouched and visible in ordinary Explorer folder windows.
    static bool ReconcileSuppressedDesktopPaths();
    // Groups nested logical membership/filter transactions while preserving
    // Explorer's native icon layer. Calls may nest.
    static bool BeginNativeDesktopUpdate();
    static void EndNativeDesktopUpdate();
    static bool RestoreNativeDesktopView();
    // Returns the actual SysListView32 used for native desktop icons, or null
    // when the current Explorer generation has not exposed one yet.
    static HWND GetNativeDesktopIconWindow();
    // Returns the WorkerW/Progman host that owns the native desktop view.
    static HWND GetNativeDesktopHostWindow();
    static void ForgetPathSuppression(const std::wstring& path);
    static void ClearSuppressionState();
    // Returns stable, human-readable shell discovery/PIDL diagnostics for the
    // interactive probe. This never changes the desktop view.
    static std::wstring DiagnoseDesktopView(const std::wstring& path);
    static void RefreshDesktop();
    static bool IsPointOnDesktop(POINT physicalScreenPoint);
    static ItemContextMenuResult ShowItemContextMenu(
        HWND owner, const std::wstring& path, POINT screenPoint);

    // Snaps a physical screen point to the actual desktop icon grid.
    // Uses IFolderView::GetSpacing() and the listview client origin — the same
    // grid Windows uses for native desktop icon placement.  Cached internally
    // with a short TTL so it is safe to call on every mouse-move.
    static POINT SnapPointToDesktopGrid(POINT pt);
};
