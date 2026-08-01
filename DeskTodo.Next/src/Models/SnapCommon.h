#pragma once
#include <string>
#include <vector>
#include <Windows.h>

enum class SnapAxis {
    None,
    Horizontal,
    Vertical
};

enum class SnapType {
    None,

    LeftToLeft,
    LeftToRight,
    RightToLeft,
    RightToRight,

    TopToTop,
    TopToBottom,
    BottomToTop,
    BottomToBottom,

    CenterXToCenterX,
    CenterYToCenterY,

    DesktopLeft,
    DesktopRight,
    DesktopTop,
    DesktopBottom,
    DesktopCenterX,
    DesktopCenterY
};

struct SnapLine {
    bool visible = false;
    SnapAxis axis = SnapAxis::None;
    SnapType type = SnapType::None;
    POINT start = {0, 0};
    POINT end = {0, 0};
    std::wstring sourceFenceId;
    std::wstring targetFenceId;
};

struct SnapCandidate {
    SnapAxis axis = SnapAxis::None;
    SnapType type = SnapType::None;
    int sourceValue = 0;
    int targetValue = 0;
    int distance = 0;
    std::wstring targetFenceId;
    RECT targetRect = {};
};

struct SnapResult {
    bool snappedX = false;
    bool snappedY = false;
    int offsetX = 0;
    int offsetY = 0;
    RECT snappedRect = {};
    std::vector<SnapLine> lines;
};

inline int RectWidth(const RECT& rc) {
    return rc.right - rc.left;
}

inline int RectHeight(const RECT& rc) {
    return rc.bottom - rc.top;
}

inline int RectCenterX(const RECT& rc) {
    return rc.left + RectWidth(rc) / 2;
}

inline int RectCenterY(const RECT& rc) {
    return rc.top + RectHeight(rc) / 2;
}
