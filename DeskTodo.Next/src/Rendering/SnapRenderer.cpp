#include "SnapRenderer.h"

#define SafeRelease(p) if (p) { p->Release(); p = nullptr; }

SnapRenderer::SnapRenderer() {}
SnapRenderer::~SnapRenderer() { DestroyResources(); }

void SnapRenderer::CreateResources(ID2D1DCRenderTarget* renderTarget) {
    if (!renderTarget) return;

    if (snapLineBrush && resourceTarget == renderTarget) return;

    // A brush cannot be used with a different Direct2D render target. This
    // happens when a monitor reconnect recreates the DIB/DC target.
    DestroyResources();

    HRESULT hr = renderTarget->CreateSolidColorBrush(
        D2D1::ColorF(0.31f, 0.67f, 1.0f, 0.8f),
        &snapLineBrush);
    if (SUCCEEDED(hr)) {
        resourceTarget = renderTarget;
        resourceTarget->AddRef();
    }
}

void SnapRenderer::DestroyResources() {
    SafeRelease(snapLineBrush);
    SafeRelease(resourceTarget);
}

void SnapRenderer::DrawSnapLines(
    ID2D1DCRenderTarget* renderTarget,
    const std::vector<SnapLine>& lines,
    POINT windowOrigin)
{
    if (!renderTarget || lines.empty()) return;

    if (!snapLineBrush || resourceTarget != renderTarget) {
        CreateResources(renderTarget);
    }

    if (!snapLineBrush || resourceTarget != renderTarget) return;

    for (const auto& line : lines) {
        if (!line.visible) continue;

        float x1 = (float)(line.start.x - windowOrigin.x);
        float y1 = (float)(line.start.y - windowOrigin.y);
        float x2 = (float)(line.end.x - windowOrigin.x);
        float y2 = (float)(line.end.y - windowOrigin.y);

        renderTarget->DrawLine(
            D2D1::Point2F(x1, y1),
            D2D1::Point2F(x2, y2),
            snapLineBrush, 1.0f);
    }
}
