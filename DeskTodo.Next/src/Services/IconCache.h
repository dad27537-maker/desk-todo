#pragma once
#include <Windows.h>
#include <d2d1.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "Models/Fence.h"
#include "Models/DesktopItem.h"

class IconCache {
public:
    IconCache();
    ~IconCache();

    void Initialize();
    void Shutdown();
    HICON GetIcon(const DesktopItem& item);
    ID2D1Bitmap* GetOrCreateBitmap(const DesktopItem& item, ID2D1RenderTarget* rt);
    void PreloadIconsForFence(const Fence& fence);
    void ClearBitmaps();
    void ClearAll();

private:
    std::unordered_map<std::wstring, HICON> iconMap;
    std::unordered_map<std::wstring, ID2D1Bitmap*> bitmapCache;
    HICON defaultIcon = nullptr;

    std::wstring GetCacheKey(const DesktopItem& item) const;
    HICON ExtractIconForExt(const std::wstring& ext);
    HICON ExtractIconForPath(const std::wstring& path);
    ID2D1Bitmap* CreateBitmapFromHICON(HICON hIcon, ID2D1RenderTarget* rt);
};
