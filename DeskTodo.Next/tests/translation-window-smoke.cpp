#include "Window/TranslationWindow.h"
#include "Services/TranslationService.h"
#include <ole2.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;
std::atomic<int> requests{0}, completed{0};
std::atomic<int> bitmapRequests{0}, textRequests{0};
std::atomic<bool> failed{false};
std::atomic<bool> releaseCancelledRequest{false}, releaseClosedRequest{false};
std::string failure;
int checks = 0;
std::string showcase;
const std::wstring ShowcaseOriginal = L"Great ideas travel across languages.\r\n\r\n"
    L"Capture text from a document, a website, or a conversation. Edit the original on the left and keep the translated version beside it.\r\n\r\n"
    L"Everything starts with a screenshot. Your image stays on this device; only the recognized text is sent to your selected translation service.";
const std::wstring ShowcaseTranslation = L"让好想法，跨越语言。\r\n\r\n"
    L"从文档、网页或对话中捕捉文字。在左侧编辑原文，让译文始终与它并排呈现。\r\n\r\n"
    L"一切从一张截图开始。图片始终保留在这台设备上，只有识别出的文字会发送到你选择的翻译服务。";
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
template<class Predicate> bool Wait(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    return predicate();
}
std::wstring Text(HWND window, int id) {
    wchar_t buffer[1024]{};
    GetDlgItemTextW(window, id, buffer, 1024);
    return buffer;
}
void SavePreview(HWND window) {
    wchar_t path[32768]{};
    if (!GetEnvironmentVariableW(L"DESKTODO_TRANSLATION_PREVIEW", path, 32768)) return;
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    RECT rect{};
    GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto old = SelectObject(dc, bitmap);
    BOOL rendered = PrintWindow(window, dc, 2);
    GdiFlush();
    const DWORD bytes = width * height * 4;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + bytes;
    std::ofstream output(std::filesystem::path(path), std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    if (pixels) output.write(static_cast<const char*>(pixels), bytes);
    const bool written = output.good();
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    Check(rendered && pixels && written, "render translation window for visual inspection");
}
}

const std::vector<TranslationService::Language>& TranslationService::Languages() {
    static const std::vector<Language> list{{L"zh-Hans", L"简体中文"}, {L"en", L"英语"}, {L"ja", L"日语"}};
    return list;
}
TranslationService::Settings TranslationService::LoadSettings() {
    return {};
}
bool TranslationService::SaveSettings(const Settings&, std::wstring&) { return true; }
bool TranslationService::TranslateText(const std::wstring& text, const Settings& settings, Result& result,
    std::wstring& error, const std::atomic<bool>&, const Transport&) {
    ++textRequests;
    ++requests;
    std::this_thread::sleep_for(showcase == "--preview-loading" ? 2s : 250ms);
    result = {text, settings.target == L"ja" ? L"こんにちは世界！" : L"你好，世界！", L"en", settings.target};
    if (!showcase.empty()) result.translated = ShowcaseTranslation;
    if (showcase == "--preview-error") {
        result.translated.clear();
        error = L"连接暂时中断。原文已保留，可切换翻译服务后重试。";
    }
    ++completed;
    return showcase != "--preview-error";
}
bool TranslationService::TranslateBitmap(HBITMAP bitmap, const Settings& settings, Result& result,
    std::wstring& error, const std::atomic<bool>& cancelled, const Transport&) {
    const int bitmapRequest = ++bitmapRequests;
    ++requests;
    // Keep these jobs pending until the driver has observed cancellation or
    // closure. A fixed sleep races with UI scheduling on a loaded machine.
    if (showcase.empty() && bitmapRequest == 1)
        Wait([] { return releaseCancelledRequest.load(); });
    else if (showcase.empty() && bitmapRequest == 4)
        Wait([] { return releaseClosedRequest.load(); });
    else std::this_thread::sleep_for(250ms);
    BITMAP details{};
    const bool valid = GetObjectW(bitmap, sizeof(details), &details) != 0;
    if (!valid) { failed = true; error = L"invalid bitmap lifetime"; }
    result = {L"Hello world!", settings.target == L"ja" ? L"こんにちは世界！" : L"你好，世界！", L"en", settings.target};
    ++completed;
    // Return success even after cancellation to exercise suppression by the UI.
    return valid;
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc > 1 && std::string(argv[1]).find("--preview") == 0) showcase = argv[1];
    OleInitialize(nullptr);
    HDC screen = GetDC(nullptr);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, 360, 100);
    HDC image = CreateCompatibleDC(screen);
    auto old = SelectObject(image, bitmap);
    RECT area{0, 0, 360, 100};
    FillRect(image, &area, GetSysColorBrush(COLOR_WINDOW));
    HFONT font = CreateFontW(-26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto oldFont = SelectObject(image, font);
    TextOutW(image, 24, 30, L"Hello world!", 12);
    SelectObject(image, oldFont);
    SelectObject(image, old);
    DeleteObject(font);
    DeleteDC(image);
    ReleaseDC(nullptr, screen);
    GdiFlush();
    auto driver = std::thread([] {
        HWND window = nullptr;
        try {
            Check(Wait([&] { window = FindWindowW(L"DeskTodoTranslation", nullptr); return window != nullptr; }), "translation window created");
            if (!showcase.empty()) {
                Check(Wait([&] { return !Text(window, 6206).empty() && IsWindowEnabled(GetDlgItem(window, 6203)) &&
                    Text(window, 6215).find(L"翻译完成") == 0; }), "preview initial result");
                SendDlgItemMessageW(window, 6201, CB_SETCURSEL, 2, 0);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(6201, CBN_SELCHANGE), 0);
                SendDlgItemMessageW(window, 6202, CB_SETCURSEL, 0, 0);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(6202, CBN_SELCHANGE), 0);
                SendDlgItemMessageW(window, 6205, EM_SETSEL, 0, -1);
                SendDlgItemMessageW(window, 6205, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(ShowcaseOriginal.c_str()));
                SendMessageW(window, WM_COMMAND, 6203, 0);
                if (showcase == "--preview-loading") {
                    Check(Wait([&] { return requests == 2; }), "preview pending request");
                    std::this_thread::sleep_for(100ms);
                } else Check(Wait([&] { return IsWindowEnabled(GetDlgItem(window, 6203)) != FALSE; }), "preview completion");
                if (showcase == "--preview-compact") {
                    SetWindowPos(window, nullptr, 0, 0, 860, 680, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    std::this_thread::sleep_for(100ms);
                    RECT source{}, target{};
                    GetWindowRect(GetDlgItem(window, 6205), &source);
                    GetWindowRect(GetDlgItem(window, 6206), &target);
                    Check(source.top == target.top && source.right < target.left && source.bottom > source.top,
                        "compact window keeps aligned, separate reading panes");
                }
                SavePreview(window);
                PostMessageW(window, WM_CLOSE, 0, 0);
                return;
            }
            Check(Wait([&] { return requests == 1; }), "screenshot automatically translates without any credentials");
            Check(Wait([&] { return IsWindowEnabled(GetDlgItem(window, 6204)) && !IsWindowEnabled(GetDlgItem(window, 6203)); }),
                "request locks duplicate starts while keeping cancel available");
            PostMessageW(window, WM_COMMAND, 6204, 0);
            Check(Wait([&] { return Text(window, 6215) == L"已取消翻译。"; }), "cancel returns while worker is running");
            Check(completed == 0, "cancel did not wait for network completion");
            releaseCancelledRequest = true;
            Check(Wait([&] { return completed == 1; }), "cancelled worker completes safely");
            Check(Text(window, 6206).empty(), "cancelled result never appears");
            SendDlgItemMessageW(window, 6202, CB_SETCURSEL, 2, 0);
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(6202, CBN_SELCHANGE), 0);
            PostMessageW(window, WM_COMMAND, 6203, 0);
            Check(Wait([&] { return Text(window, 6206) == L"こんにちは世界！"; }), "retry uses newly selected target language");
            Check(Text(window, 6205) == L"Hello world!" && IsWindowEnabled(GetDlgItem(window, 6207)),
                "original and translated text appear together");
            Check((GetWindowLongPtrW(GetDlgItem(window, 6205), GWL_STYLE) & ES_READONLY) == 0,
                "recognized original can be corrected or replaced");
            Check(bitmapRequests == 2 && textRequests == 0, "initial and cancelled retry use owned screenshot inputs");
            SavePreview(window);
            PostMessageW(window, WM_COMMAND, 6207, 0);
            const bool copied = Wait([&] { return Text(window, 6215) == L"译文已复制。"; });
            if (!copied) std::wcerr << L"Copy status: " << Text(window, 6215) << L'\n';
            Check(copied, "copy translated result");
            SendDlgItemMessageW(window, 6205, EM_SETSEL, 0, -1);
            SendDlgItemMessageW(window, 6205, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"Edited source"));
            PostMessageW(window, WM_COMMAND, 6209, 0);
            Check(Wait([&] { return Text(window, 6215) == L"请先完成一次翻译，或手动选择源语言后再交换。"; }) &&
                Text(window, 6205) == L"Edited source", "editing invalidates the previous detected language before swap");
            SendDlgItemMessageW(window, 6202, CB_SETCURSEL, 0, 0);
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(6202, CBN_SELCHANGE), 0);
            PostMessageW(window, WM_COMMAND, 6203, 0);
            Check(Wait([&] { return Text(window, 6206) == L"你好，世界！"; }) && textRequests == 1 && bitmapRequests == 2,
                "target language change translates edited source without running OCR again");
            Check(Text(window, 6205) == L"Edited source", "edited source is not overwritten by old OCR");
            PostMessageW(window, WM_COMMAND, 6212, 0);
            Check(Wait([&] { return Text(window, 6215) == L"对照文本已复制。"; }), "copy bilingual comparison");
            PostMessageW(window, WM_COMMAND, 6209, 0);
            Check(Wait([&] { return Text(window, 6205) == L"你好，世界！"; }) && Text(window, 6206) == L"Edited source",
                "swap exchanges languages and both text panes");
            SendDlgItemMessageW(window, 6201, CB_SETCURSEL, 2, 0);
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(6201, CBN_SELCHANGE), 0);
            Check(Wait([&] { return Text(window, 6215) == L"源语言已切换，将使用你编辑的原文翻译。"; }) &&
                Text(window, 6205) == L"你好，世界！", "selecting source language preserves edited or pasted original");
            PostMessageW(window, WM_COMMAND, 6210, 0);
            Check(Wait([&] { return completed == 4 && Text(window, 6205) == L"Hello world!"; }) && bitmapRequests == 3,
                "explicit re-recognition replaces edits using the screenshot");
            SendDlgItemMessageW(window, 6201, CB_SETCURSEL, 3, 0);
            PostMessageW(window, WM_COMMAND, MAKEWPARAM(6201, CBN_SELCHANGE), 0);
            Check(Wait([&] { return Text(window, 6205).empty(); }), "source language change discards stale recognition");
            PostMessageW(window, WM_COMMAND, 6203, 0);
            Check(Wait([&] { return requests == 5 && bitmapRequests == 4; }), "source change reruns local OCR");
            PostMessageW(window, WM_CLOSE, 0, 0);
            Check(Wait([&] { return !IsWindow(window); }) && completed == 4,
                "closing the result window does not wait for the worker");
            releaseClosedRequest = true;
        } catch (const std::exception& error) {
            releaseCancelledRequest = releaseClosedRequest = true;
            failure = error.what();
            failed = true;
            if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        }
    });
    TranslationWindow::Show(nullptr, bitmap);
    driver.join();
    Check(Wait([&] { return completed == requests; }), "detached worker releases owned bitmap after close");
    if (failed) { std::cerr << failure << '\n'; return 1; }
    std::cout << "Translation window: " << checks << " checks passed; mock backend, no network.\n";
    return 0;
}
