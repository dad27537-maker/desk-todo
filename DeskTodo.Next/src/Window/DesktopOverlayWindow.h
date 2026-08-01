#pragma once
#include <Windows.h>
#include <shellapi.h>
#include <string>
#include <memory>
#include <vector>
#include "Rendering/FenceRenderer.h"
#include "Rendering/SnapRenderer.h"
#include "Models/Fence.h"
#include "Models/DragContext.h"
#include "Models/SnapCommon.h"
#include "Services/SnapManager.h"

class FenceManager;
class TooltipService;
class FileMoveService;
class DesktopIconLayoutService;
class PomodoroWidgetWindow;
class TodoWidgetWindow;
class LanTransferWindow;
class BlackHoleBackground;
class ScreenshotTool;

enum class FenceTitleControl {
    None,
    Collapse,
    Lock,
    View,    // 视图切换按钮（图标/列表）
    Search,  // 搜索按钮
    Menu
};

enum class FenceInteractionState {
    None,
    Hover,
    PressedTitleBar,
    MovingFence,
    Locked,
    PressedItem,
    DraggingItem,
    ResizingFence,    // 拉伸格子
    TextInput         // 文本输入（重命名/搜索）
};

struct FenceMoveContext {
    bool active = false;
    bool hasMoved = false;
    std::wstring fenceId;
    POINT startMouse = {0, 0};
    RECT startRect = {0, 0, 0, 0};
};

// 拉伸上下文
struct FenceResizeContext {
    bool active = false;
    std::wstring fenceId;
    POINT startMouse = {0, 0};
    RECT startRect = {0, 0, 0, 0};
    int startExpandedHeight = 0;
    ResizeEdge edge = ResizeEdge::None;
};

// 文本输入模式
enum class TextInputMode {
    None,
    Rename,
    Search
};

class DesktopOverlayWindow {
public:
    explicit DesktopOverlayWindow(FenceManager* mgr);
    ~DesktopOverlayWindow();

    bool Create(HINSTANCE hInst);
    void Destroy();
    void RequestRedraw();
    void ExecuteCommand(const std::wstring& command);

    HWND Handle() const { return hwnd; }

private:
    class DropTarget;

    HWND hwnd = nullptr;
    HWND desktopHostWindow = nullptr;
    FenceManager* fenceManager = nullptr;
    FenceRenderer renderer;
    SnapRenderer snapRenderer;
    SnapManager snapManager;
    std::vector<SnapLine> currentSnapLines;
    std::unique_ptr<TooltipService> tooltipService;
    std::unique_ptr<FileMoveService> fileMoveService;
    std::unique_ptr<DesktopIconLayoutService> desktopIconLayoutService;
    std::unique_ptr<PomodoroWidgetWindow> pomodoroWidget;
    std::unique_ptr<TodoWidgetWindow> todoWidget;
    std::unique_ptr<LanTransferWindow> lanTransferWidget;
    std::unique_ptr<BlackHoleBackground> blackHoleBackground;
    std::unique_ptr<ScreenshotTool> screenshotTool;
    bool exitInProgress = false;
    std::wstring displayTopologySignature;
    MonitorService::DisplayTopologySnapshot displayTopologySnapshot;
    MonitorService::DisplayTopologySnapshot topologyCandidate;
    int topologyCandidateSamples = 0;
    bool displayTopologyDirty = false;
    bool displayTopologyRefreshPending = false;
    // A display can be power-cycled without changing its final topology
    // signature.  Keep a separate recovery bit so the compositor, D2D
    // target and interaction caches are rebuilt even when the monitor list
    // looks identical after reconnect.
    bool displayRecoveryPending = false;
    // Input is enabled only after the current monitor topology has been
    // committed and one complete layered frame has been uploaded.  The
    // generation prevents a drag from mixing coordinates from two topology
    // snapshots during a hot-plug transition.
    bool topologyReady = false;
    bool topologyFramePending = false;
    ULONGLONG topologyGeneration = 0;
    ULONGLONG interactionTopologyGeneration = 0;
    ULONGLONG topologyRefreshSerial = 0;
    ULONGLONG topologyFrameSerial = 0;
    int desktopIconRestorePassesRemaining = 0;
    DropTarget* dropTarget = nullptr;
    bool dropTargetRegistered = false;
    HWINEVENTHOOK desktopViewEventHook = nullptr;
    HWND watchedDesktopIconWindow = nullptr;
    HANDLE showRunningInstanceEvent = nullptr;

    HDC memDC = nullptr;
    HBITMAP memBitmap = nullptr;
    void* bitmapBits = nullptr;
    int width = 0;
    int height = 0;
    ULONGLONG backbufferGeneration = 0;
    ULONGLONG completeFrameGeneration = 0;
    bool forceFullRedraw = true;
    bool backbufferHasCompleteFrame = false;
    ULONGLONG lastRenderTick = 0;
    bool interactiveRedrawPending = false;
    bool interactiveRedrawTimerArmed = false;
    RECT previousInteractiveDirtyRect{};
    bool previousInteractiveDirtyValid = false;
    ULONGLONG lastDesktopLayoutSyncTick = 0;

    std::wstring hoverFenceId;
    std::wstring activeFenceId;
    FenceInteractionState interactionState = FenceInteractionState::None;
    FenceMoveContext moveContext;
    FenceResizeContext resizeContext;
    MonitorService::DisplayTopologySnapshot interactionTopology;

    DragContext dragContext;
    GhostIcon ghostIcon;

    std::wstring currentDropFenceId;

    std::wstring tooltipFenceId;
    int tooltipItemIndex = -1;
    bool tooltipTimerActive = false;
    std::wstring visibleTooltipFenceId;
    int visibleTooltipItemIndex = -1;
    static constexpr UINT_PTR TOOLTIP_TIMER_ID = 1001;
    static constexpr UINT_PTR TOOLTIP_GUARD_TIMER_ID = 1007;
    static constexpr int TOOLTIP_DELAY_MS = 500;
    static constexpr int TOOLTIP_GUARD_INTERVAL_MS = 100;

    // 文本输入状态（重命名 / 搜索）
    TextInputMode textInputMode = TextInputMode::None;
    std::wstring textInputFenceId;
    std::wstring textInputText;
    int textInputCaret = 0;
    bool caretBlinkOn = true;
    bool textInputHadNoActivate = true; // 记录原本是否有 NOACTIVATE
    static constexpr UINT_PTR CARET_TIMER_ID = 1002;
    static constexpr UINT_PTR ANIM_TIMER_ID = 1003;
    static constexpr UINT_PTR BLACKHOLE_TIMER_ID = 1004;
    static constexpr UINT_PTR INTERACTIVE_REDRAW_TIMER_ID = 1010;
    static constexpr UINT_PTR DESKTOP_RECONCILE_TIMER_ID = 1011;
    static constexpr UINT_PTR SHOW_INSTANCE_TIMER_ID = 1012;
    static constexpr UINT_PTR CLOCK_TIMER_ID = 1013;
    static constexpr int CARET_BLINK_MS = 530;
    static constexpr int ANIM_FRAME_MS = 16;
    static constexpr int BLACKHOLE_FRAME_MS = 33; // ~30 FPS
    static constexpr int DISPLAY_TOPOLOGY_SAMPLE_MS = 400;
    static constexpr int DISPLAY_TOPOLOGY_STABLE_SAMPLES = 3;
    static constexpr int INTERACTIVE_FRAME_MS = 16;
    static constexpr int DESKTOP_LAYOUT_SYNC_MS = 2000;

    NOTIFYICONDATAW nid = {};
    static constexpr UINT WM_TRAY_ICON = WM_APP + 1;

    bool CreateTrayIcon(HINSTANCE hInst);
    void DestroyTrayIcon();
    void ShowTrayMenu();
    void ShowFenceMenu(const std::wstring& fenceId, POINT pt);
    void DissolveFence(const std::wstring& fenceId);
    bool RequestExit();
    bool ReleaseAllManagedFilesToDesktop();
    void BackupLayout();
    void RestoreLayout();
    FenceTitleControl HitTestTitleControl(const Fence& fence, POINT pt) const;
    ResizeEdge HitTestResizeEdge(const Fence& fence, POINT pt) const;
    std::wstring SelectFolder();

    bool EnsureBackbuffer(int w, int h);
    void ClearBackbufferRect(const RECT& localRect);
    void AttachToDesktopHost(const RECT& virtualScreen);
    bool PositionForVirtualScreen(const RECT& virtualScreen);
    void Render();
    void UpdateMovingFence(POINT pt);
    void CancelMove();
    void CancelItemDrag();
    void ClearDragContext();
    void RestoreDraggedItemVisualState();

    // 拉伸相关
    void StartResize(const std::wstring& fenceId, ResizeEdge edge, POINT pt);
    void UpdateResize(POINT pt);
    void EndResize();
    void CancelResize();

    // 文本输入相关
    void EnterTextInputMode(TextInputMode mode, const std::wstring& fenceId);
    void ExitTextInputMode(bool commit);
    void CommitRename();
    void CommitSearchText(const std::wstring& text);
    RECT GetRenameInputRect(const Fence& fence) const;
    RECT GetSearchInputRect(const Fence& fence) const;
    bool IsPointInSearchClearButton(POINT pt) const;
    bool IsPointInTextInput(POINT pt) const;
    void HandleCharInput(wchar_t ch);
    void HandleKeyDownInput(WPARAM vk);
    void UpdateCaretBlink();
    bool HasActiveAnimation() const;

    void StartTooltipTimer(const std::wstring& fenceId, int itemIndex);
    void CancelTooltipTimer();
    void ShowItemTooltip(const std::wstring& fenceId, int itemIndex);
    void ShowControlTooltip(const std::wstring& text, POINT pt);
    void HideTooltip();

    void BeginPressedItem(const std::wstring& fenceId, int itemIndex, POINT pt);
    void UpdatePressedItem(POINT pt);
    void EndPressedItemWithoutDrag(POINT pt);
    void UpdateDraggingItem(POINT pt);
    void UpdateItemDragVisual(POINT pt);
    void EndDraggingItem(POINT pt);
    void ShowItemContextMenu(POINT pt);
    DropTargetInfo DetectDropTarget(POINT pt) const;
    bool DropItemToFence(const std::wstring& targetFenceId, POINT pt);
    bool DropItemToDesktop(POINT pt);
    bool MovePathWithConflictPrompt(const std::wstring& sourcePath,
                                    const std::wstring& targetDirectory,
                                    std::wstring* movedPath = nullptr,
                                    bool reserveDesktopName = false);
    void RefreshFenceAfterMove(const std::wstring& sourceFenceId, const std::wstring& targetFenceId);

    std::wstring QueryDropFilePath(HDROP hDrop, UINT index);
    void OnDropFiles(HDROP hDrop);
    bool HandleDroppedPaths(const std::vector<std::wstring>& paths,
                            POINT screenPoint);

    // 设置窗口
    void ShowSettingsDialog();
    void StartScreenshotCaptureSoon();

    // 黑洞背景层
    void ToggleBlackHoleBackground();
    void UpdateBlackHoleBackgroundSize();
    void SetBlackHoleBackgroundEnabled(bool enabled);
    void CancelInteractionsForTopology();
    void ArmDisplayTopologyRefreshTimer();
    std::vector<std::wstring> CollectLogicalMemberPaths() const;
    void RestoreDesktopIconsAfterTopology(
        const MonitorService::DisplayTopologySnapshot& topology);
    void SyncStableDesktopIconLayout(
        const MonitorService::DisplayTopologySnapshot& topology);
    void ScheduleDisplayTopologyRefresh(bool forceRecovery = false);
    void RefreshDisplayTopology();
    void EnsureDesktopViewEventHook();
    void RemoveDesktopViewEventHook();
    void ScheduleDesktopReconcile();

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT, WPARAM, LPARAM);
};
