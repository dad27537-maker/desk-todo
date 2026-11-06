#include <Windows.h>
#include <iostream>
#include "Rendering/FenceRenderer.h"
#include "Services/IconCache.h"

// Count bitmap requests without involving Shell extensions or filesystem I/O.
int bitmapRequests = 0;
IconCache::IconCache(IconLoader iconLoader) : loader(iconLoader) {}
IconCache::~IconCache() = default;
void IconCache::ClearBitmaps() {}
ID2D1Bitmap* IconCache::GetOrCreateBitmap(const DesktopItem&, ID2D1RenderTarget*) {
    ++bitmapRequests;
    return nullptr;
}

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!FenceRenderer::EnsureFactories()) return 2;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 400;
    info.bmiHeader.biHeight = -300;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    int failures = 0;
    {
        IconCache cache;
        FenceRenderer renderer;
        renderer.SetIconCache(&cache);
        if (!renderer.CreateRenderTarget(dc, 400, 300)) return 3;
        Fence fence;
        fence.rect = {0, 0, 400, 200};
        DesktopItem first;
        first.name = L"visible";
        first.iconRect = {20, 60, 44, 84};
        first.textRect = {60, 55, 380, 90};
        first.hitRect = {10, 55, 390, 90};
        fence.items.push_back(first);
        DesktopItem offscreen = first;
        offscreen.name = L"outside viewport";
        OffsetRect(&offscreen.iconRect, 0, 300);
        OffsetRect(&offscreen.textRect, 0, 300);
        OffsetRect(&offscreen.hitRect, 0, 300);
        fence.items.push_back(offscreen);
        const auto draw = [&](const char* name, const RECT* dirty,
                              POINT origin, int expected, bool dragSource = false) {
            bitmapRequests = 0;
            renderer.BeginFrame(dirty);
            renderer.DrawFence(fence, false, false, false, dragSource, 0, origin);
            const bool drawn = renderer.EndFrame();
            const bool passed = drawn && bitmapRequests == expected;
            failures += !passed;
            std::cout << (passed ? "PASS " : "FAIL ") << name
                      << " bitmap requests=" << bitmapRequests << '\n';
        };
        draw("full frame excludes scrolled-out row", nullptr, {}, 1);
        RECT textDirty{100, 55, 200, 90};
        draw("list text-only dirty region does not load icon", &textDirty, {}, 0);
        RECT iconDirty{20, 60, 40, 80};
        draw("visible icon dirty region loads icon", &iconDirty, {}, 1);
        RECT unrelatedDirty{0, 220, 400, 290};
        draw("unrelated fence excluded from dirty frame", &unrelatedDirty, {}, 0);
        draw("drag source does not load hidden source bitmap", nullptr, {}, 0, true);
        OffsetRect(&fence.rect, -1920, -100);
        for (auto& item : fence.items) {
            OffsetRect(&item.iconRect, -1920, -100);
            OffsetRect(&item.textRect, -1920, -100);
            OffsetRect(&item.hitRect, -1920, -100);
        }
        draw("negative screen origin", &iconDirty, {-1920, -100}, 1);
        fence.items[0].filtered = true;
        draw("filtered item excluded", nullptr, {-1920, -100}, 0);
        fence.items[0].filtered = false;
        fence.collapsed = true;
        draw("collapsed fence excludes all items", nullptr, {-1920, -100}, 0);
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    FenceRenderer::ReleaseFactories();
    CoUninitialize();
    return failures == 0 ? 0 : 1;
}
