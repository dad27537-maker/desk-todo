#pragma once
#include <Windows.h>
#include <vector>
#include "Models/DesktopItem.h"
#include "Models/Fence.h"

namespace GridLayoutService {

// 图标模式参数
constexpr int ITEM_WIDTH = 84;
constexpr int ITEM_HEIGHT = 92;
constexpr int ICON_SIZE = 40;
constexpr int GRID_PADDING = 10;
constexpr int ITEM_SPACING = 4;

// 列表模式参数
constexpr int LIST_ROW_HEIGHT = 34;
constexpr int LIST_ICON_SIZE = 20;
constexpr int LIST_TEXT_LEFT = 30;

// 拉伸参数
constexpr int MIN_FENCE_WIDTH = 180;
constexpr int MIN_FENCE_HEIGHT = 120;
constexpr int RESIZE_EDGE_HOT = 8;    // 边缘热区
constexpr int RESIZE_CORNER_HOT = 12; // 四角热区

// Windows 桌面图标设置是逻辑像素；格子布局使用物理像素。
int GetDesktopIconSize(POINT physicalPoint);
int GetDesktopIconSize(const RECT& physicalRect);
int GetIconItemHeight(const RECT& physicalRect);

// 图标模式强制整齐网格布局（从左到右、从上到下），忽略任何自由坐标
void LayoutItems(const RECT& contentRect, std::vector<DesktopItem>& items, int scrollOffset = 0);

// 列表模式布局（纵向列表，小图标 + 文件名）
void LayoutItemsList(const RECT& contentRect, std::vector<DesktopItem>& items, int scrollOffset = 0);

// 根据当前显示模式布局
void LayoutByMode(const RECT& contentRect, std::vector<DesktopItem>& items,
                  FenceViewMode mode, int scrollOffset = 0);

// A game fence is a single large launch card rather than a file grid.
void LayoutGameLauncher(const RECT& contentRect, std::vector<DesktopItem>& items);

int GetContentHeight(const RECT& contentRect, const std::vector<DesktopItem>& items,
                     FenceViewMode mode);

// 命中测试（跳过 filtered 项）
int HitTestItem(const POINT& pt, const std::vector<DesktopItem>& items);

// 根据点位置计算最近的可视网格索引（用于重排序）
int GetNearestItemIndex(const RECT& contentRect, POINT pt, size_t itemCount);

// 仅根据可见项计算插入位置（跳过 filtered）
int GetNearestVisibleItemIndex(const RECT& contentRect, POINT pt,
                               const std::vector<DesktopItem>& items, FenceViewMode mode);

POINT SnapPointToDesktopGrid(POINT pt);

// Raw desktop icon size in physical pixels (no fence 5/4 display factor).
int GetRawDesktopIconSize(POINT physicalPoint);

// Keeps the same cursor-to-icon offset when a fence item is dragged onto the desktop.
POINT GetDraggedDesktopIconTopLeft(POINT startMouse,
                                   RECT sourceIconRect,
                                   POINT currentMouse,
                                   int targetIconSize);

// 将某个项移动到目标顺序位置（重排序），不涉及坐标
bool ReorderItemToIndex(std::vector<DesktopItem>& items, const std::wstring& itemName, int targetIndex);

}
