#include "TooltipService.h"

TooltipService::TooltipService() {}
TooltipService::~TooltipService() { Destroy(); }

bool TooltipService::Create(HWND parentHwnd) {
    if (tooltipHwnd) return true;

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icex);

    tooltipHwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        TOOLTIPS_CLASS,
        nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        parentHwnd, nullptr, GetModuleHandle(nullptr), nullptr);

    if (!tooltipHwnd) return false;

    toolInfo = {};
    toolInfo.cbSize = TTTOOLINFOW_V2_SIZE;
    toolInfo.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    toolInfo.hwnd = parentHwnd;
    toolInfo.uId = (UINT_PTR)parentHwnd;
    toolInfo.lpszText = const_cast<LPWSTR>(L"");

    SendMessageW(tooltipHwnd, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);
    SendMessageW(tooltipHwnd, TTM_SETMAXTIPWIDTH, 0, 300);
    SendMessageW(tooltipHwnd, TTM_SETDELAYTIME, TTDT_AUTOPOP, 8000);

    return true;
}

void TooltipService::Destroy() {
    Hide();
    if (tooltipHwnd) {
        SendMessageW(tooltipHwnd, TTM_DELTOOL, 0, (LPARAM)&toolInfo);
        DestroyWindow(tooltipHwnd);
        tooltipHwnd = nullptr;
    }
}

void TooltipService::Show(HWND parentHwnd, const std::wstring& text, int screenX, int screenY) {
    if (!tooltipHwnd) return;

    currentText = text;
    toolInfo.lpszText = const_cast<LPWSTR>(currentText.c_str());
    SendMessageW(tooltipHwnd, TTM_UPDATETIPTEXT, 0, (LPARAM)&toolInfo);
    SendMessageW(tooltipHwnd, TTM_TRACKPOSITION, 0, MAKELPARAM(screenX + 12, screenY + 12));
    SendMessageW(tooltipHwnd, TTM_TRACKACTIVATE, TRUE, (LPARAM)&toolInfo);

    visible = true;
}

void TooltipService::Hide() {
    if (tooltipHwnd && visible) {
        SendMessageW(tooltipHwnd, TTM_TRACKACTIVATE, FALSE, (LPARAM)&toolInfo);
        visible = false;
    }
}

void TooltipService::SetText(const std::wstring& text) {
    currentText = text;
}
