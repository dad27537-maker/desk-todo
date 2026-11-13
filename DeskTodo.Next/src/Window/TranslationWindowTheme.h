#pragma once

#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace TranslationTheme {

inline constexpr COLORREF Background = RGB(240, 249, 246);
inline constexpr COLORREF Surface = RGB(255, 255, 255);
inline constexpr COLORREF Text = RGB(25, 57, 51);
inline constexpr COLORREF Muted = RGB(92, 112, 106);
inline constexpr COLORREF Border = RGB(220, 232, 226);
inline constexpr COLORREF Teal = RGB(15, 118, 110);
inline constexpr COLORREF TealSoft = RGB(231, 246, 240);
inline constexpr COLORREF Orange = RGB(194, 65, 12);
inline constexpr COLORREF Error = RGB(180, 48, 48);

enum class State { Idle, Busy, Success, Error };
enum class Icon { Translate, Copy, Swap, Refresh, Shield, Document, Check, Close, Chevron };

struct Fonts {
    HFONT body = nullptr, caption = nullptr, label = nullptr, heading = nullptr, strong = nullptr;
    void Reset(int dpi) {
        Clear();
        auto font = [dpi](int size, int weight) {
            return CreateFontW(-MulDiv(size, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        };
        body = font(16, FW_NORMAL); caption = font(12, FW_NORMAL);
        label = font(13, FW_NORMAL); heading = font(25, FW_SEMIBOLD); strong = font(14, FW_SEMIBOLD);
    }
    void Clear() {
        for (HFONT font : {body, caption, label, heading, strong}) if (font) DeleteObject(font);
        body = caption = label = heading = strong = nullptr;
    }
    ~Fonts() { Clear(); }
};

struct Layout {
    RECT toolbar{}, original{}, translated{}, preview{};
    int width = 0, height = 0;
    static Layout Build(int width, int height) {
        Layout layout;
        layout.width = width; layout.height = height;
        layout.toolbar = {28, 100, width - 28, 204};
        const int column = (width - 72) / 2;
        layout.original = {28, 224, 28 + column, height - 152};
        layout.translated = {44 + column, 224, width - 28, height - 152};
        layout.preview = {28, height - 132, width - 28, height - 52};
        return layout;
    }
};

inline Gdiplus::Color Color(COLORREF value, BYTE alpha = 255) {
    return {alpha, GetRValue(value), GetGValue(value), GetBValue(value)};
}
inline COLORREF Mix(COLORREF a, COLORREF b, float amount) {
    auto channel = [amount](int x, int y) { return static_cast<BYTE>(x + (y - x) * amount); };
    return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)), channel(GetBValue(a), GetBValue(b)));
}
inline RECT Pixels(RECT rect, int dpi) {
    return {MulDiv(rect.left, dpi, 96), MulDiv(rect.top, dpi, 96),
            MulDiv(rect.right, dpi, 96), MulDiv(rect.bottom, dpi, 96)};
}
inline void Round(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF stroke = CLR_INVALID) {
    using namespace Gdiplus;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const REAL x = static_cast<REAL>(rect.left) + .5f, y = static_cast<REAL>(rect.top) + .5f;
    const REAL width = static_cast<REAL>(rect.right - rect.left) - 1, height = static_cast<REAL>(rect.bottom - rect.top) - 1;
    const REAL diameter = (std::min)(static_cast<REAL>(radius * 2), (std::min)(width, height));
    GraphicsPath path;
    path.AddArc(x, y, diameter, diameter, 180, 90);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270, 90);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0, 90);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90, 90);
    path.CloseFigure();
    SolidBrush brush(Color(fill));
    graphics.FillPath(&brush, &path);
    if (stroke != CLR_INVALID) { Pen pen(Color(stroke)); graphics.DrawPath(&pen, &path); }
}
inline void Label(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
                  UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, font);
    const auto oldColor = SetTextColor(dc, color);
    const auto oldMode = SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags | DT_NOPREFIX);
    SetBkMode(dc, oldMode); SetTextColor(dc, oldColor); SelectObject(dc, old);
}
inline void DrawIcon(HDC dc, Icon icon, RECT rect, COLORREF color) {
    using namespace Gdiplus;
    Graphics graphics(dc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.TranslateTransform(static_cast<REAL>(rect.left), static_cast<REAL>(rect.top));
    graphics.ScaleTransform((rect.right - rect.left) / 24.f, (rect.bottom - rect.top) / 24.f);
    Pen pen(Color(color), 1.65f);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    auto line = [&](REAL a, REAL b, REAL c, REAL d) { graphics.DrawLine(&pen, a, b, c, d); };
    switch (icon) {
    case Icon::Copy:
        graphics.DrawRectangle(&pen, 8.f, 8.f, 12.f, 13.f);
        line(4, 16, 4, 3); line(4, 3, 16, 3); break;
    case Icon::Swap:
        line(4, 7, 20, 7); line(16, 3, 20, 7); line(20, 7, 16, 11);
        line(20, 17, 4, 17); line(8, 13, 4, 17); line(4, 17, 8, 21); break;
    case Icon::Refresh:
        graphics.DrawArc(&pen, 4.f, 4.f, 16.f, 16.f, 35.f, 285.f);
        line(19, 3, 19, 9); line(19, 9, 13, 9); break;
    case Icon::Shield: {
        PointF points[]{{12, 2}, {20, 6}, {19, 15}, {16, 19}, {12, 22}, {8, 19}, {5, 15}, {4, 6}, {12, 2}};
        graphics.DrawLines(&pen, points, 9);
        line(8, 12, 11, 15); line(11, 15, 16, 9); break;
    }
    case Icon::Document:
        line(6, 2, 15, 2); line(15, 2, 20, 7); line(20, 7, 20, 22);
        line(20, 22, 4, 22); line(4, 22, 4, 2); line(4, 2, 6, 2);
        line(15, 2, 15, 8); line(15, 8, 20, 8); line(8, 12, 15, 12); line(8, 17, 15, 17); break;
    case Icon::Check: line(5, 12, 10, 17); line(10, 17, 20, 6); break;
    case Icon::Close: line(6, 6, 18, 18); line(18, 6, 6, 18); break;
    case Icon::Chevron: line(7, 10, 12, 15); line(12, 15, 17, 10); break;
    case Icon::Translate:
        line(3, 5, 14, 5); line(8, 2, 8, 5); line(12, 5, 11, 10); line(11, 10, 4, 16);
        line(5, 8, 12, 14); line(13, 21, 18, 9); line(18, 9, 23, 21); line(15, 17, 21, 17); break;
    }
}
}
