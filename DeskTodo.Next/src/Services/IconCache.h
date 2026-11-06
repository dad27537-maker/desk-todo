#pragma once
#include <Windows.h>
#include <d2d1.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include "Models/Fence.h"
#include "Models/DesktopItem.h"

class IconCache {
public:
    // A custom loader runs on the worker and transfers ownership of its HICON.
    using IconLoader = HICON (*)(const std::wstring&, const std::wstring&);
    explicit IconCache(IconLoader loader = nullptr);
    ~IconCache();

    void Initialize();
    void Shutdown();
    HICON GetIcon(const DesktopItem& item);
    ID2D1Bitmap* GetOrCreateBitmap(const DesktopItem& item, ID2D1RenderTarget* rt);
    void PreloadIconsForFence(const Fence& fence);
    void ClearBitmaps();
    void ClearAll();
    // UI thread only. Adopt ready HICONs and invalidate their placeholder
    // bitmaps. Returns true when the caller should schedule a repaint.
    bool DrainCompletedIcons();

private:
    std::unordered_map<std::wstring, HICON> iconMap;
    std::unordered_map<std::wstring, ID2D1Bitmap*> bitmapCache;
    HICON defaultIcon = nullptr;
    struct IconRequest {
        std::wstring key;
        std::wstring path;
        std::wstring ext;
        uint64_t generation = 0;
    };
    struct IconResult {
        std::wstring key;
        HICON icon = nullptr;
        uint64_t generation = 0;
    };
    IconLoader loader = nullptr;
    std::thread worker;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::deque<IconRequest> requests;
    std::deque<IconResult> completed;
    std::unordered_set<std::wstring> pendingKeys;
    uint64_t generation = 0;
    bool stopping = false;
    bool initialized = false;

    void RunWorker();

    std::wstring GetCacheKey(const DesktopItem& item) const;
    HICON ExtractIconForExt(const std::wstring& ext);
    HICON ExtractIconForPath(const std::wstring& path);
    ID2D1Bitmap* CreateBitmapFromHICON(HICON hIcon, ID2D1RenderTarget* rt);
};
