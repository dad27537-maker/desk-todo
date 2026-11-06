#pragma once

#include <Windows.h>
#include <algorithm>

namespace CaptureRegion {

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
