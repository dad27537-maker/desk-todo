#include "OcrResultWindow.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

constexpr wchar_t WindowClass[] = L"DeskTodoOcrResult";
constexpr int EditId = 5101;
constexpr int CopyId = 5102;
constexpr int CloseId = 5103;

struct Context {
    std::wstring text;
    std::wstring title;
    HWND edit = nullptr;
    HFONT font = nullptr;
};

bool CopyEditText(HWND window) {
    HWND edit = GetDlgItem(window, EditId);
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(length);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE,
        (text.size() + 1) * sizeof(wchar_t));
    if (!memory) return false;
    void* target = GlobalLock(memory);
    memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!OpenClipboard(window)) { GlobalFree(memory); return false; }
    EmptyClipboard();
    const bool copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!copied) GlobalFree(memory);
    return copied;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* context = reinterpret_cast<Context*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<Context*>(
            reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(context));
    }
    switch (message) {
    case WM_CREATE: {
        context->font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        context->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            context->text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            16, 16, 608, 300, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(EditId)), nullptr, nullptr);
        HWND copy = CreateWindowW(L"BUTTON", L"复制结果",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            410, 330, 102, 36, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(CopyId)), nullptr, nullptr);
        HWND close = CreateWindowW(L"BUTTON", L"关闭",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            522, 330, 102, 36, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(CloseId)), nullptr, nullptr);
        SendMessageW(context->edit, WM_SETFONT,
                     reinterpret_cast<WPARAM>(context->font), TRUE);
        SendMessageW(copy, WM_SETFONT,
                     reinterpret_cast<WPARAM>(context->font), TRUE);
        SendMessageW(close, WM_SETFONT,
                     reinterpret_cast<WPARAM>(context->font), TRUE);
        SetFocus(context->edit);
        SendMessageW(context->edit, EM_SETSEL, 0, 0);
        return 0;
    }
    case WM_SIZE: {
        const int width = LOWORD(lp);
        const int height = HIWORD(lp);
        MoveWindow(context->edit, 16, 16, (std::max)(200, width - 32),
                   (std::max)(100, height - 76), TRUE);
        MoveWindow(GetDlgItem(window, CopyId), width - 230, height - 50,
                   102, 34, TRUE);
        MoveWindow(GetDlgItem(window, CloseId), width - 118, height - 50,
                   102, 34, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = { 520, 300 };
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == CopyId) {
            if (CopyEditText(window))
                SetWindowTextW(window, (context->title + L" - 已复制").c_str());
            return 0;
        }
        if (LOWORD(wp) == CloseId) { DestroyWindow(window); return 0; }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (context && context->font) {
            DeleteObject(context->font);
            context->font = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

}

void OcrResultWindow::Show(HWND owner, const std::wstring& text,
                           const std::wstring& title) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass{ sizeof(windowClass) };
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = WindowClass;
        registered = RegisterClassExW(&windowClass) != 0 ||
                     GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return;
    Context context{ text, title };
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int width = 660;
    const int height = 430;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        WindowClass, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
        WS_THICKFRAME, workArea.left + (workArea.right - workArea.left - width) / 2,
        workArea.top + (workArea.bottom - workArea.top - height) / 2,
        width, height, owner, nullptr, GetModuleHandleW(nullptr), &context);
    if (!window) return;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    MSG message{};
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
}
