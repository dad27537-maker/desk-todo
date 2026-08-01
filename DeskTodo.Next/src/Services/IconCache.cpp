#include "IconCache.h"
#include <wincodec.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>

#pragma comment(lib, "windowscodecs.lib")

IconCache::IconCache() {}
IconCache::~IconCache() { ClearAll(); }

void IconCache::Initialize() {
    defaultIcon = LoadIcon(nullptr, IDI_APPLICATION);
}

void IconCache::Shutdown() {
    ClearAll();
}

std::wstring IconCache::GetCacheKey(const DesktopItem& item) const {
    if (!item.fullPath.empty()) return item.fullPath;
    if (!item.ext.empty()) return item.ext;
    return item.name;
}

HICON IconCache::GetIcon(const DesktopItem& item) {
    std::wstring key = GetCacheKey(item);
    if (key.empty()) {
        if (!defaultIcon) defaultIcon = LoadIcon(nullptr, IDI_APPLICATION);
        return defaultIcon;
    }

    auto it = iconMap.find(key);
    if (it != iconMap.end()) return it->second;

    HICON hIcon = item.fullPath.empty() ? nullptr : ExtractIconForPath(item.fullPath);
    if (!hIcon && !item.ext.empty()) {
        hIcon = ExtractIconForExt(item.ext);
    }
    if (!hIcon) {
        hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }
    iconMap[key] = hIcon;
    return hIcon;
}

ID2D1Bitmap* IconCache::GetOrCreateBitmap(const DesktopItem& item, ID2D1RenderTarget* rt) {
    if (!rt) return nullptr;

    std::wstring key = GetCacheKey(item);
    auto it = bitmapCache.find(key);
    if (it != bitmapCache.end()) {
        ID2D1Bitmap* bmp = it->second;
        if (bmp) {
            D2D1_SIZE_U sz = bmp->GetPixelSize();
            if (sz.width > 0) return bmp;
            bmp->Release();
            bitmapCache.erase(key);
        }
    }

    HICON hIcon = GetIcon(item);
    ID2D1Bitmap* bmp = CreateBitmapFromHICON(hIcon, rt);
    bitmapCache[key] = bmp;
    return bmp;
}

void IconCache::PreloadIconsForFence(const Fence& fence) {
    for (const auto& item : fence.items) {
        GetIcon(item);
    }
}

void IconCache::ClearBitmaps() {
    for (auto& pair : bitmapCache) {
        if (pair.second) pair.second->Release();
    }
    bitmapCache.clear();
}

void IconCache::ClearAll() {
    ClearBitmaps();
    for (auto& pair : iconMap) {
        if (pair.second) DestroyIcon(pair.second);
    }
    iconMap.clear();
    if (defaultIcon) { DestroyIcon(defaultIcon); defaultIcon = nullptr; }
}

HICON IconCache::ExtractIconForExt(const std::wstring& ext) {
    std::wstring tempPath = L"dummy" + ext;
    SHFILEINFOW sfi = {};
    DWORD_PTR result = SHGetFileInfoW(tempPath.c_str(), FILE_ATTRIBUTE_NORMAL,
                                      &sfi, sizeof(sfi),
                                      SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES);
    if (result) {
        IImageList* imageList = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                                     reinterpret_cast<void**>(&imageList))) && imageList) {
            HICON icon = nullptr;
            imageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon);
            imageList->Release();
            if (icon) return icon;
        }
    }

    result = SHGetFileInfoW(tempPath.c_str(), FILE_ATTRIBUTE_NORMAL,
                            &sfi, sizeof(sfi),
                            SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
    if (result && sfi.hIcon) return sfi.hIcon;
    return nullptr;
}

HICON IconCache::ExtractIconForPath(const std::wstring& path) {
    SHFILEINFOW sfi = {};
    DWORD_PTR result = SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                                      SHGFI_SYSICONINDEX);
    if (result) {
        IImageList* imageList = nullptr;
        if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                                     reinterpret_cast<void**>(&imageList))) && imageList) {
            HICON icon = nullptr;
            imageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon);
            imageList->Release();
            if (icon) return icon;
        }
    }

    result = SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                            SHGFI_ICON | SHGFI_LARGEICON);
    if (result && sfi.hIcon) return sfi.hIcon;
    return nullptr;
}

ID2D1Bitmap* IconCache::CreateBitmapFromHICON(HICON hIcon, ID2D1RenderTarget* rt) {
    if (!hIcon || !rt) return nullptr;

    IWICImagingFactory* wicFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) return nullptr;

    IWICBitmap* wicBitmap = nullptr;
    hr = wicFactory->CreateBitmapFromHICON(hIcon, &wicBitmap);
    if (FAILED(hr)) { wicFactory->Release(); return nullptr; }

    IWICFormatConverter* converter = nullptr;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        wicBitmap->Release();
        wicFactory->Release();
        return nullptr;
    }

    hr = converter->Initialize(
        wicBitmap,
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->Release();
        wicBitmap->Release();
        wicFactory->Release();
        return nullptr;
    }

    ID2D1Bitmap* d2dBitmap = nullptr;
    hr = rt->CreateBitmapFromWicBitmap(converter, nullptr, &d2dBitmap);

    converter->Release();
    wicBitmap->Release();
    wicFactory->Release();

    return SUCCEEDED(hr) ? d2dBitmap : nullptr;
}
