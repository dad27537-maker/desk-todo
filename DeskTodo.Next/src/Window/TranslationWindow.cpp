#include "TranslationWindow.h"
#include "Services/TranslationService.h"
#include "TranslationWindowTheme.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <richedit.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>

namespace {
constexpr wchar_t ClassName[] = L"DeskTodoTranslation";
constexpr UINT ReplaceImage = WM_APP + 81;
constexpr int SourceId = 6201, TargetId = 6202, TranslateId = 6203, CancelId = 6204;
constexpr int OriginalId = 6205, ResultId = 6206, CopyId = 6207, ProviderId = 6208;
constexpr int SwapId = 6209, OcrId = 6210, CopyOriginalId = 6211, CopyBothId = 6212;
constexpr int SourceLabel = 6213, TargetLabel = 6214, StatusId = 6215, NoticeId = 6216;
constexpr int ProviderLabel = 6217, OriginalLabel = 6219, ResultLabel = 6220, PreviewId = 6221;
constexpr UINT_PTR PollTimer = 1;
constexpr UINT_PTR AnimationTimer = 2;
namespace Theme = TranslationTheme;
HWND activeWindow = nullptr;
using Bitmap = std::shared_ptr<std::remove_pointer_t<HBITMAP>>;
struct Work {
    std::atomic<bool> cancelled{false}, complete{false};
    bool success = false;
    bool fromBitmap = false;
    TranslationService::Result result;
    std::wstring error;
};
struct Context {
    Bitmap image;
    TranslationService::Settings settings;
    std::shared_ptr<Work> work;
    std::wstring detectedSource;
    Theme::Fonts fonts;
    Theme::Layout layout;
    Theme::State state = Theme::State::Idle;
    HBRUSH background = CreateSolidBrush(Theme::Background);
    HBRUSH surface = CreateSolidBrush(Theme::Surface);
    HWND tooltip = nullptr;
    ULONG_PTR graphicsToken = 0;
    int hovered = 0;
    std::array<float, 32> hover{};
    BOOL animate = TRUE;
    bool settingText = false;
    bool originalEdited = false;
    int dpi = 96;
    Context() {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&graphicsToken, &input, nullptr);
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    }
    ~Context() {
        if (work) work->cancelled = true;
        DeleteObject(background); DeleteObject(surface);
        if (graphicsToken) Gdiplus::GdiplusShutdown(graphicsToken);
    }
};
std::wstring Text(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(length);
    return text;
}
void Status(HWND window, const std::wstring& text, Theme::State state = Theme::State::Idle) {
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (context) context->state = state;
    SetDlgItemTextW(window, StatusId, text.c_str());
    InvalidateRect(GetDlgItem(window, StatusId), nullptr, FALSE);
    InvalidateRect(GetDlgItem(window, ResultId), nullptr, FALSE);
    if (context && context->animate && state == Theme::State::Busy) SetTimer(window, AnimationTimer, 16, nullptr);
}
int Scale(const Context& context, int value) { return MulDiv(value, context.dpi, 96); }
RECT Pixels(const Context& context, RECT rect) { return Theme::Pixels(rect, context.dpi); }
void ApplyFonts(HWND window, Context& context) {
    context.fonts.Reset(context.dpi);
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const int id = GetDlgCtrlID(child);
        HFONT font = context.fonts.strong;
        if (id == OriginalId || id == ResultId) font = context.fonts.body;
        if (id == SourceLabel || id == TargetLabel || id == ProviderLabel) font = context.fonts.label;
        if (id == NoticeId || id == StatusId) font = context.fonts.caption;
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    for (int id : {SourceId, TargetId, ProviderId}) {
        SendDlgItemMessageW(window, id, CB_SETITEMHEIGHT, -1, Scale(context, 36));
        SendDlgItemMessageW(window, id, CB_SETITEMHEIGHT, 0, Scale(context, 36));
    }
    for (int id : {OriginalId, ResultId}) {
        SendDlgItemMessageW(window, id, EM_SETBKGNDCOLOR, 0, Theme::Surface);
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format); format.dwMask = CFM_COLOR;
        format.crTextColor = Theme::Text;
        SendDlgItemMessageW(window, id, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
        PARAFORMAT2 paragraph{};
        paragraph.cbSize = sizeof(paragraph); paragraph.dwMask = PFM_LINESPACING;
        paragraph.bLineSpacingRule = 4; paragraph.dyLineSpacing = 390;
        SendDlgItemMessageW(window, id, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&paragraph));
    }
}
void PaintSurface(HWND window, HDC dc, Context& context) {
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, context.background);
    auto label = [&](const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
        Theme::Label(dc, text, Pixels(context, rect), font, color, flags);
    };
    auto panel = [&](RECT rect, bool focus = false) {
        RECT shadow = rect; shadow.top += 2; shadow.bottom += 2;
        Theme::Round(dc, Pixels(context, shadow), Scale(context, 14), RGB(228, 240, 233));
        Theme::Round(dc, Pixels(context, rect), Scale(context, 14), Theme::Surface, focus ? Theme::Teal : Theme::Border);
    };
    const auto& l = context.layout;
    Theme::Round(dc, Pixels(context, {28, 24, 72, 68}), Scale(context, 13), Theme::Teal);
    Theme::DrawIcon(dc, Theme::Icon::Translate, Pixels(context, {38, 34, 62, 58}), Theme::Surface);
    label(L"截图翻译", {88, 20, 400, 54}, context.fonts.heading, Theme::Text);
    label(L"识别、编辑与翻译，在一个窗口完成", {89, 57, 560, 76}, context.fonts.label, Theme::Muted);
    if (l.width >= 850) {
        Theme::Round(dc, Pixels(context, {l.width - 160, 33, l.width - 28, 63}), Scale(context, 15), Theme::TealSoft);
        Theme::DrawIcon(dc, Theme::Icon::Shield, Pixels(context, {l.width - 147, 40, l.width - 131, 56}), Theme::Teal);
        label(L"截图不上传", {l.width - 122, 34, l.width - 37, 62}, context.fonts.caption, Theme::Teal);
    }
    panel(l.toolbar);
    panel(l.original, GetFocus() == GetDlgItem(window, OriginalId));
    panel(l.translated, GetFocus() == GetDlgItem(window, ResultId));
    panel(l.preview);
    Theme::DrawIcon(dc, Theme::Icon::Document,
        Pixels(context, {l.original.left + 20, l.original.top + 21, l.original.left + 38, l.original.top + 39}), Theme::Teal);
    Theme::DrawIcon(dc, Theme::Icon::Translate,
        Pixels(context, {l.translated.left + 20, l.translated.top + 21, l.translated.left + 38, l.translated.top + 39}), Theme::Teal);
    const int baseline = l.original.bottom - 32;
    label(L"可直接修改识别结果", {l.original.left + 22, baseline, l.original.right - 120, baseline + 20}, context.fonts.caption, Theme::Muted);
    label(std::to_wstring(GetWindowTextLengthW(GetDlgItem(window, OriginalId))) + L" 字符",
        {l.original.right - 120, baseline, l.original.right - 22, baseline + 20}, context.fonts.caption, Theme::Muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    const bool memory = SendDlgItemMessageW(window, ProviderId, CB_GETCURSEL, 0, 0) == 1;
    label(memory ? L"由 MyMemory 提供翻译" : L"由 Google 翻译提供译文",
        {l.translated.left + 22, baseline, l.translated.right - 100, baseline + 20}, context.fonts.caption, Theme::Muted);
    label(std::to_wstring(GetWindowTextLengthW(GetDlgItem(window, ResultId))) + L" 字符",
        {l.translated.right - 110, baseline, l.translated.right - 22, baseline + 20}, context.fonts.caption, Theme::Muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    BITMAP details{};
    GetObjectW(context.image.get(), sizeof(details), &details);
    label(L"所选截图", {l.preview.left + 132, l.preview.top + 12, l.preview.right - 330, l.preview.top + 34}, context.fonts.strong, Theme::Text);
    const auto dimensions = std::to_wstring(details.bmWidth) + L" × " + std::to_wstring(details.bmHeight) + L" px";
    label(dimensions, {l.preview.right - 440, l.preview.top + 13, l.preview.right - 322, l.preview.top + 33}, context.fonts.caption, Theme::Muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    label(L"Ctrl + Enter 翻译", {l.width - 200, l.height - 38, l.width - 30, l.height - 12}, context.fonts.caption, Theme::Muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}
void PaintButton(HWND window, const DRAWITEMSTRUCT& item, Context& context) {
    const int id = static_cast<int>(item.CtlID);
    const bool enabled = IsWindowEnabled(item.hwndItem) != FALSE;
    const bool down = (item.itemState & ODS_SELECTED) != 0;
    const auto slot = static_cast<size_t>(id - 6200);
    const float hover = enabled && slot < context.hover.size() ? context.hover[slot] : 0;
    COLORREF background = Theme::Surface, text = Theme::Teal, border = CLR_INVALID;
    if (id == TranslateId) {
        background = Theme::Mix(Theme::Orange, RGB(153, 48, 9), down ? 1.f : hover);
        text = Theme::Surface;
    } else if (id == CopyBothId) {
        background = Theme::Mix(Theme::TealSoft, RGB(208, 236, 225), down ? 1.f : hover);
        border = RGB(201, 226, 215);
    } else if (id == CancelId || id == SwapId) {
        background = Theme::Mix(Theme::Surface, Theme::TealSoft, down ? 1.f : hover);
        border = Theme::Border; text = Theme::Muted;
    } else background = Theme::Mix(Theme::Surface, Theme::TealSoft, down ? 1.f : hover);
    if (!enabled && id != TranslateId) text = RGB(156, 174, 166);
    FillRect(item.hDC, &item.rcItem, context.surface);
    RECT rect = item.rcItem;
    InflateRect(&rect, -1, -1);
    Theme::Round(item.hDC, rect, Scale(context, 9), background, border);
    if (GetFocus() == item.hwndItem && !(item.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = rect; InflateRect(&focus, -Scale(context, 3), -Scale(context, 3));
        Theme::Round(item.hDC, focus, Scale(context, 6), background, id == TranslateId ? RGB(255, 220, 191) : Theme::Teal);
    }
    Theme::Icon icon = Theme::Icon::Copy;
    if (id == TranslateId) icon = Theme::Icon::Translate;
    if (id == OcrId) icon = Theme::Icon::Refresh;
    if (id == SwapId) icon = Theme::Icon::Swap;
    if (id == CancelId) icon = Theme::Icon::Close;
    const auto caption = id == SwapId ? std::wstring{} : Text(window, id);
    auto old = SelectObject(item.hDC, context.fonts.strong);
    SIZE size{};
    GetTextExtentPoint32W(item.hDC, caption.c_str(), static_cast<int>(caption.size()), &size);
    SelectObject(item.hDC, old);
    const int iconSize = id == CancelId ? 0 : Scale(context, 17);
    const int gap = caption.empty() || !iconSize ? 0 : Scale(context, 7);
    const int contentWidth = iconSize + gap + size.cx;
    const int x = (rect.left + rect.right - contentWidth) / 2;
    const int y = (rect.top + rect.bottom - iconSize) / 2;
    if (iconSize) Theme::DrawIcon(item.hDC, icon, {x, y, x + iconSize, y + iconSize}, text);
    if (!caption.empty()) Theme::Label(item.hDC, caption,
        {x + iconSize + gap, rect.top, rect.right - 1, rect.bottom}, context.fonts.strong, text,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}
void PaintCombo(HWND combo, HDC dc, Context& context) {
    RECT rect{};
    GetClientRect(combo, &rect);
    FillRect(dc, &rect, context.surface);
    const bool enabled = IsWindowEnabled(combo) != FALSE;
    const bool active = GetFocus() == combo || SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0);
    const bool hovering = context.hovered == GetDlgCtrlID(combo);
    const COLORREF fill = enabled ? Theme::Surface : RGB(246, 249, 247);
    Theme::Round(dc, rect, Scale(context, 9), fill, active ? Theme::Teal : (hovering ? RGB(157, 193, 178) : Theme::Border));
    wchar_t value[256]{};
    const auto selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected >= 0) SendMessageW(combo, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(value));
    RECT text = rect; text.left += Scale(context, 13); text.right -= Scale(context, 30);
    Theme::Label(dc, value, text, context.fonts.strong, enabled ? Theme::Text : Theme::Muted);
    const int x = rect.right - Scale(context, 24), y = (rect.bottom - Scale(context, 16)) / 2;
    Theme::DrawIcon(dc, Theme::Icon::Chevron, {x, y, x + Scale(context, 16), y + Scale(context, 16)}, Theme::Muted);
}
void PaintPlaceholder(HWND edit, HDC dc, Context& context) {
    if (GetWindowTextLengthW(edit)) return;
    RECT rect{};
    GetClientRect(edit, &rect);
    const bool result = GetDlgCtrlID(edit) == ResultId;
    const bool busy = context.state == Theme::State::Busy;
    std::wstring title = result ? L"译文将在这里呈现" : L"识别文字，也可以直接粘贴原文";
    std::wstring subtitle = result ? L"选择目标语言，开始一次轻松的阅读。" : L"你可以修正识别结果，再重新翻译。";
    if (busy) { title = result ? L"正在为你整理译文…" : L"正在识别截图中的文字…"; subtitle = L"请稍候，随时可以取消。"; }
    if (result && context.state == Theme::State::Error) { title = L"暂时没能完成翻译"; subtitle = L"原文已保留，可切换引擎后重试。"; }
    RECT line{Scale(context, 3), Scale(context, 9), rect.right - Scale(context, 5), Scale(context, 37)};
    Theme::Label(dc, title, line, context.fonts.body, Theme::Muted);
    line.top += Scale(context, 38); line.bottom += Scale(context, 45);
    Theme::Label(dc, subtitle, line, context.fonts.label, Theme::Muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
}
LRESULT CALLBACK ControlProc(HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& context = *reinterpret_cast<Context*>(data);
    const int id = GetDlgCtrlID(control);
    const bool combo = id == SourceId || id == TargetId || id == ProviderId;
    const bool edit = id == OriginalId || id == ResultId;
    HWND parent = GetParent(control);
    switch (message) {
    case WM_MOUSEMOVE:
        if (!edit && context.hovered != id) {
            context.hovered = id;
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
            TrackMouseEvent(&tracking);
            if (context.animate) SetTimer(parent, AnimationTimer, 16, nullptr);
            else { if (id - 6200 < static_cast<int>(context.hover.size())) context.hover[id - 6200] = 1; InvalidateRect(control, nullptr, FALSE); }
            if (combo) InvalidateRect(control, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (context.hovered == id) context.hovered = 0;
        if (context.animate) SetTimer(parent, AnimationTimer, 16, nullptr);
        else { context.hover[id - 6200] = 0; InvalidateRect(control, nullptr, FALSE); }
        break;
    case WM_SETCURSOR:
        if (!edit && IsWindowEnabled(control)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(control, nullptr, FALSE);
        if (edit) InvalidateRect(parent, nullptr, FALSE);
        break;
    case WM_KEYDOWN:
        if (wp == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) { PostMessageW(parent, WM_COMMAND, TranslateId, 0); return 0; }
        break;
    case WM_PAINT:
        if (combo) {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(control, &paint);
            PaintCombo(control, dc, context); EndPaint(control, &paint); return 0;
        }
        if (edit) {
            const auto result = DefSubclassProc(control, message, wp, lp);
            HDC dc = GetDC(control); PaintPlaceholder(control, dc, context); ReleaseDC(control, dc); return result;
        }
        break;
    case WM_PRINTCLIENT:
        if (combo) { PaintCombo(control, reinterpret_cast<HDC>(wp), context); return 0; }
        if (edit) { const auto result = DefSubclassProc(control, message, wp, lp); PaintPlaceholder(control, reinterpret_cast<HDC>(wp), context); return result; }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(control, ControlProc, 1);
        break;
    }
    return DefSubclassProc(control, message, wp, lp);
}
void SetOriginal(HWND window, Context& context, const std::wstring& text, bool edited = false) {
    context.settingText = true;
    SetDlgItemTextW(window, OriginalId, text.c_str());
    context.settingText = false;
    context.originalEdited = edited;
    EnableWindow(GetDlgItem(window, CopyOriginalId), !text.empty());
}
void Notice(HWND window) {
    const bool myMemory = SendDlgItemMessageW(window, ProviderId, CB_GETCURSEL, 0, 0) == 1;
    SetDlgItemTextW(window, NoticeId, myMemory ?
        L"仅发送原文至 MyMemory · 免费额度有限" :
        L"仅发送原文至 Google 翻译" );
    InvalidateRect(window, nullptr, FALSE);
}
void Layout(HWND window, Context& context) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = MulDiv(client.right, 96, context.dpi), height = MulDiv(client.bottom, 96, context.dpi);
    context.layout = Theme::Layout::Build(width, height);
    const auto& l = context.layout;
    auto move = [&](int id, RECT rect) {
        rect = Pixels(context, rect);
        MoveWindow(GetDlgItem(window, id), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
    };
    const int field = (width - 96 - 224 - 60) / 3;
    int x = 48;
    move(SourceLabel, {x, 116, x + field, 134});
    move(SourceId, {x, 140, x + field, 420});
    x += field + 12;
    move(SwapId, {x, 140, x + 44, 182});
    x += 56;
    move(TargetLabel, {x, 116, x + field, 134});
    move(TargetId, {x, 140, x + field, 420});
    x += field + 12;
    move(ProviderLabel, {x, 116, x + field, 134});
    move(ProviderId, {x, 140, x + field, 420});
    x += field + 12;
    move(TranslateId, {x, 140, x + 116, 182});
    x += 128;
    move(CancelId, {x, 140, x + 64, 182});
    const auto& a = l.original;
    const auto& b = l.translated;
    move(OriginalLabel, {a.left + 48, a.top + 17, a.right - 118, a.top + 44});
    move(ResultLabel, {b.left + 48, b.top + 17, b.right - 118, b.top + 44});
    move(CopyOriginalId, {a.right - 108, a.top + 14, a.right - 16, a.top + 47});
    move(CopyId, {b.right - 108, b.top + 14, b.right - 16, b.top + 47});
    move(OriginalId, {a.left + 22, a.top + 64, a.right - 22, a.bottom - 48});
    move(ResultId, {b.left + 22, b.top + 64, b.right - 22, b.bottom - 48});
    move(PreviewId, {l.preview.left + 14, l.preview.top + 12, l.preview.left + 118, l.preview.top + 68});
    move(NoticeId, {l.preview.left + 132, l.preview.top + 39, l.preview.right - 322, l.preview.top + 62});
    move(OcrId, {l.preview.right - 302, l.preview.top + 20, l.preview.right - 178, l.preview.top + 60});
    move(CopyBothId, {l.preview.right - 166, l.preview.top + 20, l.preview.right - 16, l.preview.top + 60});
    move(StatusId, {30, height - 42, width - 220, height - 8});
    for (int id : {OriginalId, ResultId}) {
        RECT text{};
        GetClientRect(GetDlgItem(window, id), &text);
        text.left += Scale(context, 2); text.right -= Scale(context, 2);
        SendDlgItemMessageW(window, id, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&text));
    }
    InvalidateRect(window, nullptr, FALSE);
}
void Busy(HWND window, bool busy) {
    for (int id : {SourceId, TargetId, TranslateId, ProviderId, SwapId, OcrId})
        EnableWindow(GetDlgItem(window, id), !busy);
    SendDlgItemMessageW(window, OriginalId, EM_SETREADONLY, busy, 0);
    const auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    SetDlgItemTextW(window, TranslateId, busy ? L"翻译中…" :
        (context && context->state == Theme::State::Error ? L"重试翻译" : L"重新翻译"));
    EnableWindow(GetDlgItem(window, CancelId), busy);
}
std::wstring SelectedLanguage(HWND window, int id) {
    const auto index = SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0);
    if (id == SourceId && index == 0) return L"auto";
    const auto language = index - (id == SourceId ? 1 : 0);
    const auto& languages = TranslationService::Languages();
    return language >= 0 && static_cast<size_t>(language) < languages.size() ? languages[language].code : L"";
}
bool SelectLanguage(HWND window, int id, const std::wstring& code) {
    int index = id == SourceId ? 1 : 0;
    if (id == SourceId && code == L"auto") { SendDlgItemMessageW(window, id, CB_SETCURSEL, 0, 0); return true; }
    for (const auto& language : TranslationService::Languages()) {
        if (code == language.code) { SendDlgItemMessageW(window, id, CB_SETCURSEL, index, 0); return true; }
        ++index;
    }
    return false;
}
void ReadSettings(HWND window, Context& context) {
    context.settings.source = SelectedLanguage(window, SourceId);
    context.settings.target = SelectedLanguage(window, TargetId);
    context.settings.provider = SendDlgItemMessageW(window, ProviderId, CB_GETCURSEL, 0, 0) == 1
        ? TranslationService::Provider::MyMemory : TranslationService::Provider::Google;
}
void Start(HWND window, Context& context, bool forceOcr = false) {
    if (context.work) return;
    ReadSettings(window, context);
    const std::wstring original = forceOcr ? L"" : Text(window, OriginalId);
    const bool needsOcr = original.find_first_not_of(L" \t\r\n") == std::wstring::npos;
    Bitmap image;
    if (needsOcr) {
        image = Bitmap(static_cast<HBITMAP>(CopyImage(context.image.get(), IMAGE_BITMAP, 0, 0,
            LR_CREATEDIBSECTION)), [](HBITMAP bitmap) { DeleteObject(bitmap); });
        if (!image) { Status(window, L"无法读取截图，请重新截图或粘贴原文。", Theme::State::Error); return; }
    }
    auto work = std::make_shared<Work>();
    work->fromBitmap = needsOcr;
    try {
        // Workers own their input/result and never access HWNDs. Cancelled jobs cannot write back.
        std::thread([work, image = std::move(image), original, settings = context.settings, needsOcr] {
            try {
                work->success = needsOcr
                    ? TranslationService::TranslateBitmap(image.get(), settings, work->result, work->error, work->cancelled)
                    : TranslationService::TranslateText(original, settings, work->result, work->error, work->cancelled);
            } catch (...) { work->error = L"翻译失败，请稍后重试。"; }
            work->complete = true;
        }).detach();
    } catch (...) { Status(window, L"无法启动翻译，请稍后重试。", Theme::State::Error); return; }
    std::wstring ignored;
    TranslationService::SaveSettings(context.settings, ignored);
    context.work = std::move(work);
    Busy(window, true);
    SetDlgItemTextW(window, OriginalLabel, L"原文");
    SetDlgItemTextW(window, ResultId, L"");
    EnableWindow(GetDlgItem(window, CopyId), FALSE);
    EnableWindow(GetDlgItem(window, CopyBothId), FALSE);
    Status(window, needsOcr ? L"正在识别并翻译截图…" : L"正在翻译原文…", Theme::State::Busy);
    SetTimer(window, PollTimer, 60, nullptr);
}
void Cancel(HWND window, Context& context) {
    if (context.work) context.work->cancelled = true;
    context.work.reset();
    KillTimer(window, PollTimer);
    Busy(window, false);
    Status(window, L"已取消翻译。");
}
void Copy(HWND window, int id) {
    auto text = id == CopyOriginalId ? Text(window, OriginalId) : Text(window, ResultId);
    if (id == CopyBothId) text = L"原文\r\n" + Text(window, OriginalId) + L"\r\n\r\n译文\r\n" + text;
    if (text.empty()) return;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (!memory) { Status(window, L"无法分配剪贴板内存。"); return; }
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return; }
    memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!OpenClipboard(window)) { GlobalFree(memory); Status(window, L"剪贴板正被占用，请重试。"); return; }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!copied) GlobalFree(memory);
    Status(window, copied ? (id == CopyOriginalId ? L"原文已复制。" : (id == CopyBothId ? L"对照文本已复制。" : L"译文已复制。")) : L"无法复制，请重试。",
        copied ? Theme::State::Success : Theme::State::Error);
}
void ClearTranslation(HWND window) {
    SetDlgItemTextW(window, ResultId, L"");
    EnableWindow(GetDlgItem(window, CopyId), FALSE);
    EnableWindow(GetDlgItem(window, CopyBothId), FALSE);
}
void Swap(HWND window, Context& context) {
    ReadSettings(window, context);
    const auto source = context.settings.source == L"auto" ? context.detectedSource : context.settings.source;
    if (source.empty() || !SelectLanguage(window, TargetId, source)) {
        Status(window, L"请先完成一次翻译，或手动选择源语言后再交换。"); return;
    }
    SelectLanguage(window, SourceId, context.settings.target);
    auto original = Text(window, OriginalId), translated = Text(window, ResultId);
    context.detectedSource.clear();
    if (!translated.empty()) {
        SetOriginal(window, context, translated, true);
        SetDlgItemTextW(window, ResultId, original.c_str());
    } else {
        SetOriginal(window, context, L"");
        ClearTranslation(window);
    }
    Status(window, L"已交换语言，可编辑原文后继续翻译。");
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<Context*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    switch (message) {
    case WM_CREATE: {
        context->dpi = static_cast<int>(GetDpiForWindow(window));
        auto control = [&](int id, const wchar_t* type, const wchar_t* text, DWORD style) {
            const bool button = wcscmp(type, L"BUTTON") == 0;
            HWND child = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE |
                (button ? WS_TABSTOP | BS_OWNERDRAW : style), 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            if (button || wcscmp(type, L"COMBOBOX") == 0 || wcscmp(type, MSFTEDIT_CLASS) == 0)
                SetWindowSubclass(child, ControlProc, 1, reinterpret_cast<DWORD_PTR>(context));
            if (wcscmp(type, L"COMBOBOX") == 0) SetWindowTheme(child, L"", L"");
            return child;
        };
        control(SourceLabel, L"STATIC", L"原文语言", SS_CENTERIMAGE);
        control(TargetLabel, L"STATIC", L"目标语言", SS_CENTERIMAGE);
        const DWORD comboStyle = WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL;
        HWND source = control(SourceId, L"COMBOBOX", L"", comboStyle);
        HWND target = control(TargetId, L"COMBOBOX", L"", comboStyle);
        SendMessageW(source, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"自动识别"));
        for (const auto& language : TranslationService::Languages()) {
            SendMessageW(source, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(language.name));
            SendMessageW(target, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(language.name));
        }
        SelectLanguage(window, SourceId, context->settings.source);
        SelectLanguage(window, TargetId, context->settings.target);
        control(SwapId, L"BUTTON", L"交换语言", 0);
        control(TranslateId, L"BUTTON", L"开始翻译", 0);
        control(CancelId, L"BUTTON", L"取消", 0);
        control(ProviderLabel, L"STATIC", L"翻译服务", SS_CENTERIMAGE);
        HWND provider = control(ProviderId, L"COMBOBOX", L"", comboStyle);
        SendMessageW(provider, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Google 翻译"));
        SendMessageW(provider, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MyMemory"));
        SendMessageW(provider, CB_SETCURSEL, context->settings.provider == TranslationService::Provider::MyMemory ? 1 : 0, 0);
        control(NoticeId, L"STATIC", L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        Notice(window);
        control(PreviewId, L"STATIC", L"", SS_OWNERDRAW);
        control(OriginalLabel, L"STATIC", L"原文", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        control(ResultLabel, L"STATIC", L"译文", SS_CENTERIMAGE);
        control(OcrId, L"BUTTON", L"重新识别", 0);
        for (int id : {OriginalId, ResultId}) {
            control(id, MSFTEDIT_CLASS, L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN | WS_VSCROLL | (id == ResultId ? ES_READONLY : 0));
            SendDlgItemMessageW(window, id, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
            SendDlgItemMessageW(window, id, EM_SETEVENTMASK, 0, ENM_CHANGE);
            SendDlgItemMessageW(window, id, EM_EXLIMITTEXT, 0, id == OriginalId ? 20000 : 200000);
        }
        control(CopyId, L"BUTTON", L"复制译文", 0);
        control(CopyOriginalId, L"BUTTON", L"复制原文", 0);
        control(CopyBothId, L"BUTTON", L"复制双语对照", 0);
        control(StatusId, L"STATIC", L"准备识别截图…", SS_OWNERDRAW);
        ApplyFonts(window, *context);
        Busy(window, false);
        SetDlgItemTextW(window, TranslateId, L"开始翻译");
        for (int id : {CopyId, CopyOriginalId, CopyBothId}) EnableWindow(GetDlgItem(window, id), FALSE);
        context->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(context->tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(*context, 360));
        auto tip = [&](int id, const wchar_t* text) {
            TOOLINFOW tool{sizeof(tool)};
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = window; tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(window, id));
            tool.lpszText = const_cast<wchar_t*>(text);
            SendMessageW(context->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        };
        tip(SwapId, L"交换原文与译文的语言");
        tip(OcrId, L"重新读取截图文字，替换当前原文");
        tip(ProviderId, L"无需账号或密钥。仅发送识别出的文字；MyMemory 的免费额度有限。");
        tip(CopyBothId, L"复制原文和译文，方便整理与分享");
        Layout(window, *context);
        return 0;
    }
    case WM_SIZE:
        if (context) Layout(window, *context);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {context ? Scale(*context, 840) : 840, context ? Scale(*context, 640) : 640};
        return 0;
    case WM_DPICHANGED: {
        context->dpi = HIWORD(wp);
        ApplyFonts(window, *context);
        auto* bounds = reinterpret_cast<RECT*>(lp);
        SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
            bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(window, *context);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        RECT rect{}; GetClientRect(window, &rect);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, (std::max)(1L, rect.right), (std::max)(1L, rect.bottom));
        if (buffer && bitmap) {
            auto old = SelectObject(buffer, bitmap);
            PaintSurface(window, buffer, *context);
            BitBlt(dc, 0, 0, rect.right, rect.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, old);
        } else PaintSurface(window, dc, *context);
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        EndPaint(window, &paint); return 0;
    }
    case WM_PRINTCLIENT:
        PaintSurface(window, reinterpret_cast<HDC>(wp), *context); return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), Theme::Muted);
        SetBkColor(reinterpret_cast<HDC>(wp), Theme::Surface);
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<LRESULT>(context->surface);
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = Scale(*context, 36); return TRUE;
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (item.CtlType == ODT_BUTTON) { PaintButton(window, item, *context); return TRUE; }
        if (item.CtlType == ODT_COMBOBOX) {
            FillRect(item.hDC, &item.rcItem, context->surface);
            const bool selected = (item.itemState & ODS_SELECTED) != 0;
            RECT row = item.rcItem; InflateRect(&row, -Scale(*context, 3), -Scale(*context, 2));
            if (selected) Theme::Round(item.hDC, row, Scale(*context, 6), Theme::TealSoft);
            if (item.itemID != static_cast<UINT>(-1)) {
                wchar_t value[256]{};
                SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(value));
                row.left += Scale(*context, 10); row.right -= Scale(*context, 10);
                Theme::Label(item.hDC, value, row, context->fonts.strong, selected ? Theme::Teal : Theme::Text);
            }
            return TRUE;
        }
        if (item.CtlID == StatusId) {
            FillRect(item.hDC, &item.rcItem, context->background);
            const auto state = context->state;
            const COLORREF color = state == Theme::State::Error ? Theme::Error : Theme::Teal;
            const RECT icon = Pixels(*context, {1, 8, 17, 24});
            if (state == Theme::State::Busy) {
                Gdiplus::Graphics graphics(item.hDC);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::Pen pen(Theme::Color(color), static_cast<float>(Scale(*context, 2)));
                pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
                const float phase = context->animate ? static_cast<float>(GetTickCount64() % 1200) * .3f : -90.f;
                graphics.DrawArc(&pen, static_cast<float>(icon.left), static_cast<float>(icon.top),
                    static_cast<float>(icon.right - icon.left), static_cast<float>(icon.bottom - icon.top), phase, 250.f);
            } else Theme::DrawIcon(item.hDC, state == Theme::State::Error ? Theme::Icon::Close :
                (state == Theme::State::Success ? Theme::Icon::Check : Theme::Icon::Shield), icon, color);
            RECT text = item.rcItem; text.left += Scale(*context, 26);
            Theme::Label(item.hDC, Text(window, StatusId), text, context->fonts.caption,
                state == Theme::State::Error ? Theme::Error : Theme::Muted);
            return TRUE;
        }
        if (item.CtlID != PreviewId) break;
        FillRect(item.hDC, &item.rcItem, context->surface);
        Theme::Round(item.hDC, item.rcItem, Scale(*context, 8), RGB(242, 247, 244), Theme::Border);
        BITMAP details{};
        if (context->image && GetObjectW(context->image.get(), sizeof(details), &details) && details.bmWidth > 0 && details.bmHeight > 0) {
            const int padding = Scale(*context, 5);
            double scale = (std::min)(1.0, (std::min)(static_cast<double>(item.rcItem.right - padding * 2) / details.bmWidth,
                                      static_cast<double>(item.rcItem.bottom - padding * 2) / details.bmHeight));
            const int width = (std::max)(1, static_cast<int>(details.bmWidth * scale));
            const int height = (std::max)(1, static_cast<int>(details.bmHeight * scale));
            HDC source = CreateCompatibleDC(item.hDC);
            auto old = SelectObject(source, context->image.get());
            SetStretchBltMode(item.hDC, HALFTONE);
            SetBrushOrgEx(item.hDC, 0, 0, nullptr);
            StretchBlt(item.hDC, (item.rcItem.right - width) / 2, (item.rcItem.bottom - height) / 2,
                width, height, source, 0, 0, details.bmWidth, details.bmHeight, SRCCOPY);
            SelectObject(source, old); DeleteDC(source);
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case TranslateId: Start(window, *context); return 0;
        case OcrId: Start(window, *context, true); return 0;
        case CancelId: Cancel(window, *context); return 0;
        case IDCANCEL:
            if (context->work) Cancel(window, *context);
            else DestroyWindow(window);
            return 0;
        case CopyId: case CopyOriginalId: case CopyBothId: Copy(window, LOWORD(wp)); return 0;
        case SwapId: Swap(window, *context); return 0;
        case SourceId:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                if (!context->originalEdited) SetOriginal(window, *context, L"");
                context->detectedSource.clear();
                ClearTranslation(window);
                Status(window, context->originalEdited ? L"源语言已切换，将使用你编辑的原文翻译。" :
                    L"源语言已切换，点击“翻译”重新识别截图，或粘贴原文。");
            }
            return 0;
        case TargetId: case ProviderId:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                ClearTranslation(window);
                Notice(window);
                Status(window, L"点击“翻译”使用当前原文重新翻译。");
            }
            return 0;
        case OriginalId:
            if (HIWORD(wp) == EN_CHANGE && !context->settingText) {
                context->originalEdited = true;
                context->detectedSource.clear();
                ClearTranslation(window);
                EnableWindow(GetDlgItem(window, CopyOriginalId), !Text(window, OriginalId).empty());
                Status(window, L"原文已修改，点击“翻译”更新结果。");
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == AnimationTimer) {
            bool changing = false;
            for (size_t index = 0; index < context->hover.size(); ++index) {
                const float target = context->hovered == static_cast<int>(6200 + index) ? 1.f : 0.f;
                if (std::abs(context->hover[index] - target) < .01f) continue;
                context->hover[index] += (target > context->hover[index] ? 1.f : -1.f) * .12f;
                context->hover[index] = (std::max)(0.f, (std::min)(1.f, context->hover[index]));
                InvalidateRect(GetDlgItem(window, static_cast<int>(6200 + index)), nullptr, FALSE);
                changing = std::abs(context->hover[index] - target) > .01f || changing;
            }
            const bool busy = context->state == Theme::State::Busy;
            if (busy) {
                InvalidateRect(GetDlgItem(window, StatusId), nullptr, FALSE);
                if (!GetWindowTextLengthW(GetDlgItem(window, ResultId))) InvalidateRect(GetDlgItem(window, ResultId), nullptr, FALSE);
            }
            if (!changing && !busy) KillTimer(window, AnimationTimer);
            return 0;
        }
        if (wp == PollTimer && context->work && context->work->complete.load()) {
            auto work = std::move(context->work);
            KillTimer(window, PollTimer);
            if (!work->cancelled && !work->result.original.empty())
                SetOriginal(window, *context, work->result.original, !work->fromBitmap && context->originalEdited);
            if (work->success && !work->cancelled) {
                context->detectedSource = work->result.source;
                SetDlgItemTextW(window, ResultId, work->result.translated.c_str());
                EnableWindow(GetDlgItem(window, CopyId), TRUE);
                EnableWindow(GetDlgItem(window, CopyBothId), TRUE);
                Status(window, L"翻译完成。可编辑原文、切换语言或复制对照。", Theme::State::Success);
                if (context->settings.source == L"auto") for (const auto& language : TranslationService::Languages()) {
                    if (work->result.source == language.code) {
                        SetDlgItemTextW(window, OriginalLabel, (L"原文 · " + std::wstring(language.name)).c_str());
                        break;
                    }
                }
            } else {
                SetDlgItemTextW(window, TranslateId, L"重试翻译");
                Status(window, work->error, Theme::State::Error);
            }
            Busy(window, false);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case ReplaceImage:
        Cancel(window, *context);
        context->image = Bitmap(reinterpret_cast<HBITMAP>(lp), [](HBITMAP bitmap) { DeleteObject(bitmap); });
        SetOriginal(window, *context, L"");
        context->detectedSource.clear();
        ClearTranslation(window);
        Layout(window, *context);
        Start(window, *context);
        return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        if (context->work) context->work->cancelled = true;
        KillTimer(window, PollTimer);
        KillTimer(window, AnimationTimer);
        if (context->tooltip && IsWindow(context->tooltip)) DestroyWindow(context->tooltip);
        if (activeWindow == window) activeWindow = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
void TranslationWindow::Show(HWND owner, HBITMAP screenshot) {
    if (!screenshot) return;
    if (activeWindow && IsWindow(activeWindow)) {
        SendMessageW(activeWindow, ReplaceImage, 0, reinterpret_cast<LPARAM>(screenshot));
        ShowWindow(activeWindow, SW_RESTORE);
        SetForegroundWindow(activeWindow);
        return;
    }
    static HMODULE richEditModule = LoadLibraryW(L"Msftedit.dll");
    if (!richEditModule) { DeleteObject(screenshot); return; }
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    Context context;
    context.image = Bitmap(screenshot, [](HBITMAP bitmap) { DeleteObject(bitmap); });
    context.settings = TranslationService::LoadSettings();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = ClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto& area = monitor.rcWork;
    context.dpi = static_cast<int>(GetDpiForSystem());
    const int width = (std::min)(Scale(context, 1100), static_cast<int>(area.right - area.left));
    const int height = (std::min)(Scale(context, 800), static_cast<int>(area.bottom - area.top));
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, ClassName, L"截图翻译 · DeskTodo",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, area.left + (area.right - area.left - width) / 2,
        area.top + (area.bottom - area.top - height) / 2, width, height,
        owner, nullptr, GetModuleHandleW(nullptr), &context);
    if (!window) return;
    // Recalculate with this monitor's DPI, which can differ from the primary screen.
    const int actualWidth = (std::min)(Scale(context, 1100), static_cast<int>(area.right - area.left));
    const int actualHeight = (std::min)(Scale(context, 800), static_cast<int>(area.bottom - area.top));
    SetWindowPos(window, nullptr, area.left + (area.right - area.left - actualWidth) / 2,
        area.top + (area.bottom - area.top - actualHeight) / 2, actualWidth, actualHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    const COLORREF caption = Theme::Background, captionText = Theme::Text;
    const DWORD corners = 2;
    DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 36, &captionText, sizeof(captionText));
    DwmSetWindowAttribute(window, 33, &corners, sizeof(corners));
    if (HICON icon = LoadIconW(GetModuleHandleW(nullptr), L"IDI_DESKTODO_ICON")) {
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    }
    activeWindow = window;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    Start(window, context);
    MSG message{};
    int status = 0;
    while (IsWindow(window) && (status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(window)) DestroyWindow(window);
    if (status == 0 && message.message == WM_QUIT) PostQuitMessage(static_cast<int>(message.wParam));
    if (owner && IsWindow(owner)) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
}
