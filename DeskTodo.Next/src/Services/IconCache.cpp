#include "IconCache.h"
#include <wincodec.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>

#pragma comment(lib, "windowscodecs.lib")

IconCache::IconCache(IconLoader iconLoader) : loader(iconLoader) {}
IconCache::~IconCache() { Shutdown(); }

void IconCache::Initialize() {
    if (initialized) return;
    defaultIcon = LoadIcon(nullptr, IDI_APPLICATION);
    stopping = false;
    initialized = true;
    try {
        worker = std::thread(&IconCache::RunWorker, this);
    } catch (...) {
        // A thread resource failure must not send Shell work back to the UI.
        stopping = true;
    }
}

void IconCache::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
        requests.clear();
    }
    queueReady.notify_one();
    if (worker.joinable()) worker.join();
    ClearAll();
    initialized = false;
    // LoadIcon without LR_COPYFROMRESOURCE returns a shared system handle.
    defaultIcon = nullptr;
}

std::wstring IconCache::GetCacheKey(const DesktopItem& item) const {
    if (!item.fullPath.empty()) return item.fullPath;
    if (!item.ext.empty()) return item.ext;
    return item.name;
}

HICON IconCache::GetIcon(const DesktopItem& item) {
    if (!initialized) Initialize();
    std::wstring key = GetCacheKey(item);
    if (key.empty()) {
        return defaultIcon;
    }

    auto it = iconMap.find(key);
    if (it != iconMap.end()) return it->second ? it->second : defaultIcon;

    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!stopping && pendingKeys.insert(key).second) {
            requests.push_back({key, item.fullPath, item.ext, generation});
            queueReady.notify_one();
        }
    }
    return defaultIcon;
}

void IconCache::RunWorker() {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        IconRequest request;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueReady.wait(lock, [&] { return stopping || !requests.empty(); });
            if (stopping) break;
            request = std::move(requests.front());
            requests.pop_front();
        }
        HICON icon = nullptr;
        try {
            if (SUCCEEDED(comResult)) {
                if (loader) {
                    icon = loader(request.path, request.ext);
                } else {
                    if (!request.path.empty()) icon = ExtractIconForPath(request.path);
                    if (!icon && !request.ext.empty()) icon = ExtractIconForExt(request.ext);
                }
            }
        } catch (...) {
            // A failed extraction is cached as a default icon, not retried on
            // every frame. No exception may escape the background thread.
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!stopping && request.generation == generation) {
                completed.push_back({std::move(request.key), icon, request.generation});
                icon = nullptr;
            }
        }
        if (icon) DestroyIcon(icon);
    }
    if (SUCCEEDED(comResult)) CoUninitialize();
}

bool IconCache::DrainCompletedIcons() {
    std::deque<IconResult> ready;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        ready.swap(completed);
        for (const auto& result : ready) pendingKeys.erase(result.key);
    }
    bool changed = false;
    for (auto& result : ready) {
        // ClearAll and adoption both run on the UI thread. The worker only
        // publishes handles; it never touches maps, D2D resources or models.
        if (result.generation != generation) {
            if (result.icon) DestroyIcon(result.icon);
            continue;
        }
        auto previous = iconMap.find(result.key);
        if (previous != iconMap.end() && previous->second) DestroyIcon(previous->second);
        iconMap[result.key] = result.icon;
        if (result.icon) {
            auto bitmap = bitmapCache.find(result.key);
            if (bitmap != bitmapCache.end()) {
                if (bitmap->second) bitmap->second->Release();
                bitmapCache.erase(bitmap);
            }
            changed = true;
        }
    }
    return changed;
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
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        ++generation;
        requests.clear();
        pendingKeys.clear();
        for (const auto& result : completed) {
            if (result.icon) DestroyIcon(result.icon);
        }
        completed.clear();
    }
    ClearBitmaps();
    for (auto& pair : iconMap) {
        if (pair.second) DestroyIcon(pair.second);
    }
    iconMap.clear();
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
