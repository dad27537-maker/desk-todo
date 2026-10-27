#pragma once
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <vector>
#include "Models/Fence.h"
#include "Models/DesktopItem.h"
#include "Models/DragContext.h"

class IconCache;

// 重命名/搜索输入框状态（由窗口维护，传递给渲染器绘制）
struct TextInputState {
    bool active = false;
    std::wstring text;
    int caretPos = 0;      // 字符位置
    bool caretBlinkOn = true;
    RECT rect{};           // 屏幕坐标
};

class FenceRenderer {
public:
    FenceRenderer();
    ~FenceRenderer();

    bool CreateRenderTarget(HDC hdc, int w, int h);
    void DestroyRenderTarget();

    void BeginFrame(const RECT* dirtyRect = nullptr);
    void DrawBlackHoleBackground(const uint8_t* pixels, uint32_t w, uint32_t h);
    void DrawFence(const Fence& fence, bool isActive, bool isHovered, bool isMoving,
                   bool isDragSource, int draggedItemIndex, POINT windowOrigin);
    void DrawItem(const DesktopItem& item, ID2D1Bitmap* iconBitmap, POINT windowOrigin, float opacity = 1.0f);
    void DrawEmptyText(const Fence& fence, POINT windowOrigin);
    void DrawGhostIcon(const GhostIcon& ghost, ID2D1Bitmap* icon, POINT windowOrigin);
    void DrawDropTargetHighlight(const Fence& fence, POINT windowOrigin);
    // Returns false when Direct2D lost the target (for example during a
    // display mode switch). The caller must discard the partial frame and
    // perform a complete redraw before uploading the bitmap again.
    bool EndFrame();

    // 绘制重命名输入框
    void DrawRenameInput(const TextInputState& state, POINT windowOrigin);
    // 绘制搜索框（格子内）
    void DrawSearchInput(const Fence& fence, const TextInputState& state, POINT windowOrigin);
    // 绘制"未找到匹配文件"提示
    void DrawNoMatchText(const Fence& fence, POINT windowOrigin);

    void SetIconCache(IconCache* cache) { iconCache = cache; }
    ID2D1DCRenderTarget* GetRenderTarget() const { return renderTarget; }

    static bool EnsureFactories();
    static void ReleaseFactories();

private:
    ID2D1DCRenderTarget* renderTarget = nullptr;
    ID2D1Bitmap* blackHoleBitmap = nullptr;
    ID2D1SolidColorBrush* titleTextBrush = nullptr;
    ID2D1SolidColorBrush* borderBrush = nullptr;
    ID2D1SolidColorBrush* activeBorderBrush = nullptr;
    ID2D1SolidColorBrush* movingBorderBrush = nullptr;
    ID2D1SolidColorBrush* hoverBorderBrush = nullptr;
    ID2D1SolidColorBrush* itemHoverBrush = nullptr;
    ID2D1SolidColorBrush* itemSelectedBrush = nullptr;
    ID2D1SolidColorBrush* itemTextBrush = nullptr;
    ID2D1SolidColorBrush* dropTargetBrush = nullptr;
    ID2D1SolidColorBrush* ghostBgBrush = nullptr;
    ID2D1SolidColorBrush* inputBgBrush = nullptr;
    ID2D1SolidColorBrush* inputCursorBrush = nullptr;
    IconCache* iconCache = nullptr;
    int width = 0;
    int height = 0;
    bool frameClipActive = false;
    RECT frameClipRect{}; // Backbuffer coordinates, including full-frame bounds.

    static ID2D1Factory* s_d2dFactory;
    static IDWriteFactory* s_dwriteFactory;
    static IDWriteTextFormat* s_titleFormat;
    static IDWriteTextFormat* s_bodyFormat;
    static IDWriteTextFormat* s_itemTextFormat;
    static IDWriteTextFormat* s_listTextFormat;
    static IDWriteTextFormat* s_ghostTextFormat;
    static IDWriteTextFormat* s_countFormat;
    static IDWriteTextFormat* s_inputFormat;
    static int s_refCount;

    bool IsVisibleInFrame(RECT screenRect, POINT windowOrigin,
                          int padding = 0) const;

    void DrawLockIconWithAnimation(const Fence& fence, float cx, float cy, float baseSize);
    void DrawViewToggleIcon(float cx, float cy, bool isListMode);
    void DrawSearchIcon(float cx, float cy, bool active);
    void DrawGameFenceCard(const Fence& fence, bool isActive, bool isHovered,
                           bool isMoving, bool isDragSource,
                           int draggedItemIndex, POINT windowOrigin);
    void DrawClockFenceCard(const Fence& fence, bool isActive, bool isHovered,
                            bool isMoving, POINT windowOrigin);
};
