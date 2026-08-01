#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <memory>
#include "Models/Fence.h"
#include "Models/DesktopItem.h"

class ConfigService;
class IconCache;
class TooltipService;

enum class DesktopOriginResolution {
    Missing,
    Offline,
    Online
};

class FenceManager {
public:
    FenceManager();
    ~FenceManager();

    bool Initialize(HINSTANCE hInstance);
    void Shutdown();

    std::vector<Fence> GetAllFences() const;
    std::vector<Fence>& GetAllFencesRef() { return fences; }
    Fence* GetFenceById(const std::wstring& id);
    FenceHitTestResult HitTest(POINT screenPt) const;
    void BringToFront(const std::wstring& fenceId);
    Fence* CreateFenceAt(POINT point, const std::wstring& title,
                         const std::wstring& folderPath = L"", bool mappedFolder = false);
    Fence* CreateGameFenceAt(POINT point, const std::wstring& title = L"\u6e38\u620f");
    Fence* CreateClockFenceAt(POINT point, const std::wstring& title = L"\u65f6\u95f4");
    bool ToggleFenceCollapsed(const std::wstring& fenceId);
    bool ToggleFenceLocked(const std::wstring& fenceId);
    bool SortFenceItems(const std::wstring& fenceId);
    void ArrangeFenceItems(const std::wstring& fenceId);
    void ArrangeAllItems();
    void ToggleAllLocks();
    bool RemoveFence(const std::wstring& fenceId);
    bool BackupLayout(const std::wstring& backupPath);
    bool RestoreLayout(const std::wstring& backupPath);
    bool SetItemDesktopOrigin(const std::wstring& fenceId,
                              const std::wstring& itemName, POINT position);
    bool GetItemDesktopOrigin(const std::wstring& fenceId,
                              const std::wstring& itemName, POINT& position) const;
    DesktopOriginResolution ResolveItemDesktopOrigin(
        const std::wstring& fenceId,
        const std::wstring& itemName,
        const MonitorService::DisplayTopologySnapshot& topology,
        POINT& position) const;
    bool AddLogicalItem(const std::wstring& fenceId, const std::wstring& fullPath,
                        POINT desktopOrigin = { LONG_MIN, LONG_MIN },
                        bool deferCommit = false);
    bool RemoveLogicalItem(
        const std::wstring& fenceId, const std::wstring& fullPath,
        const POINT* desiredDesktopPosition = nullptr);
    bool MoveLogicalItem(const std::wstring& sourceFenceId,
                         const std::wstring& targetFenceId,
                         const std::wstring& fullPath);
    bool IsLogicalItem(const std::wstring& fenceId, const std::wstring& fullPath) const;
    // Hides native Explorer entries for logical desktop members; the fence
    // renders the only visible copy while real paths stay unchanged.
    void SyncLogicalDesktopPositions(const std::wstring& fenceId);
    bool RestoreLogicalDesktopItems(
        const std::wstring& fenceId,
        bool allowOfflineFallback = false);

    // 重命名格子（持久化）
    bool RenameFence(const std::wstring& fenceId, const std::wstring& newTitle);

    // 切换显示模式（图标/列表，持久化）
    bool ToggleViewMode(const std::wstring& fenceId);

    // 搜索当前格子文件（仅影响显示，不移动/删除文件）
    void SetFenceSearch(const std::wstring& fenceId, const std::wstring& query);
    void ClearFenceSearch(const std::wstring& fenceId);
    bool ScrollFence(const std::wstring& fenceId, int deltaPixels);

    // 重排序：将指定项移动到目标点对应的顺序位置（不保存坐标）
    bool ReorderItemAtPoint(const std::wstring& fenceId, const std::wstring& itemName, POINT point);

    // 调整格子尺寸
    bool ResizeFence(
        const std::wstring& fenceId,
        const RECT& newRect,
        const MonitorService::DisplayTopologySnapshot* topology = nullptr);

    // 一键整理桌面：扫描桌面文件按类型分到格子
    int AutoOrganizeDesktop();

    // 显隐所有格子
    void SetAllVisible(bool visible);
    bool IsAllVisible() const { return allVisible; }
    void ToggleAllVisibility();

    // 锁动画触发
    void TriggerLockAnimation(const std::wstring& fenceId);

    void RefreshFenceItems(
        const std::wstring& fenceId,
        bool syncDesktop = true);
    void RefreshAllFenceItems();
    void LayoutFenceItems(const std::wstring& fenceId);
    int HitTestItem(POINT screenPt, std::wstring& outFenceId) const;
    void ClearItemHover(const std::wstring& fenceId);
    void ClearItemSelection(const std::wstring& fenceId);
    void SetItemHover(POINT screenPt);

    void ClearDropTargetState();
    DesktopItem* GetItem(const std::wstring& fenceId, int index);
    bool ApplyDisplayTopology(const MonitorService::DisplayTopologySnapshot& topology);
    bool CommitFencePlacement(
        const std::wstring& fenceId,
        const MonitorService::DisplayTopologySnapshot* topology = nullptr);
    bool RepairFencePositions();
    void SaveConfig();
    // 退出时恢复逻辑桌面成员的原生图标显示；不移动成员的真实路径。
    bool ReleaseFenceItemsToDesktop();
    IconCache* GetIconCache() const { return iconCache.get(); }

private:
    HINSTANCE hInstance = nullptr;
    std::vector<Fence> fences;
    std::unique_ptr<ConfigService> configService;
    std::unique_ptr<IconCache> iconCache;
    bool allVisible = true;
    // Remains false when configuration could not be read safely. This prevents
    // shutdown cleanup from overwriting a corrupt or temporarily inaccessible
    // layout with an empty/default one.
    bool configWritesEnabled = false;

    void GenerateDefaultFences();
    void SyncItemOrder(Fence& fence);
    std::wstring CreateFenceId() const;
    void ApplySearchFilter(Fence& fence);
    RECT GetContentRect(const Fence& fence) const;
    void ClampScrollOffset(Fence& fence);
    bool CaptureItemDesktopOriginPlacement(Fence& fence, size_t index,
                                           POINT position);
    bool MigrateItemDesktopOriginPlacements(
        Fence& fence,
        const MonitorService::DisplayTopologySnapshot& topology);
};
