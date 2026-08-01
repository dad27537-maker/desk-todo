#include "FenceRenderer.h"
#include "Services/IconCache.h"
#include "Services/GridLayoutService.h"
#include <algorithm>
#include <cmath>
#include <string>

#define SafeRelease(p) if (p) { p->Release(); p = nullptr; }

static constexpr int GHOST_ICON_SIZE = 40;

ID2D1Factory* FenceRenderer::s_d2dFactory = nullptr;
IDWriteFactory* FenceRenderer::s_dwriteFactory = nullptr;
IDWriteTextFormat* FenceRenderer::s_titleFormat = nullptr;
IDWriteTextFormat* FenceRenderer::s_bodyFormat = nullptr;
IDWriteTextFormat* FenceRenderer::s_itemTextFormat = nullptr;
IDWriteTextFormat* FenceRenderer::s_listTextFormat = nullptr;
IDWriteTextFormat* FenceRenderer::s_ghostTextFormat = nullptr;
IDWriteTextFormat* FenceRenderer::s_countFormat = nullptr;
IDWriteTextFormat* FenceRenderer::s_inputFormat = nullptr;
int FenceRenderer::s_refCount = 0;

FenceRenderer::FenceRenderer() {}
FenceRenderer::~FenceRenderer() { DestroyRenderTarget(); }

bool FenceRenderer::EnsureFactories() {
    if (s_refCount++ > 0) return true;

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &s_d2dFactory);
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&s_dwriteFactory));
    if (FAILED(hr)) return false;

    hr = s_dwriteFactory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-CN", &s_titleFormat);
    if (FAILED(hr)) return false;
    s_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    s_titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s_titleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    hr = s_dwriteFactory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", &s_bodyFormat);
    if (FAILED(hr)) return false;
    s_bodyFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    s_bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    hr = s_dwriteFactory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"zh-CN", &s_itemTextFormat);
    if (FAILED(hr)) return false;
    s_itemTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    s_itemTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s_itemTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);

    hr = s_dwriteFactory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 23.0f, L"zh-CN", &s_listTextFormat);
    if (FAILED(hr)) return false;
    s_listTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    s_listTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s_listTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    hr = s_dwriteFactory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", &s_ghostTextFormat);
    if (FAILED(hr)) return false;
    s_ghostTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    s_ghostTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    hr = s_dwriteFactory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-CN", &s_countFormat);
    if (FAILED(hr)) return false;
    s_countFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    s_countFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // 输入框 / 列表模式文字格式（左对齐）
    hr = s_dwriteFactory->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", &s_inputFormat);
    if (FAILED(hr)) return false;
    s_inputFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    s_inputFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    s_inputFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    return true;
}

void FenceRenderer::ReleaseFactories() {
    if (--s_refCount > 0) return;
    SafeRelease(s_inputFormat);
    SafeRelease(s_countFormat);
    SafeRelease(s_ghostTextFormat);
    SafeRelease(s_itemTextFormat);
    SafeRelease(s_listTextFormat);
    SafeRelease(s_bodyFormat);
    SafeRelease(s_titleFormat);
    SafeRelease(s_dwriteFactory);
    SafeRelease(s_d2dFactory);
}

bool FenceRenderer::CreateRenderTarget(HDC hdc, int w, int h) {
    if (renderTarget && width == w && height == h)
        return true;

    DestroyRenderTarget();

    RECT rc = { 0, 0, w, h };
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f,
        96.0f);

    HRESULT hr = s_d2dFactory->CreateDCRenderTarget(&props, &renderTarget);
    if (FAILED(hr)) return false;

    hr = renderTarget->BindDC(hdc, &rc);
    if (FAILED(hr)) { SafeRelease(renderTarget); return false; }

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.94f, 1.0f, 0.98f, 0.98f), &titleTextBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.78f, 0.94f, 0.89f, 0.34f), &borderBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.35f, 0.86f, 0.75f, 0.96f), &activeBorderBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 0.70f, 0.62f, 0.72f), &hoverBorderBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.35f, 0.86f, 0.75f, 1.0f), &movingBorderBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 0.76f, 0.69f, 0.18f), &itemHoverBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.35f, 0.86f, 0.75f, 0.30f), &itemSelectedBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.98f, 0.99f, 1.0f, 0.98f), &itemTextBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.35f, 0.86f, 0.75f, 0.28f), &dropTargetBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.06f, 0.09f, 0.12f, 0.72f), &ghostBgBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.95f, 0.97f, 1.0f, 0.96f), &inputBgBrush);
    if (FAILED(hr)) return false;

    hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.2f, 0.2f, 0.25f, 1.0f), &inputCursorBrush);
    if (FAILED(hr)) return false;

    width = w;
    height = h;
    return true;
}

void FenceRenderer::DestroyRenderTarget() {
    if (iconCache) iconCache->ClearBitmaps();
    SafeRelease(inputCursorBrush);
    SafeRelease(inputBgBrush);
    SafeRelease(ghostBgBrush);
    SafeRelease(dropTargetBrush);
    SafeRelease(itemTextBrush);
    SafeRelease(itemSelectedBrush);
    SafeRelease(itemHoverBrush);
    SafeRelease(movingBorderBrush);
    SafeRelease(hoverBorderBrush);
    SafeRelease(activeBorderBrush);
    SafeRelease(borderBrush);
    SafeRelease(titleTextBrush);
    SafeRelease(blackHoleBitmap);
    SafeRelease(renderTarget);
    width = 0;
    height = 0;
}

void FenceRenderer::BeginFrame(const RECT* dirtyRect) {
    if (!renderTarget) return;
    renderTarget->BeginDraw();
    renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
    frameClipActive = dirtyRect &&
        dirtyRect->right > dirtyRect->left &&
        dirtyRect->bottom > dirtyRect->top;
    if (frameClipActive) {
        renderTarget->PushAxisAlignedClip(
            D2D1::RectF(
                static_cast<float>(dirtyRect->left),
                static_cast<float>(dirtyRect->top),
                static_cast<float>(dirtyRect->right),
                static_cast<float>(dirtyRect->bottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
    } else {
        renderTarget->Clear(
            D2D1::ColorF(0, 0, 0, 0));
    }
}

void FenceRenderer::DrawBlackHoleBackground(const uint8_t* pixels, uint32_t w, uint32_t h) {
    if (!renderTarget || !pixels || w == 0 || h == 0) return;

    // Recreate the bitmap if dimensions changed.
    if (!blackHoleBitmap) {
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        D2D1_SIZE_U size = { w, h };
        renderTarget->CreateBitmap(size, pixels, w * 4, &props, &blackHoleBitmap);
    } else {
        D2D1_SIZE_U bmpSize = blackHoleBitmap->GetPixelSize();
        if (bmpSize.width != w || bmpSize.height != h) {
            blackHoleBitmap->Release();
            blackHoleBitmap = nullptr;
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f, 96.0f);
            D2D1_SIZE_U size = { w, h };
            renderTarget->CreateBitmap(size, pixels, w * 4, &props, &blackHoleBitmap);
        } else {
            D2D1_RECT_U dstRect = { 0, 0, w, h };
            blackHoleBitmap->CopyFromMemory(&dstRect, pixels, w * 4);
        }
    }

    if (blackHoleBitmap) {
        // Scale to fill the entire render target.
        D2D1_SIZE_F targetSize = renderTarget->GetSize();
        D2D1_RECT_F dst = D2D1::RectF(0.0f, 0.0f, targetSize.width, targetSize.height);
        renderTarget->DrawBitmap(blackHoleBitmap, dst, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
}

// 缓动函数：带回弹的 ease-out
static float EaseOutBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

void FenceRenderer::DrawLockIconWithAnimation(const Fence& fence, float cx, float cy, float baseSize) {
    if (!titleTextBrush || !renderTarget) return;

    // 计算动画进度
    float progress = 1.0f;
    if (fence.lockAnimActive) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG elapsed = now - fence.lockAnimStartTime;
        const ULONGLONG duration = 450;
        if (elapsed >= duration) {
            progress = 1.0f;
            fence.lockAnimActive = false;
        } else {
            progress = (float)elapsed / (float)duration;
        }
        fence.lockAnimProgress = progress;
    }

    float scale = 1.0f;
    float rotation = 0.0f;
    float bgAlpha = 0.0f;

    if (fence.lockAnimActive && progress < 1.0f) {
        // 缩放：0.4 → 1.3 → 1.0（带回弹）
        if (progress < 0.5f) {
            scale = 0.4f + 0.9f * (progress / 0.5f);
        } else {
            scale = 1.3f - 0.3f * EaseOutBack((progress - 0.5f) / 0.5f);
            if (scale < 1.0f) scale = 1.0f + (scale - 1.0f) * 0.5f;
        }
        // 旋转：180° → 0°
        rotation = 180.0f * (1.0f - progress);
        // 背景高亮：渐隐
        bgAlpha = 0.6f * (1.0f - progress);
    }

    // 绘制背景高亮圆
    if (bgAlpha > 0.01f) {
        ID2D1SolidColorBrush* hlBrush = nullptr;
        renderTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.36f, 0.78f, 1.0f, bgAlpha), &hlBrush);
        if (hlBrush) {
            D2D1_ELLIPSE halo{ D2D1::Point2F(cx, cy), baseSize * 1.6f, baseSize * 1.6f };
            renderTarget->FillEllipse(&halo, hlBrush);
            hlBrush->Release();
        }
    }

    // 应用变换
    renderTarget->SetTransform(
        D2D1::Matrix3x2F::Rotation(rotation, D2D1::Point2F(cx, cy)) *
        D2D1::Matrix3x2F::Scale(D2D1::SizeF(scale, scale), D2D1::Point2F(cx, cy))
    );

    // 绘制锁体
    float halfW = baseSize * 0.55f;
    float bodyTop = cy - baseSize * 0.05f;
    float bodyBottom = cy + baseSize * 0.7f;
    renderTarget->DrawRectangle(
        D2D1::RectF(cx - halfW, bodyTop, cx + halfW, bodyBottom),
        titleTextBrush, fence.locked ? 1.8f : 1.0f);

    // 绘制锁扣（弧形）
    ID2D1PathGeometry* lockPath = nullptr;
    ID2D1GeometrySink* lockSink = nullptr;
    if (SUCCEEDED(s_d2dFactory->CreatePathGeometry(&lockPath)) &&
        SUCCEEDED(lockPath->Open(&lockSink))) {
        float arcR = halfW * 0.9f;
        lockSink->BeginFigure(D2D1::Point2F(cx - arcR, bodyTop), D2D1_FIGURE_BEGIN_HOLLOW);
        D2D1_ARC_SEGMENT lockArc{
            D2D1::Point2F(cx + arcR, bodyTop), D2D1::SizeF(arcR, arcR), 0,
            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL
        };
        lockSink->AddArc(lockArc);
        lockSink->EndFigure(D2D1_FIGURE_END_OPEN);
        lockSink->Close();
        renderTarget->DrawGeometry(lockPath, titleTextBrush, fence.locked ? 1.8f : 1.0f);
    }
    SafeRelease(lockSink);
    SafeRelease(lockPath);

    renderTarget->SetTransform(D2D1::Matrix3x2F::Identity());
}

void FenceRenderer::DrawViewToggleIcon(float cx, float cy, bool isListMode) {
    if (!titleTextBrush || !renderTarget) return;
    if (isListMode) {
        // 列表模式图标：3 条水平线 + 小方块
        for (int i = 0; i < 3; ++i) {
            float y = cy - 6 + i * 6;
            renderTarget->DrawRectangle(
                D2D1::RectF(cx - 8, y - 1, cx - 4, y + 2), titleTextBrush, 1.0f);
            renderTarget->DrawLine(
                D2D1::Point2F(cx - 2, y), D2D1::Point2F(cx + 8, y), titleTextBrush, 1.0f);
        }
    } else {
        // 图标模式：2x2 网格
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                float x = cx - 7 + col * 7.0f;
                float y = cy - 7 + row * 7.0f;
                renderTarget->DrawRectangle(
                    D2D1::RectF(x, y, x + 5, y + 5), titleTextBrush, 0.9f);
            }
        }
    }
}

void FenceRenderer::DrawSearchIcon(float cx, float cy, bool active) {
    if (!titleTextBrush || !renderTarget) return;
    // 放大镜：圆 + 手柄（用 DrawEllipse 避免全弧枚举兼容问题）
    const float r = 5.0f;
    const D2D1_ELLIPSE ellipse{ D2D1::Point2F(cx - 1, cy - 1), r, r };
    renderTarget->DrawEllipse(&ellipse, titleTextBrush, active ? 1.8f : 1.2f);
    // 手柄
    renderTarget->DrawLine(
        D2D1::Point2F(cx + 3, cy + 3), D2D1::Point2F(cx + 7, cy + 7),
        titleTextBrush, active ? 1.8f : 1.2f);
}

void FenceRenderer::DrawGameFenceCard(const Fence& fence, bool isActive, bool isHovered,
                                      bool isMoving, bool isDragSource,
                                      int draggedItemIndex, POINT windowOrigin) {
    if (!renderTarget) return;

    const float fx = static_cast<float>(fence.rect.left - windowOrigin.x);
    const float fy = static_cast<float>(fence.rect.top - windowOrigin.y);
    const float fw = static_cast<float>(fence.rect.right - fence.rect.left);
    const float fh = static_cast<float>(fence.rect.bottom - fence.rect.top);
    const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
        D2D1::RectF(fx, fy, fx + fw, fy + fh), 16.0f, 16.0f);

    ID2D1SolidColorBrush* background = nullptr;
    renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.29f, 0.29f, 0.30f,
                     (std::clamp)(fence.opacity, 0.56f, 0.78f)),
        &background);
    if (background) {
        renderTarget->FillRoundedRectangle(&card, background);
        background->Release();
    }

    ID2D1SolidColorBrush* edge = borderBrush;
    float edgeWidth = 1.0f;
    if (isMoving) {
        edge = movingBorderBrush;
        edgeWidth = 2.0f;
    } else if (isActive) {
        edge = activeBorderBrush;
        edgeWidth = 1.5f;
    } else if (isHovered) {
        edge = hoverBorderBrush;
    }
    if (edge) renderTarget->DrawRoundedRectangle(&card, edge, edgeWidth);

    if (fence.dropTarget && dropTargetBrush) {
        const D2D1_ROUNDED_RECT dropRect = D2D1::RoundedRect(
            D2D1::RectF(fx + 2.0f, fy + 2.0f, fx + fw - 2.0f, fy + fh - 2.0f),
            14.0f, 14.0f);
        renderTarget->DrawRoundedRectangle(&dropRect, dropTargetBrush, 2.0f);
    }

    if (fence.items.empty()) {
        if (titleTextBrush) {
            const float plusX = fx + fw * 0.5f;
            const float plusY = fy + fh * 0.42f;
            renderTarget->DrawLine({ plusX - 10.0f, plusY },
                                   { plusX + 10.0f, plusY },
                                   titleTextBrush, 3.0f);
            renderTarget->DrawLine({ plusX, plusY - 10.0f },
                                   { plusX, plusY + 10.0f },
                                   titleTextBrush, 3.0f);
        }

        if (s_itemTextFormat && titleTextBrush) {
            const std::wstring hint = L"拖入游戏快\n捷方式";
            const D2D1_RECT_F hintRect = D2D1::RectF(
                fx + 7.0f, fy + fh - 82.0f, fx + fw - 7.0f, fy + fh - 8.0f);
            renderTarget->DrawText(hint.c_str(), static_cast<UINT32>(hint.size()),
                                   s_itemTextFormat, &hintRect, titleTextBrush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }

    // A game container accepts one launcher. Keep the icon/name renderer, but
    // use the compact card as its viewport instead of the old title-bar card.
    const auto& launcher = fence.items.front();
    if (!launcher.filtered && !(isDragSource && draggedItemIndex == 0)) {
        ID2D1Bitmap* bitmap = iconCache
            ? iconCache->GetOrCreateBitmap(launcher, renderTarget) : nullptr;
        if (bitmap) {
            const D2D1_RECT_F iconRect = {
                static_cast<float>(launcher.iconRect.left - windowOrigin.x),
                static_cast<float>(launcher.iconRect.top - windowOrigin.y),
                static_cast<float>(launcher.iconRect.right - windowOrigin.x),
                static_cast<float>(launcher.iconRect.bottom - windowOrigin.y)
            };
            renderTarget->DrawBitmap(bitmap, iconRect, 1.0f,
                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
}

void FenceRenderer::DrawClockFenceCard(const Fence& fence, bool isActive,
                                       bool isHovered, bool isMoving,
                                       POINT windowOrigin) {
    if (!renderTarget || !s_dwriteFactory) return;

    const float fx = static_cast<float>(fence.rect.left - windowOrigin.x);
    const float fy = static_cast<float>(fence.rect.top - windowOrigin.y);
    const float fw = static_cast<float>(fence.rect.right - fence.rect.left);
    const float fh = static_cast<float>(fence.rect.bottom - fence.rect.top);
    const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
        D2D1::RectF(fx, fy, fx + fw, fy + fh), 16.0f, 16.0f);

    ID2D1SolidColorBrush* background = nullptr;
    renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.93f, 0.90f, 0.91f,
                     (std::clamp)(fence.opacity, 0.62f, 0.90f)),
        &background);
    if (background) {
        renderTarget->FillRoundedRectangle(&card, background);
        background->Release();
    }

    ID2D1SolidColorBrush* edge = borderBrush;
    float edgeWidth = 1.0f;
    if (isMoving) {
        edge = movingBorderBrush;
        edgeWidth = 2.0f;
    } else if (isActive) {
        edge = activeBorderBrush;
        edgeWidth = 1.5f;
    } else if (isHovered) {
        edge = hoverBorderBrush;
    }
    if (edge) renderTarget->DrawRoundedRectangle(&card, edge, edgeWidth);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::wstring timeText =
        std::wstring(now.wHour < 10 ? L"0" : L"") + std::to_wstring(now.wHour) + L":" +
        std::wstring(now.wMinute < 10 ? L"0" : L"") + std::to_wstring(now.wMinute);
    const std::wstring secondsText =
        std::wstring(L":") + std::wstring(now.wSecond < 10 ? L"0" : L"") +
        std::to_wstring(now.wSecond);
    static constexpr const wchar_t* weekdays[] = {
        L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"
    };
    const std::wstring dateText =
        std::to_wstring(now.wYear) + L"/" +
        std::wstring(now.wMonth < 10 ? L"0" : L"") + std::to_wstring(now.wMonth) + L"/" +
        std::wstring(now.wDay < 10 ? L"0" : L"") + std::to_wstring(now.wDay) + L" " +
        weekdays[(std::min)(6, static_cast<int>(now.wDayOfWeek))];

    auto drawText = [&](const std::wstring& text, const D2D1_RECT_F& rect,
                        float fontSize, DWRITE_FONT_WEIGHT weight,
                        D2D1_COLOR_F color,
                        DWRITE_TEXT_ALIGNMENT alignment) {
        IDWriteTextFormat* format = nullptr;
        if (FAILED(s_dwriteFactory->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, fontSize, L"zh-CN", &format)) ||
            !format) {
            return;
        }
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ID2D1SolidColorBrush* brush = nullptr;
        renderTarget->CreateSolidColorBrush(color, &brush);
        if (brush) {
            renderTarget->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
                                   format, &rect, brush,
                                   D2D1_DRAW_TEXT_OPTIONS_CLIP);
            brush->Release();
        }
        format->Release();
    };

    const float scale = (std::clamp)(
        (std::min)(fw / static_cast<float>(CLOCK_FENCE_WIDTH),
                   fh / static_cast<float>(CLOCK_FENCE_HEIGHT)),
        0.50f, 2.0f);
    const float timeSize = 88.0f * scale;
    const float secondsSize = 47.0f * scale;
    const float dateSize = 28.0f * scale;
    drawText(timeText,
             D2D1::RectF(fx + fw * 0.12f, fy + fh * 0.20f,
                         fx + fw * 0.72f, fy + fh * 0.63f),
             timeSize, DWRITE_FONT_WEIGHT_LIGHT,
             D2D1::ColorF(0.06f, 0.06f, 0.07f, 0.96f),
             DWRITE_TEXT_ALIGNMENT_CENTER);
    drawText(secondsText,
             D2D1::RectF(fx + fw * 0.67f, fy + fh * 0.34f,
                         fx + fw * 0.96f, fy + fh * 0.64f),
             secondsSize, DWRITE_FONT_WEIGHT_LIGHT,
             D2D1::ColorF(0.38f, 0.37f, 0.39f, 0.84f),
             DWRITE_TEXT_ALIGNMENT_LEADING);
    drawText(dateText,
             D2D1::RectF(fx + fw * 0.12f, fy + fh * 0.62f,
                         fx + fw * 0.88f, fy + fh * 0.86f),
             dateSize, DWRITE_FONT_WEIGHT_NORMAL,
             D2D1::ColorF(0.43f, 0.42f, 0.44f, 0.76f),
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void FenceRenderer::DrawFence(const Fence& fence, bool isActive, bool isHovered, bool isMoving,
                               bool isDragSource, int draggedItemIndex, POINT windowOrigin) {
    if (!renderTarget) return;

    if (fence.kind == FenceKind::Game) {
        DrawGameFenceCard(fence, isActive, isHovered, isMoving,
                          isDragSource, draggedItemIndex, windowOrigin);
        return;
    }
    if (fence.kind == FenceKind::Clock) {
        DrawClockFenceCard(fence, isActive, isHovered, isMoving, windowOrigin);
        return;
    }

    float r = GetRValue(fence.color) / 255.0f;
    float g = GetGValue(fence.color) / 255.0f;
    float b = GetBValue(fence.color) / 255.0f;
    float a = (std::min)(fence.opacity, 0.78f);
    const bool gameFence = fence.kind == FenceKind::Game;
    if (gameFence) {
        r = 0.035f;
        g = 0.235f;
        b = 0.215f;
        a = (std::max)(0.82f, fence.opacity);
    }

    float fx = (float)(fence.rect.left - windowOrigin.x);
    float fy = (float)(fence.rect.top - windowOrigin.y);
    float fw = (float)(fence.rect.right - fence.rect.left);
    float fh = (float)(fence.rect.bottom - fence.rect.top);

    const float cornerRadius = gameFence ? 16.0f : 7.0f;
    D2D1_ROUNDED_RECT outerRect = D2D1::RoundedRect(
        D2D1_RECT_F{ fx, fy, fx + fw, fy + fh }, cornerRadius, cornerRadius);

    ID2D1SolidColorBrush* bgBrush = nullptr;
    renderTarget->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &bgBrush);
    renderTarget->FillRoundedRectangle(&outerRect, bgBrush);
    SafeRelease(bgBrush);

    ID2D1SolidColorBrush* edgeBrush = borderBrush;
    float edgeWidth = 1.0f;
    if (isMoving) {
        edgeBrush = movingBorderBrush;
        edgeWidth = 2.0f;
    } else if (isActive) {
        edgeBrush = activeBorderBrush;
        edgeWidth = 1.5f;
    } else if (isHovered) {
        edgeBrush = hoverBorderBrush;
    }
    renderTarget->DrawRoundedRectangle(&outerRect, edgeBrush, edgeWidth);

    float titleH = (float)FENCE_TITLE_BAR_HEIGHT;
    float titleA = (std::min)(fence.opacity + 0.14f, 0.92f);

    ID2D1SolidColorBrush* titleBrush = nullptr;
    if (isMoving) {
        renderTarget->CreateSolidColorBrush(
            D2D1::ColorF((std::min)(r + 0.10f, 1.0f), (std::min)(g + 0.12f, 1.0f),
                          (std::min)(b + 0.14f, 1.0f), titleA), &titleBrush);
    } else if (isActive || isHovered) {
        renderTarget->CreateSolidColorBrush(
            D2D1::ColorF((std::min)(r + 0.07f, 1.0f), (std::min)(g + 0.08f, 1.0f),
                          (std::min)(b + 0.10f, 1.0f), titleA), &titleBrush);
    } else {
        renderTarget->CreateSolidColorBrush(
            D2D1::ColorF((std::max)(r - 0.06f, 0.0f), (std::max)(g - 0.06f, 0.0f),
                          (std::max)(b - 0.06f, 0.0f), titleA), &titleBrush);
    }

    D2D1_ROUNDED_RECT topRound = D2D1::RoundedRect(
        D2D1_RECT_F{ fx, fy, fx + fw, fy + titleH }, 7.0f, 7.0f);
    renderTarget->FillRoundedRectangle(&topRound, titleBrush);
    SafeRelease(titleBrush);

    if (titleTextBrush) {
        ID2D1SolidColorBrush* lineBrush = (isMoving || isActive) ? activeBorderBrush : borderBrush;
        renderTarget->DrawLine(
            D2D1::Point2F(fx + 2, fy + titleH),
            D2D1::Point2F(fx + fw - 3, fy + titleH),
            lineBrush, 0.5f);
    }

    // 标题文字（居中于左侧按钮和右侧按钮之间）
    const bool compactTitle = fw < 240.0f;
    if (s_titleFormat && titleTextBrush) {
        D2D1_RECT_F titleTextRect = gameFence
            ? D2D1_RECT_F{ fx + 72, fy, fx + fw - 42, fy + titleH }
            : (compactTitle
                ? D2D1_RECT_F{ fx + 34, fy, fx + fw - 70, fy + titleH }
                : D2D1_RECT_F{ fx + 72, fy, fx + fw - 112, fy + titleH });
        renderTarget->DrawText(fence.title.c_str(), (UINT32)fence.title.size(),
                               s_titleFormat, &titleTextRect, titleTextBrush,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (titleTextBrush) {
        const float cy = fy + titleH / 2.0f;

        // 左侧：折叠箭头
        const float leftChevron = fx + 16.0f;
        if (fence.collapsed) {
            renderTarget->DrawLine({ leftChevron - 4, cy - 3 }, { leftChevron, cy + 1 }, titleTextBrush, 1.2f);
            renderTarget->DrawLine({ leftChevron, cy + 1 }, { leftChevron + 4, cy - 3 }, titleTextBrush, 1.2f);
        } else {
            renderTarget->DrawLine({ leftChevron - 4, cy + 2 }, { leftChevron, cy - 2 }, titleTextBrush, 1.2f);
            renderTarget->DrawLine({ leftChevron, cy - 2 }, { leftChevron + 4, cy + 2 }, titleTextBrush, 1.2f);
        }

        // 左侧：锁图标（带动画）
        if (!compactTitle || gameFence) {
            DrawLockIconWithAnimation(fence, fx + 48.0f, cy, 12.0f);

            // Game cards have one launcher, so grid/list switching is not useful.
            if (!gameFence) {
                DrawViewToggleIcon(fx + fw - 86.0f, cy,
                                   fence.viewMode == FenceViewMode::List);
            }
        }
        if (!gameFence) {
            DrawSearchIcon(fx + fw - 52.0f, cy, fence.searchActive);
        }

        // 右侧：菜单按钮（三横线）
        const float menuX = fx + fw - 18.0f;
        renderTarget->DrawLine({ menuX - 6, cy - 5 }, { menuX + 6, cy - 5 }, titleTextBrush, 1.0f);
        renderTarget->DrawLine({ menuX - 6, cy }, { menuX + 6, cy }, titleTextBrush, 1.0f);
        renderTarget->DrawLine({ menuX - 6, cy + 5 }, { menuX + 6, cy + 5 }, titleTextBrush, 1.0f);
    }

    if (!fence.collapsed && fence.dropTarget && dropTargetBrush) {
        D2D1_RECT_F dropRect = { fx + 2, fy + titleH + 2, fx + fw - 2, fy + fh - 2 };
        renderTarget->FillRectangle(&dropRect, dropTargetBrush);
    }

    if (fence.collapsed) {
        return;
    }

    const float viewportTop = fy + titleH + (fence.searchActive ? 36.0f : 0.0f);
    const float viewportBottom = fy + fh - 1.0f;
    if (viewportBottom <= viewportTop) return;
    renderTarget->PushAxisAlignedClip(
        D2D1::RectF(fx + 1.0f, viewportTop, fx + fw - 1.0f, viewportBottom),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // 搜索激活时：如果有搜索词但无可见项，显示"未找到匹配文件"
    bool hasVisible = false;
    for (const auto& item : fence.items) {
        if (!item.filtered) { hasVisible = true; break; }
    }

    if (fence.items.empty()) {
        DrawEmptyText(fence, windowOrigin);
    } else if (!hasVisible && fence.searchActive && !fence.searchQuery.empty()) {
        DrawNoMatchText(fence, windowOrigin);
    } else {
        for (size_t i = 0; i < fence.items.size(); ++i) {
            const auto& item = fence.items[i];
            if (item.filtered) continue; // 跳过被搜索过滤的项

            float opacity = 1.0f;
            if (isDragSource && (int)i == draggedItemIndex) {
                opacity = 0.0f;
            }

            ID2D1Bitmap* bmp = iconCache
                ? iconCache->GetOrCreateBitmap(item, renderTarget)
                : nullptr;
            DrawItem(item, bmp, windowOrigin, opacity);
        }
    }
    renderTarget->PopAxisAlignedClip();

    if (gameFence && !fence.items.empty() && s_bodyFormat && titleTextBrush) {
        const std::wstring hint = L"\u53cc\u51fb\u542f\u52a8";
        D2D1_RECT_F hintRect{
            fx + 16.0f, fy + fh - 34.0f, fx + fw - 16.0f, fy + fh - 10.0f
        };
        renderTarget->DrawText(hint.c_str(), static_cast<UINT32>(hint.size()),
                               s_bodyFormat, &hintRect, titleTextBrush);
    }
    if (gameFence) return;

    RECT contentRect = fence.rect;
    contentRect.top += FENCE_TITLE_BAR_HEIGHT + (fence.searchActive ? 36 : 0);
    const int viewportHeight = (std::max)(0L, contentRect.bottom - contentRect.top);
    const int contentHeight = GridLayoutService::GetContentHeight(
        contentRect, fence.items, fence.viewMode);
    const int maxOffset = (std::max)(0, contentHeight - viewportHeight);
    if (maxOffset > 0 && borderBrush) {
        const float trackHeight = (std::max)(1.0f, viewportBottom - viewportTop - 8.0f);
        const float thumbHeight = (std::max)(24.0f,
            trackHeight * (float)viewportHeight / (float)contentHeight);
        const float travel = (std::max)(0.0f, trackHeight - thumbHeight);
        const float thumbY = viewportTop + 4.0f +
            travel * (float)fence.scrollOffset / (float)maxOffset;
        D2D1_ROUNDED_RECT thumb = D2D1::RoundedRect(
            D2D1::RectF(fx + fw - 5.0f, thumbY,
                        fx + fw - 2.0f, thumbY + thumbHeight),
            1.5f, 1.5f);
        renderTarget->FillRoundedRectangle(&thumb, borderBrush);
    }
}

void FenceRenderer::DrawItem(const DesktopItem& item, ID2D1Bitmap* iconBitmap, POINT windowOrigin, float opacity) {
    if (!renderTarget) return;
    if (item.filtered) return; // 不绘制被过滤的项

    float ix = (float)(item.hitRect.left - windowOrigin.x);
    float iy = (float)(item.hitRect.top - windowOrigin.y);
    float iw = (float)(item.hitRect.right - item.hitRect.left);
    float ih = (float)(item.hitRect.bottom - item.hitRect.top);

    if (item.hovered && itemHoverBrush) {
        D2D1_ROUNDED_RECT hoverBg = D2D1::RoundedRect(
            D2D1_RECT_F{ ix, iy, ix + iw, iy + ih }, 7.0f, 7.0f);
        renderTarget->FillRoundedRectangle(&hoverBg, itemHoverBrush);
    }

    if (item.selected && itemSelectedBrush) {
        D2D1_ROUNDED_RECT selectedBg = D2D1::RoundedRect(
            D2D1_RECT_F{ ix, iy, ix + iw, iy + ih }, 7.0f, 7.0f);
        renderTarget->FillRoundedRectangle(&selectedBg, itemSelectedBrush);
        renderTarget->DrawRoundedRectangle(&selectedBg, activeBorderBrush, 1.0f);
    }

    if (iconBitmap) {
        float iconX = (float)(item.iconRect.left - windowOrigin.x);
        float iconY = (float)(item.iconRect.top - windowOrigin.y);
        float iconW = (float)(item.iconRect.right - item.iconRect.left);
        float iconH = (float)(item.iconRect.bottom - item.iconRect.top);

        D2D1_RECT_F dstRect = { iconX, iconY, iconX + iconW, iconY + iconH };
        renderTarget->DrawBitmap(iconBitmap, dstRect, opacity,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    if (s_itemTextFormat && s_listTextFormat && itemTextBrush) {
        float tx = (float)(item.textRect.left - windowOrigin.x);
        float ty = (float)(item.textRect.top - windowOrigin.y);
        float tw = (float)(item.textRect.right - item.textRect.left);
        float th = (float)(item.textRect.bottom - item.textRect.top);

        D2D1_RECT_F textRect = { tx, ty, tx + tw, ty + th };
        const bool listItem = (item.iconRect.right - item.iconRect.left) <=
                              GridLayoutService::LIST_ICON_SIZE;
        IDWriteTextFormat* textFormat = listItem ? s_listTextFormat : s_itemTextFormat;
        ID2D1SolidColorBrush* textBrush = itemTextBrush;

        if (opacity < 1.0f && textBrush) {
            D2D1_COLOR_F c = textBrush->GetColor();
            c.a *= opacity;
            ID2D1SolidColorBrush* dimBrush = nullptr;
            renderTarget->CreateSolidColorBrush(c, &dimBrush);
            if (dimBrush) {
                renderTarget->DrawText(item.name.c_str(), (UINT32)item.name.size(),
                                       textFormat, &textRect, dimBrush,
                                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
                dimBrush->Release();
                return;
            }
        }

        renderTarget->DrawText(item.name.c_str(), (UINT32)item.name.size(),
                               textFormat, &textRect, textBrush,
                               D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void FenceRenderer::DrawEmptyText(const Fence& fence, POINT windowOrigin) {
    if (!renderTarget || !s_bodyFormat || !titleTextBrush) return;

    float fx = (float)(fence.rect.left - windowOrigin.x);
    float fy = (float)(fence.rect.top - windowOrigin.y);
    float fw = (float)(fence.rect.right - fence.rect.left);
    float fh = (float)(fence.rect.bottom - fence.rect.top);
    float titleH = (float)FENCE_TITLE_BAR_HEIGHT;

    const float searchOffset = fence.searchActive ? 36.0f : 0.0f;
    D2D1_RECT_F contentRect = { fx + 8, fy + titleH + searchOffset + 8, fx + fw - 8, fy + fh - 8 };
    std::wstring emptyText = fence.kind == FenceKind::Game
        ? L"\u62d6\u5165\u4e00\u4e2a .lnk / .url / .exe \u6e38\u620f\u542f\u52a8\u9879"
        : L"\u7a7a\u683c\u5b50";
    renderTarget->DrawText(emptyText.c_str(), (UINT32)emptyText.size(),
                           s_bodyFormat, &contentRect, titleTextBrush);
}

void FenceRenderer::DrawNoMatchText(const Fence& fence, POINT windowOrigin) {
    if (!renderTarget || !s_bodyFormat || !titleTextBrush) return;

    float fx = (float)(fence.rect.left - windowOrigin.x);
    float fy = (float)(fence.rect.top - windowOrigin.y);
    float fw = (float)(fence.rect.right - fence.rect.left);
    float fh = (float)(fence.rect.bottom - fence.rect.top);
    float titleH = (float)FENCE_TITLE_BAR_HEIGHT;

    const float searchOffset = fence.searchActive ? 36.0f : 0.0f;
    D2D1_RECT_F contentRect = { fx + 8, fy + titleH + searchOffset + 8, fx + fw - 8, fy + fh - 8 };
    std::wstring text = L"未找到匹配文件";
    renderTarget->DrawText(text.c_str(), (UINT32)text.size(),
                           s_bodyFormat, &contentRect, titleTextBrush);
}

void FenceRenderer::DrawGhostIcon(const GhostIcon& ghost, ID2D1Bitmap* icon, POINT windowOrigin) {
    if (!renderTarget || !ghost.visible) return;

    float gx = (float)(ghost.position.x - windowOrigin.x);
    float gy = (float)(ghost.position.y - windowOrigin.y);
    float gs = (float)ghost.iconSize;

    if (ghostBgBrush) {
        D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(
            D2D1_RECT_F{ gx - 6, gy - 6, gx + gs + 6, gy + gs + 24 }, 8.0f, 8.0f);
        renderTarget->FillRoundedRectangle(&bgRect, ghostBgBrush);
    }

    if (icon) {
        D2D1_RECT_F iconRect = { gx, gy, gx + gs, gy + gs };
        renderTarget->DrawBitmap(icon, iconRect, ghost.opacity,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    if (s_ghostTextFormat && itemTextBrush) {
        D2D1_RECT_F textRect = { gx - 20, gy + gs + 2, gx + gs + 20, gy + gs + 18 };
        renderTarget->DrawText(ghost.name.c_str(), (UINT32)ghost.name.size(),
                               s_ghostTextFormat, &textRect, itemTextBrush);
    }
}

void FenceRenderer::DrawDropTargetHighlight(const Fence& fence, POINT windowOrigin) {
    if (!renderTarget || !dropTargetBrush) return;

    float fx = (float)(fence.rect.left - windowOrigin.x);
    float fy = (float)(fence.rect.top - windowOrigin.y);
    float fw = (float)(fence.rect.right - fence.rect.left);
    float fh = (float)(fence.rect.bottom - fence.rect.top);

    D2D1_ROUNDED_RECT outerRect = D2D1::RoundedRect(
        D2D1_RECT_F{ fx, fy, fx + fw, fy + fh }, 10.0f, 10.0f);
    renderTarget->DrawRoundedRectangle(&outerRect, dropTargetBrush, 3.0f);
}

// 绘制重命名输入框（覆盖在标题栏上）
void FenceRenderer::DrawRenameInput(const TextInputState& state, POINT windowOrigin) {
    if (!renderTarget || !state.active || !s_inputFormat || !s_dwriteFactory) return;

    float rx = (float)(state.rect.left - windowOrigin.x);
    float ry = (float)(state.rect.top - windowOrigin.y);
    float rw = (float)(state.rect.right - state.rect.left);
    float rh = (float)(state.rect.bottom - state.rect.top);

    // 背景
    if (inputBgBrush) {
        D2D1_ROUNDED_RECT bg = D2D1::RoundedRect(
            D2D1::RectF(rx, ry, rx + rw, ry + rh), 3.0f, 3.0f);
        renderTarget->FillRoundedRectangle(&bg, inputBgBrush);
    }
    // 边框
    renderTarget->DrawRoundedRectangle(
        &D2D1::RoundedRect(D2D1::RectF(rx, ry, rx + rw, ry + rh), 3.0f, 3.0f),
        activeBorderBrush, 1.5f);

    // 文字
    const float textLeft = rx + 6.0f;
    const float textRight = rx + rw - 6.0f;
    const float visibleWidth = (std::max)(1.0f, textRight - textLeft);
    IDWriteTextLayout* layout = nullptr;
    HRESULT layoutResult = s_dwriteFactory->CreateTextLayout(
        state.text.c_str(), static_cast<UINT32>(state.text.size()), s_inputFormat,
        4096.0f, (std::max)(1.0f, rh), &layout);

    FLOAT caretOffset = 0.0f;
    FLOAT caretTop = 0.0f;
    float horizontalScroll = 0.0f;
    if (SUCCEEDED(layoutResult) && layout && inputCursorBrush) {
        const UINT32 caretPos = static_cast<UINT32>((std::min)(
            (std::max)(state.caretPos, 0), static_cast<int>(state.text.size())));
        DWRITE_HIT_TEST_METRICS hitMetrics{};
        if (!state.text.empty()) {
            const bool atEnd = caretPos == state.text.size();
            const UINT32 hitPosition = atEnd ? caretPos - 1 : caretPos;
            layout->HitTestTextPosition(
                hitPosition, atEnd, &caretOffset, &caretTop, &hitMetrics);
        }
        horizontalScroll = (std::max)(0.0f, caretOffset - visibleWidth + 2.0f);
        renderTarget->PushAxisAlignedClip(
            D2D1::RectF(textLeft, ry + 1.0f, textRight, ry + rh - 1.0f),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        renderTarget->DrawTextLayout(
            D2D1::Point2F(textLeft - horizontalScroll, ry), layout,
            inputCursorBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // 光标
    if (state.caretBlinkOn && inputCursorBrush) {
        // 估算光标 x 位置（粗略：按字符数 * 平均宽度）
        float charW = 7.5f;
        float caretX = textLeft + caretOffset - horizontalScroll;
        renderTarget->DrawLine(
            D2D1::Point2F(caretX, ry + 4),
            D2D1::Point2F(caretX, ry + rh - 4),
            inputCursorBrush, 1.5f);
    }
    if (SUCCEEDED(layoutResult) && layout && inputCursorBrush) {
        renderTarget->PopAxisAlignedClip();
    }
    SafeRelease(layout);
}

// 绘制搜索框（格子内容区顶部）
void FenceRenderer::DrawSearchInput(const Fence& fence, const TextInputState& state, POINT windowOrigin) {
    if (!renderTarget || !state.active || !s_inputFormat) return;

    float rx = (float)(state.rect.left - windowOrigin.x);
    float ry = (float)(state.rect.top - windowOrigin.y);
    float rw = (float)(state.rect.right - state.rect.left);
    float rh = (float)(state.rect.bottom - state.rect.top);

    // 背景
    if (inputBgBrush) {
        D2D1_ROUNDED_RECT bg = D2D1::RoundedRect(
            D2D1::RectF(rx, ry, rx + rw, ry + rh), 4.0f, 4.0f);
        renderTarget->FillRoundedRectangle(&bg, inputBgBrush);
    }
    renderTarget->DrawRoundedRectangle(
        &D2D1::RoundedRect(D2D1::RectF(rx, ry, rx + rw, ry + rh), 4.0f, 4.0f),
        fence.searchActive ? activeBorderBrush : borderBrush, 1.2f);

    // 放大镜图标（左侧）
    DrawSearchIcon(rx + 12, ry + rh / 2.0f, false);

    // 文字
    D2D1_RECT_F textRect = { rx + 24, ry, rx + rw - 24, ry + rh };
    std::wstring display = state.text;
    if (display.empty()) {
        display = L"搜索...";
        ID2D1SolidColorBrush* hintBrush = nullptr;
        renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.55f, 0.8f), &hintBrush);
        if (hintBrush) {
            renderTarget->DrawText(display.c_str(), (UINT32)display.size(),
                                   s_inputFormat, &textRect, hintBrush);
            hintBrush->Release();
        }
    } else {
        if (inputCursorBrush) {
            renderTarget->DrawText(display.c_str(), (UINT32)display.size(),
                                   s_inputFormat, &textRect, inputCursorBrush);
        }
        // 光标
        if (state.caretBlinkOn && inputCursorBrush) {
            float charW = 7.5f;
            float caretX = rx + 24 + (float)state.caretPos * charW;
            renderTarget->DrawLine(
                D2D1::Point2F(caretX, ry + 3),
                D2D1::Point2F(caretX, ry + rh - 3),
                inputCursorBrush, 1.5f);
        }
    }

    // 清空按钮（右侧 X）
    if (!state.text.empty() && titleTextBrush) {
        float bx = rx + rw - 14;
        float by = ry + rh / 2.0f;
        renderTarget->DrawLine(D2D1::Point2F(bx - 4, by - 4), D2D1::Point2F(bx + 4, by + 4), titleTextBrush, 1.2f);
        renderTarget->DrawLine(D2D1::Point2F(bx - 4, by + 4), D2D1::Point2F(bx + 4, by - 4), titleTextBrush, 1.2f);
    }
}

bool FenceRenderer::EndFrame() {
    if (!renderTarget) return false;
    if (frameClipActive) {
        renderTarget->PopAxisAlignedClip();
        frameClipActive = false;
    }
    HRESULT hr = renderTarget->EndDraw();
    if (FAILED(hr)) {
        if (iconCache) iconCache->ClearBitmaps();
        DestroyRenderTarget();
        return false;
    }
    return true;
}
