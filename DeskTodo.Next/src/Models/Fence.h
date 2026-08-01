#pragma once
#include <string>
#include <vector>
#include <Windows.h>
#include "DesktopItem.h"
#include "Services/MonitorService.h"

inline constexpr int FENCE_TITLE_BAR_HEIGHT = 36;
// 游戏容器是一个独立的竖向启动卡，不复用普通格子的尺寸。
inline constexpr int GAME_FENCE_WIDTH = 106;
inline constexpr int GAME_FENCE_HEIGHT = 255;
inline constexpr int CLOCK_FENCE_WIDTH = 526;
inline constexpr int CLOCK_FENCE_HEIGHT = 327;

// 格子显示模式：图标模式 / 列表模式
enum class FenceViewMode {
    Icon,
    List
};

enum class FenceKind {
    Standard,
    Game,
    Clock
};

struct Fence {
    std::wstring id;
    std::wstring title;
    RECT rect{};
    COLORREF color = 0x00606060;
    float opacity = 0.65f;
    bool locked = false;
    bool collapsed = false;
    bool sortByExtension = false;
    int expandedHeight = 240;
    int zOrder = 0;

    std::wstring folderPath;
    bool isMappedFolder = false;
    FenceKind kind = FenceKind::Standard;
    MonitorService::MonitorRelativePlacement monitorPlacement;
    // Runtime only. A fence assigned to an offline monitor keeps its persisted
    // placement and is not projected onto another monitor.
    bool monitorOnline = true;
    // 只保存文件顺序（名称列表），不再保存自由坐标
    std::vector<std::wstring> itemOrder;
    // Logical grouping keeps the original file in place and stores its real
    // path here. This allows running executables and locked project folders to
    // enter a fence without moving the file or creating a .lnk shortcut.
    std::vector<std::wstring> memberPaths;
    // Legacy absolute desktop positions, aligned with itemOrder.
    // LONG_MIN marks files that did not originate from the desktop.
    std::vector<POINT> itemDesktopOrigins;
    // Stable monitor-relative desktop origins. These are preferred whenever
    // valid; the legacy POINT remains only as an upgrade fallback.
    std::vector<MonitorService::MonitorRelativePoint>
        itemDesktopOriginPlacements;
    // 保留字段仅为兼容旧配置，布局不再使用
    std::vector<POINT> itemOffsets;
    std::vector<DesktopItem> items;

    bool dropTarget = false;

    // 显示模式（持久化）
    FenceViewMode viewMode = FenceViewMode::Icon;

    // 搜索状态（不持久化，仅当前会话）
    std::wstring searchQuery;
    bool searchActive = false;

    // Content viewport scroll in physical pixels. Session-only; not persisted.
    int scrollOffset = 0;

    // 锁动画状态（不持久化，仅渲染用）
    // lockAnimProgress: 0..1，1 表示动画完成
    mutable float lockAnimProgress = 1.0f;
    mutable ULONGLONG lockAnimStartTime = 0;
    mutable bool lockAnimActive = false;
    mutable bool lockAnimFromLocked = false; // 动画起始时的锁定状态
};

enum class FenceHitArea {
    None,
    TitleBar,
    Content,
    Border,
    ResizeHandle
};

struct FenceHitTestResult {
    bool hit = false;
    int index = -1;
    std::wstring fenceId;
    FenceHitArea area = FenceHitArea::None;
};

// 拉伸边缘命中类型（位掩码）
enum class ResizeEdge {
    None = 0,
    Left = 1,
    Right = 2,
    Top = 4,
    Bottom = 8
};
inline ResizeEdge operator|(ResizeEdge a, ResizeEdge b) {
    return static_cast<ResizeEdge>(static_cast<int>(a) | static_cast<int>(b));
}
inline bool HasResizeEdge(ResizeEdge e, ResizeEdge flag) {
    return (static_cast<int>(e) & static_cast<int>(flag)) != 0;
}
