#pragma once

#include <Windows.h>
#include <algorithm>
#include <cmath>

namespace SettingsWindowLayout {

constexpr int BaseWidth = 760;
constexpr int BaseHeight = 620;
constexpr int WorkAreaMargin = 16;

struct Placement {
    RECT rect{};
    float scale = 1.0f;
};

inline Placement FitToWorkAreaForBase(const RECT& workArea, POINT preferredCenter,
                                      int baseWidth, int baseHeight) {
    const int availableWidth = (std::max)(1L,
        workArea.right - workArea.left - WorkAreaMargin * 2);
    const int availableHeight = (std::max)(1L,
        workArea.bottom - workArea.top - WorkAreaMargin * 2);
    const float widthScale = static_cast<float>(availableWidth) / baseWidth;
    const float heightScale = static_cast<float>(availableHeight) / baseHeight;
    const float scale = (std::clamp)((std::min)(1.0f,
        (std::min)(widthScale, heightScale)), 0.55f, 1.0f);
    const int width = (std::min)(availableWidth,
        static_cast<int>(std::lround(baseWidth * scale)));
    const int height = (std::min)(availableHeight,
        static_cast<int>(std::lround(baseHeight * scale)));
    int left = preferredCenter.x - width / 2;
    int top = preferredCenter.y - height / 2;
    const int minimumLeft = static_cast<int>(workArea.left) + WorkAreaMargin;
    const int maximumLeft = static_cast<int>(workArea.right) - WorkAreaMargin - width;
    const int minimumTop = static_cast<int>(workArea.top) + WorkAreaMargin;
    const int maximumTop = static_cast<int>(workArea.bottom) - WorkAreaMargin - height;
    left = (std::clamp)(left, minimumLeft, maximumLeft);
    top = (std::clamp)(top, minimumTop, maximumTop);
    return { { left, top, left + width, top + height }, scale };
}

inline Placement FitToWorkArea(const RECT& workArea, POINT preferredCenter) {
    return FitToWorkAreaForBase(workArea, preferredCenter, BaseWidth, BaseHeight);
}

inline int Scale(int value, float scale) {
    return static_cast<int>(std::lround(value * scale));
}

} // namespace SettingsWindowLayout
