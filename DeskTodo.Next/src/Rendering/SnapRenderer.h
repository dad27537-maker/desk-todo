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
};
