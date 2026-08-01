#include "GridLayoutService.h"
#include <algorithm>
#include <limits>
#include <shellscalingapi.h>

namespace GridLayoutService {

namespace {

// Cache the desktop logical icon size to avoid a registry read on every
// mouse-move during drag. Values rarely change; refresh every few seconds.
int ReadDesktopLogicalIconSize() {
    struct IconSizeCache {
        ULONGLONG tick = 0;
        int size = 40;
    };
    static IconSizeCache cache;

    const ULONGLONG now = GetTickCount64();
    if (now - cache.tick < 2000) {
        return cache.size;
    }

    DWORD iconSize = ICON_SIZE;
    DWORD bytes = sizeof(iconSize);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop",
        L"IconSize", RRF_RT_REG_DWORD, nullptr, &iconSize, &bytes) != ERROR_SUCCESS) {
        iconSize = ICON_SIZE;
    }
    cache.size = (std::max)(8, static_cast<int>(iconSize));
    cache.tick = now;
    return cache.size;
}

UINT GetMonitorDpi(POINT physicalPoint) {
    UINT dpiX = 96;
    UINT dpiY = 96;
    HMONITOR monitor = MonitorFromPoint(physicalPoint, MONITOR_DEFAULTTONEAREST);
    if (monitor) GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    return dpiX ? dpiX : 96;
}

int GetIconColumnCount(const RECT& contentRect, int visibleItemCount = 0) {
    // Keep items left-aligned while reserving enough width for common Chinese
    // file names. Extra fence width creates more columns instead of stretching
    // a handful of items across the entire row.
    const int itemWidth = GetDesktopIconSize(contentRect) + 56;
    const int available = (std::max)(1L,
        contentRect.right - contentRect.left - GRID_PADDING * 2);
    const int capacity = (std::max)(1,
        (available + ITEM_SPACING) / (itemWidth + ITEM_SPACING));
    return capacity;
}

int GetIconCellWidth(const RECT& contentRect, int columns) {
    const int available = (std::max)(1L,
        contentRect.right - contentRect.left - GRID_PADDING * 2);
    return (std::max)(1, available / (std::max)(1, columns));
}

}

int GetDesktopIconSize(POINT physicalPoint) {
    // Fence icons use a stable 40 logical-pixel size instead of inheriting
    // and enlarging the Windows desktop icon setting.
    return MulDiv(ICON_SIZE, GetMonitorDpi(physicalPoint), 96);
}

int GetRawDesktopIconSize(POINT physicalPoint) {
    return MulDiv(ReadDesktopLogicalIconSize(), GetMonitorDpi(physicalPoint), 96);
}

POINT GetDraggedDesktopIconTopLeft(POINT startMouse,
                                   RECT sourceIconRect,
                                   POINT currentMouse,
                                   int targetIconSize) {
    const int sourceWidth = (std::max)(1L, sourceIconRect.right - sourceIconRect.left);
    const int sourceHeight = (std::max)(1L, sourceIconRect.bottom - sourceIconRect.top);
    const int iconSize = (std::max)(1, targetIconSize);
    const int offsetX = MulDiv(startMouse.x - sourceIconRect.left, iconSize, sourceWidth);
    const int offsetY = MulDiv(startMouse.y - sourceIconRect.top, iconSize, sourceHeight);
    return { currentMouse.x - offsetX, currentMouse.y - offsetY };
}

int GetDesktopIconSize(const RECT& physicalRect) {
    POINT center{
        physicalRect.left + (physicalRect.right - physicalRect.left) / 2,
        physicalRect.top + (physicalRect.bottom - physicalRect.top) / 2
    };
    return GetDesktopIconSize(center);
}

int GetIconItemHeight(const RECT& physicalRect) {
    return GetDesktopIconSize(physicalRect) + 44;
}

// 图标模式：强制整齐网格布局，从左到右、从上到下
// 关键修复：完全忽略 hasPersistedOffset / relativeOffset，只按顺序 index 排列
void LayoutItems(const RECT& contentRect, std::vector<DesktopItem>& items, int scrollOffset) {
    const int visibleCount = static_cast<int>(std::count_if(items.begin(), items.end(),
        [](const DesktopItem& item) { return !item.filtered; }));
    const int cols = GetIconColumnCount(contentRect, visibleCount);
    const int cellWidth = GetIconCellWidth(contentRect, cols);
    const int iconSize = GetDesktopIconSize(contentRect);
    const int itemHeight = GetIconItemHeight(contentRect);

    int startX = contentRect.left + GRID_PADDING;
    int startY = contentRect.top + GRID_PADDING - scrollOffset;

    int visibleIndex = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        DesktopItem& item = items[i];
        if (item.filtered) continue; // 被搜索过滤的项不占位

        int col = visibleIndex % cols;
        int row = visibleIndex / cols;

        int x = startX + col * cellWidth;
        int y = startY + row * (itemHeight + ITEM_SPACING);

        // 记录偏移仅作信息，不作为下次布局依据
        item.relativeOffset = { x - contentRect.left, y - contentRect.top };
        item.hasPersistedOffset = false; // 强制清除，确保下次仍按网格排列

        int iconX = x + (cellWidth - iconSize) / 2;
        int iconY = y + 5;
        int textY = iconY + iconSize + 3;

        item.iconRect = { iconX, iconY, iconX + iconSize, iconY + iconSize };
        item.textRect = { x + 2, textY, x + cellWidth - 2, textY + 34 };
        item.hitRect = { x, y, x + cellWidth, y + itemHeight };

        ++visibleIndex;
    }
}

// 列表模式：纵向列表，小图标 + 文件名
void LayoutItemsList(const RECT& contentRect, std::vector<DesktopItem>& items, int scrollOffset) {
    int startX = contentRect.left + GRID_PADDING;
    int startY = contentRect.top + GRID_PADDING - scrollOffset;
    int rowHeight = LIST_ROW_HEIGHT;
    int iconSize = LIST_ICON_SIZE;
    int textLeft = startX + LIST_TEXT_LEFT;

    int visibleIndex = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        DesktopItem& item = items[i];
        if (item.filtered) continue;

        int y = startY + visibleIndex * rowHeight;
        int iconY = y + (rowHeight - iconSize) / 2;

        item.relativeOffset = { startX - contentRect.left, y - contentRect.top };
        item.hasPersistedOffset = false;

        item.iconRect = { startX, iconY, startX + iconSize, iconY + iconSize };
        item.textRect = { textLeft, y, contentRect.right - GRID_PADDING, y + rowHeight };
        item.hitRect = { startX, y, contentRect.right - GRID_PADDING, y + rowHeight };

        ++visibleIndex;
    }
}

void LayoutByMode(const RECT& contentRect, std::vector<DesktopItem>& items,
                  FenceViewMode mode, int scrollOffset) {
    if (mode == FenceViewMode::List) {
        LayoutItemsList(contentRect, items, scrollOffset);
    } else {
        LayoutItems(contentRect, items, scrollOffset);
    }
}

void LayoutGameLauncher(const RECT& contentRect, std::vector<DesktopItem>& items) {
    for (auto& item : items) {
        item.filtered = false;
        item.hasPersistedOffset = false;
        item.iconRect = {};
        item.textRect = {};
        item.hitRect = {};
    }
    if (items.empty()) return;

    DesktopItem& launcher = items.front();
    const int width = (std::max)(1L, contentRect.right - contentRect.left);
    const int height = (std::max)(1L, contentRect.bottom - contentRect.top);
    const int iconSize = (std::clamp)((std::min)(width, height) / 2, 56, 112);
    const int centerX = contentRect.left + width / 2;
    const int iconTop = contentRect.top + (std::max)(12, (height - iconSize - 46) / 2);

    launcher.relativeOffset = {
        centerX - iconSize / 2 - contentRect.left,
        iconTop - contentRect.top
    };
    launcher.iconRect = {
        centerX - iconSize / 2,
        iconTop,
        centerX - iconSize / 2 + iconSize,
        iconTop + iconSize
    };
    launcher.textRect = {
        contentRect.left + 16,
        launcher.iconRect.bottom + 8,
        contentRect.right - 16,
        (std::min)(contentRect.bottom - 36, launcher.iconRect.bottom + 46)
    };
    launcher.hitRect = {
        contentRect.left + 8,
        contentRect.top + 8,
        contentRect.right - 8,
        contentRect.bottom - 8
    };
}

int GetContentHeight(const RECT& contentRect, const std::vector<DesktopItem>& items,
                     FenceViewMode mode) {
    int visibleCount = 0;
    for (const auto& item : items) {
        if (!item.filtered) ++visibleCount;
    }
    if (visibleCount == 0) return 0;

    if (mode == FenceViewMode::List) {
        return GRID_PADDING * 2 + visibleCount * LIST_ROW_HEIGHT;
    }

    const int cols = GetIconColumnCount(contentRect, visibleCount);
    const int rows = (visibleCount + cols - 1) / cols;
    return GRID_PADDING * 2 + rows * GetIconItemHeight(contentRect) +
           (rows - 1) * ITEM_SPACING;
}

int HitTestItem(const POINT& pt, const std::vector<DesktopItem>& items) {
    for (int i = (int)items.size() - 1; i >= 0; --i) {
        if (items[i].filtered) continue; // 跳过被过滤项
        if (PtInRect(&items[i].hitRect, pt))
            return i;
    }
    return -1;
}

int GetNearestItemIndex(const RECT& contentRect, POINT pt, size_t itemCount) {
    const int cols = GetIconColumnCount(contentRect, static_cast<int>(itemCount));

    const int stepX = GetIconCellWidth(contentRect, cols);
    const int stepY = GetIconItemHeight(contentRect) + ITEM_SPACING;
    const int relativeX = pt.x - contentRect.left - GRID_PADDING;
    const int relativeY = pt.y - contentRect.top - GRID_PADDING;
    const int col = (std::clamp)((relativeX + stepX / 2) / stepX, 0, cols - 1);
    const int row = (std::max)(0, (relativeY + stepY / 2) / stepY);
    return (std::min)(static_cast<int>(itemCount), row * cols + col);
}

// 根据可见项计算插入索引（用于图标/列表模式重排序）
int GetNearestVisibleItemIndex(const RECT& contentRect, POINT pt,
                               const std::vector<DesktopItem>& items, FenceViewMode mode) {
    // 统计可见项数量
    int visibleCount = 0;
    for (const auto& item : items) if (!item.filtered) ++visibleCount;
    if (visibleCount == 0) return 0;

    if (mode == FenceViewMode::List) {
        // 列表模式：按 y 坐标计算行
        const int rowHeight = LIST_ROW_HEIGHT;
        const int relativeY = pt.y - contentRect.top - GRID_PADDING;
        int row = (relativeY + rowHeight / 2) / rowHeight;
        if (row < 0) row = 0;
        if (row > visibleCount) row = visibleCount;
        return row;
    }

    // 图标模式
    const int cols = GetIconColumnCount(contentRect, visibleCount);
    const int stepX = GetIconCellWidth(contentRect, cols);
    const int stepY = GetIconItemHeight(contentRect) + ITEM_SPACING;
    const int relativeX = pt.x - contentRect.left - GRID_PADDING;
    const int relativeY = pt.y - contentRect.top - GRID_PADDING;
    int col = (std::clamp)((relativeX + stepX / 2) / stepX, 0, cols - 1);
    int row = (std::max)(0, (relativeY + stepY / 2) / stepY);
    int idx = row * cols + col;
    return (std::clamp)(idx, 0, visibleCount);
}

POINT SnapPointToDesktopGrid(POINT pt) {
    // SystemParametersInfoW returns LOGICAL pixels; convert to physical so the
    // snap grid matches the per-monitor-DPI-aware coordinates we work in.
    // Cache the logical spacing briefly — it changes rarely and this runs on
    // every mouse-move during drag.
    struct SpacingCache {
        ULONGLONG tick = 0;
        int stepX = 112;
        int stepY = 112;
    };
    static SpacingCache cache;

    const ULONGLONG now = GetTickCount64();
    if (now - cache.tick > 2000) {
        int stepX = 112;
        int stepY = 112;
        SystemParametersInfoW(SPI_ICONHORIZONTALSPACING, 0, &stepX, 0);
        SystemParametersInfoW(SPI_ICONVERTICALSPACING, 0, &stepY, 0);
        cache.stepX = (std::max)(1, stepX);
        cache.stepY = (std::max)(1, stepY);
        cache.tick = now;
    }

    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return pt;

    const UINT dpi = GetMonitorDpi(pt);
    const int stepX = MulDiv(cache.stepX, dpi, 96);
    const int stepY = MulDiv(cache.stepY, dpi, 96);

    auto snapAxis = [](int value, int origin, int step) {
        const int relative = value - origin;
        const int rounded = relative >= 0
            ? (relative + step / 2) / step
            : (relative - step / 2) / step;
        return origin + rounded * step;
    };

    pt.x = snapAxis(pt.x, info.rcWork.left, stepX);
    pt.y = snapAxis(pt.y, info.rcWork.top, stepY);
    // Clamp using the raw desktop icon size (physical), not the fence 5/4 size.
    const int iconSize = MulDiv(ReadDesktopLogicalIconSize(), dpi, 96);
    pt.x = (std::clamp)(pt.x, info.rcWork.left, (std::max)(info.rcWork.left, info.rcWork.right - iconSize));
    pt.y = (std::clamp)(pt.y, info.rcWork.top, (std::max)(info.rcWork.top, info.rcWork.bottom - iconSize));
    return pt;
}

// 重排序：将指定名称的项移动到目标顺序位置
// targetIndex 是在可见项序列中的目标位置
bool ReorderItemToIndex(std::vector<DesktopItem>& items, const std::wstring& itemName, int targetIndex) {
    int srcIndex = -1;
    for (int i = 0; i < (int)items.size(); ++i) {
        if (_wcsicmp(items[i].name.c_str(), itemName.c_str()) == 0) {
            srcIndex = i;
            break;
        }
    }
    if (srcIndex < 0) return false;
    if (srcIndex == targetIndex) return true;

    DesktopItem moved = std::move(items[srcIndex]);
    items.erase(items.begin() + srcIndex);

    // 调整目标索引（删除后）
    if (targetIndex > srcIndex) --targetIndex;
    if (targetIndex < 0) targetIndex = 0;
    if (targetIndex > (int)items.size()) targetIndex = (int)items.size();

    items.insert(items.begin() + targetIndex, std::move(moved));
    return true;
}

} // namespace GridLayoutService
