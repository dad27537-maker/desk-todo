#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstdlib>

namespace CaptureRegion {

enum ResizeEdge : unsigned {
    None = 0, Left = 1, Right = 2, Top = 4, Bottom = 8
};

inline POINT ClampPoint(POINT point, const RECT& bounds) {
    return {(std::clamp)(point.x, bounds.left, bounds.right),
            (std::clamp)(point.y, bounds.top, bounds.bottom)};
}

inline unsigned HitTestResize(const RECT& region, POINT point, int margin = 6) {
    if (point.x < region.left - margin || point.x > region.right + margin ||
        point.y < region.top - margin || point.y > region.bottom + margin) return None;
    unsigned edges = None;
    if (std::abs(point.x - region.left) <= margin &&
        std::abs(point.x - region.left) <= std::abs(point.x - region.right)) edges |= Left;
    else if (std::abs(point.x - region.right) <= margin) edges |= Right;
    if (std::abs(point.y - region.top) <= margin &&
        std::abs(point.y - region.top) <= std::abs(point.y - region.bottom)) edges |= Top;
    else if (std::abs(point.y - region.bottom) <= margin) edges |= Bottom;
    return edges;
}

inline RECT Resize(const RECT& initial, unsigned edges, POINT delta, const RECT& bounds) {
    constexpr LONG minimum = 6;
    RECT result = initial;
    if (edges & Left) result.left = (std::clamp)(initial.left + delta.x, bounds.left, initial.right - minimum);
    if (edges & Right) result.right = (std::clamp)(initial.right + delta.x, initial.left + minimum, bounds.right);
    if (edges & Top) result.top = (std::clamp)(initial.top + delta.y, bounds.top, initial.bottom - minimum);
    if (edges & Bottom) result.bottom = (std::clamp)(initial.bottom + delta.y, initial.top + minimum, bounds.bottom);
    return result;
}

inline RECT Normalize(POINT start, POINT end) {
    return { (std::min)(start.x, end.x), (std::min)(start.y, end.y),
             (std::max)(start.x, end.x), (std::max)(start.y, end.y) };
}

inline bool IsValid(const RECT& region) {
    return region.right - region.left >= 6 && region.bottom - region.top >= 6;
}

inline SIZE FitPreview(const RECT& region, int maxWidth = 800, int maxHeight = 600) {
    const int width = (std::max)(1L, region.right - region.left);
    const int height = (std::max)(1L, region.bottom - region.top);
    const double scale = (std::min)(1.0,
        (std::min)(static_cast<double>(maxWidth) / width,
                   static_cast<double>(maxHeight) / height));
    return { (std::max)(1, static_cast<int>(width * scale)),
             (std::max)(1, static_cast<int>(height * scale)) };
}

}
