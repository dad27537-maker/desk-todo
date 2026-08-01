#pragma once
#include <Windows.h>
#include <commctrl.h>
#include <string>

class TooltipService {
public:
    TooltipService();
    ~TooltipService();

    bool Create(HWND parentHwnd);
    void Destroy();
    void Show(HWND parentHwnd, const std::wstring& text, int screenX, int screenY);
    void Hide();
    void SetText(const std::wstring& text);
    bool IsVisible() const { return visible; }

private:
    HWND tooltipHwnd = nullptr;
    TOOLINFOW toolInfo = {};
    bool visible = false;
    std::wstring currentText;
};
