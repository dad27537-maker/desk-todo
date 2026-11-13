#include "Services/TranslationService.h"
#include "Services/OcrService.h"
#include "Services/DesktopPathService.h"
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <winrt/base.h>

namespace {
int checks = 0, ocrCalls = 0;
std::wstring ocrLanguage;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
constexpr char GoogleFixture[] = R"([[["你好","Hello"],["世界 🌍","world"]],null,"en"])";
constexpr char MemoryFixture[] = R"({"responseStatus":200,"quotaFinished":false,"responseData":{"translatedText":"你好 &amp; &#x1F30D; &#39;世界&#39;"}})";
}
bool OcrService::RecognizeBitmapForLanguage(HBITMAP bitmap, std::wstring& text,
    std::wstring& error, const std::wstring& tag) {
    ++ocrCalls;
    ocrLanguage = tag;
    text.clear();
    if (!bitmap) { error = L"invalid bitmap"; return false; }
    text = L"Hello world!";
    return true;
}
int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!GetEnvironmentVariableW(L"DESKTODO_DATA_ROOT", nullptr, 0)) {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const auto isolated = std::wstring(temp) + L"DeskTodoTranslationSmoke-" + std::to_wstring(GetCurrentProcessId());
        SetEnvironmentVariableW(L"DESKTODO_DATA_ROOT", isolated.c_str());
    }
    HBITMAP bitmap = nullptr;
    try {
        using namespace TranslationService;
        Check(FormEncode("a+b/= &\r\n") == "a%2Bb%2F%3D%20%26%0D%0A", "reserved query characters are encoded");
        Result result;
        std::wstring error;
        Check(ParseResponse(Provider::Google, GoogleFixture, result, error), "Google segmented response parses");
        Check(result.translated == L"你好世界 🌍" && result.source == L"en", "Unicode segments and detected language");
        Check(ParseResponse(Provider::MyMemory, MemoryFixture, result, error) &&
            result.translated == L"你好 & 🌍 '世界'", "MyMemory response and HTML entities");
        Check(ParseResponse(Provider::MyMemory, R"({"responseStatus":"200","responseData":{"translatedText":"ok"}})", result, error), "string response status accepted");
        for (const char* invalid : {"bad json", "[]", "[[],null,\"en\"]", "[[[null]],null,\"en\"]", "[[[\"ok\"],[]],null,\"en\"]"})
            Check(!ParseResponse(Provider::Google, invalid, result, error) && result.translated.empty() && !error.empty(), "invalid or partial Google results are discarded");
        for (const char* invalid : {"bad json", "{}", R"({"responseStatus":429})", R"({"responseStatus":200,"quotaFinished":true})",
                R"({"responseStatus":200,"responseData":{"translatedText":" "}})"})
            Check(!ParseResponse(Provider::MyMemory, invalid, result, error) && result.translated.empty() && !error.empty(), "invalid, quota or empty memory results are rejected");
        const std::wstring original = L"  Hello world!\r\n\r\n你好 🌍  bonjour le monde.\n";
        std::wstring rebuilt;
        for (const auto& chunk : SplitText(original, 12)) {
            Check(winrt::to_string(chunk.text).size() <= 12, "chunks obey UTF-8 bytes without splitting surrogate pairs");
            rebuilt += chunk.text + chunk.separator;
        }
        Check(rebuilt == original, "chunking preserves every character, space and CRLF");
        Check(DetectLanguage(L"This is an English sentence about translating a screenshot into another language.") == L"en", "local Windows language detection");
        Settings settings;
        std::atomic<bool> cancelled{false};
        int requests = 0;
        auto transport = [&](const Request& request, std::string& response, std::wstring&, const std::atomic<bool>&) {
            ++requests;
            Check(request.host == L"translate.googleapis.com" && request.path.find(L"sl=auto&tl=zh-CN") != std::wstring::npos,
                "default provider is keyless and auto-detects language");
            Check(request.path.find(L"q=Hello%20world%21") != std::wstring::npos && request.path.find(L"base64") == std::wstring::npos,
                "only recognized UTF-8 text is transmitted, never the screenshot");
            response = GoogleFixture;
            return true;
        };
        Check(TranslateText(L"Hello world!", settings, result, error, cancelled, transport) && requests == 1,
            "no credentials are required for text translation");
        Check(result.original == L"Hello world!" && result.target == L"zh-Hans", "original text and requested language preserved");
        HDC screen = GetDC(nullptr);
        bitmap = CreateCompatibleBitmap(screen, 80, 40);
        ReleaseDC(nullptr, screen);
        Check(TranslateBitmap(bitmap, settings, result, error, cancelled, transport) && ocrCalls == 1 && ocrLanguage == L"auto",
            "screenshot goes through local OCR before text-only translation");
        Check(!TranslateBitmap(nullptr, settings, result, error, cancelled, transport) && result.translated.empty(), "invalid screenshot never produces a stale translation");
        const auto before = requests;
        cancelled = true;
        Check(!TranslateText(L"Hello world!", settings, result, error, cancelled, transport) && requests == before, "cancelled job cannot start requests");
        cancelled = false;
        Check(!TranslateText(L"  \r\n", settings, result, error, cancelled, transport) && requests == before, "empty input rejected before network");
        Check(!TranslateText(std::wstring(20001, L'x'), settings, result, error, cancelled, transport) && requests == before, "excessive input rejected before network");
        auto same = settings;
        same.source = same.target;
        Check(TranslateText(L"你好", same, result, error, cancelled, transport) && requests == before && result.translated == L"你好", "same-language input stays local");
        auto invalid = settings;
        invalid.target = L"invalid";
        Check(!TranslateText(L"hello", invalid, result, error, cancelled, transport) && requests == before, "unsupported language is rejected");
        auto cancelling = [&](const Request&, std::string& response, std::wstring&, const std::atomic<bool>&) {
            response = GoogleFixture;
            cancelled = true;
            return true;
        };
        Check(!TranslateText(L"Hello", settings, result, error, cancelled, cancelling) && result.translated.empty(), "late completion cannot publish cancelled result");
        cancelled = false;
        int partials = 0;
        auto partial = [&](const Request&, std::string& response, std::wstring& transportError, const std::atomic<bool>&) {
            response = GoogleFixture;
            if (++partials == 2) { transportError = L"offline"; return false; }
            return true;
        };
        Check(!TranslateText(L"First line\nSecond line", settings, result, error, cancelled, partial) &&
            result.translated.empty() && result.original == L"First line\nSecond line", "network failure keeps original but discards partial translation");
        auto memory = settings;
        memory.provider = Provider::MyMemory;
        memory.source = L"en";
        memory.target = L"ja";
        auto memoryTransport = [&](const Request& request, std::string& response, std::wstring&, const std::atomic<bool>&) {
            Check(request.host == L"api.mymemory.translated.net" && request.path.find(L"langpair=en%7Cja") != std::wstring::npos,
                "alternative provider uses selected source and target");
            response = MemoryFixture;
            return true;
        };
        Check(TranslateText(L"Hello world!", memory, result, error, cancelled, memoryTransport), "alternative provider translates independently");
        Check(SaveSettings(memory, error), "save only provider and language preferences");
        const auto loaded = LoadSettings();
        Check(loaded.source == L"en" && loaded.target == L"ja" && loaded.provider == Provider::MyMemory, "preferences roundtrip");
        const auto path = std::filesystem::path(GetAppDataPath()) / L"translation-preferences.json";
        { std::ofstream corrupt(path, std::ios::binary | std::ios::trunc); corrupt << "damaged"; }
        Check(LoadSettings().source == L"auto" && LoadSettings().target == L"zh-Hans", "corrupt preferences safely use defaults");
        DeleteFileW(path.c_str());
        DeleteObject(bitmap);
        std::cout << "Translation: " << checks << " offline contract checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        if (bitmap) DeleteObject(bitmap);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
