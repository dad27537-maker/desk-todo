#pragma once
#include <Windows.h>
#include <d2d1.h>
#include <vector>
#include "Models/SnapCommon.h"

class SnapRenderer {
public:
    SnapRenderer();
    ~SnapRenderer();

    void CreateResources(ID2D1DCRenderTarget* renderTarget);
    void DestroyResources();

    void DrawSnapLines(
        ID2D1DCRenderTarget* renderTarget,
        const std::vector<SnapLine>& lines,
        POINT windowOrigin
    );

private:
    ID2D1SolidColorBrush* snapLineBrush = nullptr;
    // Direct2D resources are owned by the render target that created them.
    // Keep the target alive so a newly-created target cannot reuse the same
    // address while the old brush is still cached.
    ID2D1DCRenderTarget* resourceTarget = nullptr;
};
