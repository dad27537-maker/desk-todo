#pragma once
#include <Windows.h>
#include <algorithm>
#include <cstdint>

namespace WidgetPlacement {
inline RECT Fit(RECT saved, RECT work, SIZE defaults, SIZE minimum, int bottomMargin) {
    const int64_t workWidth = (std::max)(int64_t{1}, int64_t{work.right} - work.left);
    const int64_t workHeight = (std::max)(int64_t{1}, int64_t{work.bottom} - work.top);
    const int64_t oldWidth = int64_t{saved.right} - saved.left;
    const int64_t oldHeight = int64_t{saved.bottom} - saved.top;
    const LONG width = static_cast<LONG>((std::min)(workWidth, oldWidth >= minimum.cx ? oldWidth : defaults.cx));
    const LONG height = static_cast<LONG>((std::min)(workHeight, oldHeight >= minimum.cy ? oldHeight : defaults.cy));
    RECT intersection{};
    if (oldWidth <= 0 || oldHeight <= 0 || !IntersectRect(&intersection, &saved, &work)) {
        saved.left = work.right - width - 28;
        saved.top = work.bottom - height - bottomMargin;
    }
    saved.left = (std::clamp)(saved.left, work.left, work.right - width);
    saved.top = (std::clamp)(saved.top, work.top, work.bottom - height);
    return {saved.left, saved.top, saved.left + width, saved.top + height};
}
inline RECT Restore(RECT saved, SIZE defaults, SIZE minimum, int bottomMargin) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST), &monitor)) {
        monitor.rcWork = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return Fit(saved, monitor.rcWork, defaults, minimum, bottomMargin);
}
}
