#pragma once
#include <Windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace TranslationService {
enum class Provider { Google, MyMemory };
struct Language { const wchar_t* code; const wchar_t* name; };
const std::vector<Language>& Languages();
struct Settings {
    Provider provider = Provider::Google;
    std::wstring source = L"auto", target = L"zh-Hans";
};
struct Result { std::wstring original, translated, source, target; };
struct Request { Provider provider; std::wstring host, path; };
struct TextChunk { std::wstring text, separator; };
// Only language/provider preferences are stored. No account or key is required.
Settings LoadSettings();
bool SaveSettings(const Settings& settings, std::wstring& error);
using Transport = std::function<bool(const Request&, std::string&, std::wstring&,
                                    const std::atomic<bool>&)>;
bool TranslateText(const std::wstring& text, const Settings& settings, Result& result,
                   std::wstring& error, const std::atomic<bool>& cancelled,
                   const Transport& transport = {});
bool TranslateBitmap(HBITMAP bitmap, const Settings& settings, Result& result,
                     std::wstring& error, const std::atomic<bool>& cancelled,
                     const Transport& transport = {});
std::wstring DetectLanguage(const std::wstring& text);
std::string FormEncode(const std::string& value);
std::vector<TextChunk> SplitText(const std::wstring& text, size_t maximumUtf8Bytes);
bool ParseResponse(Provider provider, const std::string& json, Result& result, std::wstring& error);
}
