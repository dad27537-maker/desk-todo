#pragma once

#include <Windows.h>
#include <array>

enum class CaptureAction {
    None,
    Cancel,
    Pin,
    Save,
    Copy,
    Ocr,
    LatexOcr,
    Move,
    Pen,
    Arrow,
    Text,
    Rectangle,
    Clear
};

struct CaptureToolbarLayout {
    RECT bounds{};
    std::array<RECT, 12> buttons{};
};

struct CapturePropertyLayout {
    RECT bounds{};
    RECT fontButton{};
    std::array<RECT, 5> widthButtons{};
    std::array<RECT, 7> colorButtons{};
};

namespace CaptureToolbar {

inline CaptureToolbarLayout Build(const RECT& selection, const RECT& screen,
                                  int width = 672, int height = 48, int gap = 10) {
    CaptureToolbarLayout layout;
    LONG left = selection.left;
    LONG top = selection.bottom + gap;
    if (top + height > screen.bottom) top = selection.top - gap - height;
    if (top < screen.top) top = screen.top + gap;
    if (left + width > screen.right) left = screen.right - width - gap;
    if (left < screen.left) left = screen.left + gap;
    layout.bounds = { left, top, left + width, top + height };
    const LONG buttonWidth = width / static_cast<LONG>(layout.buttons.size());
    for (size_t index = 0; index < layout.buttons.size(); ++index) {
        layout.buttons[index] = {
            left + static_cast<LONG>(index) * buttonWidth,
            top,
            index + 1 == layout.buttons.size() ? left + width
                                               : left + static_cast<LONG>(index + 1) * buttonWidth,
            top + height
        };
    }
    return layout;
}

inline CaptureAction HitTest(const CaptureToolbarLayout& layout, POINT point) {
    constexpr CaptureAction actions[] = {
        CaptureAction::Cancel, CaptureAction::Pin, CaptureAction::Save,
        CaptureAction::Copy, CaptureAction::Ocr, CaptureAction::LatexOcr,
        CaptureAction::Move,
        CaptureAction::Pen, CaptureAction::Arrow, CaptureAction::Text,
        CaptureAction::Rectangle, CaptureAction::Clear
    };
    for (size_t index = 0; index < layout.buttons.size(); ++index) {
        if (PtInRect(&layout.buttons[index], point)) return actions[index];
    }
    return CaptureAction::None;
}

inline CapturePropertyLayout BuildProperty(const CaptureToolbarLayout& toolbar,
                                           const RECT& screen,
                                           bool showFont,
                                           int height = 46,
                                           int gap = 8) {
    CapturePropertyLayout layout;
    const int width = showFont ? 570 : 470;
    LONG left = toolbar.bounds.left;
    LONG top = toolbar.bounds.bottom + gap;
    if (top + height > screen.bottom) top = toolbar.bounds.top - gap - height;
    if (top < screen.top) top = screen.top + gap;
    if (left + width > screen.right) left = screen.right - width - gap;
    if (left < screen.left) left = screen.left + gap;
    layout.bounds = { left, top, left + width, top + height };

    LONG cursor = left + 10;
    if (showFont) {
        layout.fontButton = { cursor, top + 7, cursor + 132, top + height - 7 };
        cursor += 140;
    }
    for (size_t index = 0; index < layout.widthButtons.size(); ++index) {
        layout.widthButtons[index] = { cursor, top + 7, cursor + 38, top + height - 7 };
        cursor += 40;
    }
    cursor += 8;
    for (size_t index = 0; index < layout.colorButtons.size(); ++index) {
        layout.colorButtons[index] = { cursor, top + 9, cursor + 28, top + height - 9 };
        cursor += 30;
    }
    return layout;
}

}
