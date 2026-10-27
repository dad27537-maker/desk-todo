#include <Windows.h>
#include <d2d1.h>
#include <atomic>
#include <iostream>
#include "Services/IconCache.h"

namespace {
HANDLE started = nullptr;
HANDLE releaseLoader = nullptr;
std::atomic<int> loadCount{0};
std::atomic<HICON> producedIcon{nullptr};
int failures = 0;
int checks = 0;

void Check(bool passed, const char* message) {
    ++checks;
    if (passed) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool IsValidIcon(HICON icon) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return false;
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return true;
}

HICON ControlledLoader(const std::wstring&, const std::wstring&) {
    ++loadCount;
    HICON icon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
    producedIcon = icon;
    SetEvent(started);
    WaitForSingleObject(releaseLoader, 5000);
    return icon;
}

bool WaitForAdoption(IconCache& cache) {
    const ULONGLONG deadline = GetTickCount64() + 3000;
    while (GetTickCount64() < deadline) {
        if (cache.DrainCompletedIcons()) return true;
        Sleep(1);
    }
    return false;
}
}

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    releaseLoader = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HICON sharedDefault = LoadIconW(nullptr, IDI_APPLICATION);
    {
        IconCache cache(ControlledLoader);
        cache.Initialize();
        DesktopItem item;
        item.fullPath = L"test://blocking-icon";
        item.ext = L".folder";
        const ULONGLONG begin = GetTickCount64();
        Check(cache.GetIcon(item) == sharedDefault, "first request returns shared placeholder");
        Check(GetTickCount64() - begin < 250, "first request does not wait for loader");
        Check(WaitForSingleObject(started, 3000) == WAIT_OBJECT_0, "worker starts extraction");
        Fence fence;
        fence.items.push_back(item);
        for (int repeat = 0; repeat < 100; ++repeat) {
            cache.GetIcon(item);
            cache.PreloadIconsForFence(fence);
        }
        Check(loadCount == 1, "pending icon requests are deduplicated");

        ID2D1Factory* factory = nullptr;
        ID2D1DCRenderTarget* target = nullptr;
        HDC bitmapDc = CreateCompatibleDC(nullptr);
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = 64;
        bitmapInfo.bmiHeader.biHeight = -64;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        HBITMAP dib = CreateDIBSection(bitmapDc, &bitmapInfo, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
        HGDIOBJ oldBitmap = SelectObject(bitmapDc, dib);
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
        if (factory) {
            const auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            factory->CreateDCRenderTarget(&props, &target);
            if (target) {
                RECT bounds{0, 0, 64, 64};
                target->BindDC(bitmapDc, &bounds);
            }
        }
        ID2D1Bitmap* placeholder = cache.GetOrCreateBitmap(item, target);
        Check(placeholder != nullptr, "UI creates a placeholder D2D bitmap");
        if (placeholder) placeholder->AddRef();
        SetEvent(releaseLoader);
        Check(WaitForAdoption(cache), "UI adopts completed background icon");
        const HICON adopted = cache.GetIcon(item);
        Check(adopted != sharedDefault && IsValidIcon(adopted), "adopted icon is owned and valid");
        ID2D1Bitmap* finalBitmap = cache.GetOrCreateBitmap(item, target);
        Check(finalBitmap && finalBitmap != placeholder, "adoption invalidates placeholder bitmap");
        if (placeholder) placeholder->Release();

        ResetEvent(started);
        ResetEvent(releaseLoader);
        DesktopItem stale = item;
        stale.fullPath = L"test://stale-icon";
        cache.GetIcon(stale);
        Check(WaitForSingleObject(started, 3000) == WAIT_OBJECT_0, "second extraction is in flight");
        HICON staleIcon = producedIcon;
        cache.ClearAll();
        Check(!IsValidIcon(adopted), "ClearAll destroys adopted owned icons");
        SetEvent(releaseLoader);
        const ULONGLONG staleDeadline = GetTickCount64() + 3000;
        while (IsValidIcon(staleIcon) && GetTickCount64() < staleDeadline) Sleep(1);
        Check(!IsValidIcon(staleIcon), "in-flight stale generation is destroyed");
        Check(!cache.DrainCompletedIcons(), "stale generation is never adopted");
        Check(!cache.DrainCompletedIcons(), "empty completion queue stays unchanged");
        cache.Shutdown();
        cache.Shutdown();
        Check(IsValidIcon(sharedDefault), "shutdown preserves shared system icon");
        if (target) target->Release();
        if (factory) factory->Release();
        SelectObject(bitmapDc, oldBitmap);
        DeleteObject(dib);
        DeleteDC(bitmapDc);
    }
    CloseHandle(releaseLoader);
    CloseHandle(started);
    CoUninitialize();
    std::cout << "Icon cache smoke: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
