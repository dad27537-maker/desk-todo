#include "TranslationService.h"
#include "AtomicFile.h"
#include "DesktopPathService.h"
#include "OcrService.h"
#include <winhttp.h>
#include <elscore.h>
#include <elssrvc.h>
#include <roapi.h>
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
using namespace TranslationService;
using namespace winrt::Windows::Data::Json;
constexpr size_t MaxResponse = 2 * 1024 * 1024, MaxText = 20000;
struct RuntimeScope {
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    ~RuntimeScope() { if (SUCCEEDED(hr)) RoUninitialize(); }
};
struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
std::wstring ConfigPath() { return GetAppDataPath() + L"\\translation-preferences.json"; }
bool IsLanguage(const std::wstring& code, bool source) {
    if (source && code == L"auto") return true;
    for (const auto& language : Languages()) if (code == language.code) return true;
    return false;
}
std::wstring CanonicalLanguage(std::wstring code) {
    if (code == L"zh-CN" || code == L"zh-CHS" || code == L"zh" || code == L"zh-Hans-CN") return L"zh-Hans";
    if (code == L"zh-TW" || code == L"zh-CHT" || code == L"zh-Hant-TW") return L"zh-Hant";
    if (IsLanguage(code, false)) return code;
    auto dash = code.find(L'-');
    if (dash != std::wstring::npos) code.resize(dash);
    return IsLanguage(code, false) ? code : L"";
}
std::wstring RemoteLanguage(const std::wstring& code) {
    if (code == L"zh-Hans") return L"zh-CN";
    if (code == L"zh-Hant") return L"zh-TW";
    return code;
}
bool Send(const Request& request, std::string& response, std::wstring& error,
          const std::atomic<bool>& cancelled) {
    auto failed = [&] {
        error = cancelled ? L"已取消翻译。" : L"无法连接翻译服务，请检查网络或切换翻译引擎后重试。";
        return false;
    };
    HttpHandle session{WinHttpOpen(L"DeskTodo/1.7", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 15000)) return failed();
    HttpHandle connection{WinHttpConnect(session.value, request.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) return failed();
    HttpHandle handle{WinHttpOpenRequest(connection.value, L"GET", request.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!handle.value) return failed();
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(handle.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))) return failed();
    if (cancelled) return failed();
    if (!WinHttpSendRequest(handle.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(handle.value, nullptr)) return failed();
    DWORD status = 0, length = sizeof(status);
    if (!WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) return failed();
    if (status != 200) {
        error = status == 429 ? L"翻译服务访问次数受限，请稍后重试或切换引擎。" :
            L"翻译服务返回 HTTP " + std::to_wstring(status) + L"，请切换引擎或稍后重试。";
        return false;
    }
    response.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!cancelled) {
        char buffer[16384];
        DWORD read = 0;
        if (!WinHttpReadData(handle.value, buffer, sizeof(buffer), &read)) return failed();
        if (!read) return true;
        if (response.size() + read > MaxResponse || std::chrono::steady_clock::now() > deadline) {
            error = L"翻译响应过大或超时，请缩短原文后重试。";
            return false;
        }
        response.append(buffer, read);
    }
    return failed();
}
std::wstring DecodeEntities(const std::wstring& input) {
    std::wstring output;
    for (size_t index = 0; index < input.size(); ++index) {
        if (input[index] != L'&') { output += input[index]; continue; }
        const auto end = input.find(L';', index + 1);
        if (end == std::wstring::npos || end - index > 12) { output += input[index]; continue; }
        auto entity = input.substr(index + 1, end - index - 1);
        unsigned long code = 0;
        if (entity == L"amp") code = L'&';
        else if (entity == L"quot") code = L'"';
        else if (entity == L"apos") code = L'\'';
        else if (entity == L"lt") code = L'<';
        else if (entity == L"gt") code = L'>';
        else if (entity.size() > 1 && entity[0] == L'#') {
            try {
                const bool hex = entity.size() > 2 && (entity[1] == L'x' || entity[1] == L'X');
                const auto digits = entity.substr(hex ? 2 : 1);
                size_t consumed = 0;
                code = std::stoul(digits, &consumed, hex ? 16 : 10);
                if (consumed != digits.size()) code = 0;
            } catch (...) { code = 0; }
        }
        if (!code || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) { output += input[index]; continue; }
        if (code <= 0xFFFF) output += static_cast<wchar_t>(code);
        else {
            code -= 0x10000;
            output += static_cast<wchar_t>(0xD800 + (code >> 10));
            output += static_cast<wchar_t>(0xDC00 + (code & 1023));
        }
        index = end;
    }
    return output;
}
}

const std::vector<TranslationService::Language>& TranslationService::Languages() {
    static const std::vector<Language> languages{
        {L"zh-Hans", L"简体中文"}, {L"zh-Hant", L"繁體中文"}, {L"en", L"英语"},
        {L"ja", L"日语"}, {L"ko", L"韩语"}, {L"fr", L"法语"}, {L"de", L"德语"},
        {L"es", L"西班牙语"}, {L"ru", L"俄语"}, {L"pt", L"葡萄牙语"},
        {L"it", L"意大利语"}, {L"vi", L"越南语"}, {L"th", L"泰语"},
        {L"ar", L"阿拉伯语"}, {L"hi", L"印地语"}, {L"id", L"印度尼西亚语"},
        {L"nl", L"荷兰语"}, {L"ms", L"马来语"}, {L"tr", L"土耳其语"},
        {L"uk", L"乌克兰语"}, {L"pl", L"波兰语"}, {L"sv", L"瑞典语"}};
    return languages;
}
TranslationService::Settings TranslationService::LoadSettings() {
    RuntimeScope runtime;
    Settings settings;
    try {
        std::ifstream file(std::filesystem::path(ConfigPath()), std::ios::binary | std::ios::ate);
        const auto length = file.tellg();
        if (!file || length <= 0 || length > 8192) return settings;
        std::string bytes(static_cast<size_t>(length), '\0');
        file.seekg(0);
        if (!file.read(bytes.data(), bytes.size())) return settings;
        JsonObject json;
        if (!JsonObject::TryParse(winrt::to_hstring(bytes), json)) return settings;
        const std::wstring source = json.GetNamedString(L"source", L"auto").c_str();
        const std::wstring target = json.GetNamedString(L"target", L"zh-Hans").c_str();
        if (IsLanguage(source, true)) settings.source = source;
        if (IsLanguage(target, false)) settings.target = target;
        if (json.GetNamedString(L"provider", L"google") == L"mymemory") settings.provider = Provider::MyMemory;
    } catch (...) { return Settings{}; }
    return settings;
}
bool TranslationService::SaveSettings(const Settings& settings, std::wstring& error) {
    RuntimeScope runtime;
    error.clear();
    try {
        if (!IsLanguage(settings.source, true) || !IsLanguage(settings.target, false)) throw std::runtime_error("language");
        JsonObject json;
        json.SetNamedValue(L"version", JsonValue::CreateNumberValue(1));
        json.SetNamedValue(L"provider", JsonValue::CreateStringValue(settings.provider == Provider::MyMemory ? L"mymemory" : L"google"));
        json.SetNamedValue(L"source", JsonValue::CreateStringValue(settings.source));
        json.SetNamedValue(L"target", JsonValue::CreateStringValue(settings.target));
        if (!EnsureDirectoryExists(GetAppDataPath())) throw std::runtime_error("directory");
        if (!AtomicFile::Write(ConfigPath(), winrt::to_string(json.Stringify()))) throw std::runtime_error("write");
        return true;
    } catch (...) { error = L"无法保存翻译偏好，请检查配置目录权限。"; return false; }
}
std::wstring TranslationService::DetectLanguage(const std::wstring& text) {
    MAPPING_ENUM_OPTIONS options{};
    options.Size = sizeof(options);
    options.pGuid = const_cast<GUID*>(&ELS_GUID_LANGUAGE_DETECTION);
    PMAPPING_SERVICE_INFO services = nullptr;
    DWORD count = 0;
    std::wstring language;
    if (SUCCEEDED(MappingGetServices(&options, &services, &count))) {
        if (count) {
            MAPPING_PROPERTY_BAG bag{};
            bag.Size = sizeof(bag);
            if (SUCCEEDED(MappingRecognizeText(services, text.c_str(),
                static_cast<DWORD>((std::min)(text.size(), size_t{4096})), 0, nullptr, &bag))) {
                if (bag.dwRangesCount && bag.prgResultRanges && bag.prgResultRanges[0].pData) {
                    const auto& range = bag.prgResultRanges[0];
                    const auto* data = static_cast<const wchar_t*>(range.pData);
                    const size_t capacity = range.dwDataSize / sizeof(wchar_t);
                    size_t length = 0;
                    while (length < capacity && data[length]) ++length;
                    if (length && length < capacity) language = CanonicalLanguage(std::wstring(data, length));
                }
                MappingFreePropertyBag(&bag);
            }
        }
        MappingFreeServices(services);
    }
    return language;
}
std::string TranslationService::FormEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
std::vector<TranslationService::TextChunk> TranslationService::SplitText(const std::wstring& text, size_t maximum) {
    std::vector<TextChunk> chunks;
    if (maximum < 4) throw std::invalid_argument("chunk size");
    size_t start = 0;
    while (start < text.size()) {
        if (iswspace(text[start])) {
            size_t end = start + 1;
            while (end < text.size() && iswspace(text[end])) ++end;
            if (chunks.empty()) chunks.push_back({L"", text.substr(start, end - start)});
            else chunks.back().separator += text.substr(start, end - start);
            start = end;
            continue;
        }
        size_t end = start, bytes = 0, boundary = start;
        while (end < text.size() && text[end] != L'\r' && text[end] != L'\n') {
            const wchar_t c = text[end];
            const bool pair = c >= 0xD800 && c <= 0xDBFF && end + 1 < text.size() &&
                text[end + 1] >= 0xDC00 && text[end + 1] <= 0xDFFF;
            const size_t count = pair ? 4 : (c < 0x80 ? 1 : (c < 0x800 ? 2 : 3));
            if (bytes + count > maximum) break;
            bytes += count;
            end += pair ? 2 : 1;
            if (iswspace(c) || c == L'.' || c == L'!' || c == L'?' || c == L'。' || c == L'！' || c == L'？') boundary = end;
        }
        if (end < text.size() && text[end] != L'\r' && text[end] != L'\n' && boundary > start + (end - start) / 2) end = boundary;
        size_t trimmed = end;
        while (trimmed > start && iswspace(text[trimmed - 1])) --trimmed;
        chunks.push_back({text.substr(start, trimmed - start), text.substr(trimmed, end - trimmed)});
        start = end;
    }
    return chunks;
}
bool TranslationService::ParseResponse(Provider provider, const std::string& response, Result& result, std::wstring& error) {
    RuntimeScope runtime;
    result = {};
    error.clear();
    try {
        if (response.size() > MaxResponse) throw std::length_error("response");
        Result parsed;
        if (provider == Provider::Google) {
            JsonArray json;
            if (!JsonArray::TryParse(winrt::to_hstring(response), json) || json.Size() < 3) throw std::runtime_error("json");
            for (const auto& segment : json.GetArrayAt(0)) {
                const auto item = segment.GetArray();
                if (item.Size() < 1) throw std::runtime_error("segment");
                parsed.translated += item.GetStringAt(0).c_str();
            }
            parsed.source = CanonicalLanguage(json.GetStringAt(2).c_str());
        } else {
            auto json = JsonObject::Parse(winrt::to_hstring(response));
            const auto status = json.GetNamedValue(L"responseStatus");
            const bool ok = (status.ValueType() == JsonValueType::Number && status.GetNumber() == 200) ||
                (status.ValueType() == JsonValueType::String && status.GetString() == L"200");
            if (!ok) { error = L"MyMemory 翻译暂不可用或免费额度已用完，请切换引擎或稍后重试。"; return false; }
            if (json.HasKey(L"quotaFinished") && json.GetNamedBoolean(L"quotaFinished")) {
                error = L"MyMemory 免费额度已用完，请切换引擎或稍后重试。"; return false;
            }
            parsed.translated = DecodeEntities(json.GetNamedObject(L"responseData").GetNamedString(L"translatedText").c_str());
        }
        if (parsed.translated.find_first_not_of(L" \t\r\n") == std::wstring::npos) throw std::runtime_error("empty");
        result = std::move(parsed);
        return true;
    } catch (...) { error = L"翻译服务返回的数据无效，请切换引擎或稍后重试。"; return false; }
}
bool TranslationService::TranslateText(const std::wstring& text, const Settings& settings,
    Result& result, std::wstring& error, const std::atomic<bool>& cancelled, const Transport& transport) {
    RuntimeScope runtime;
    result = {};
    result.original = text;
    result.target = settings.target;
    error.clear();
    if (cancelled) { error = L"已取消翻译。"; return false; }
    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) { error = L"请在原文区域输入或粘贴要翻译的文字。"; return false; }
    if (text.size() > MaxText) { error = L"原文超过 20000 字符，请分段翻译。"; return false; }
    if (!IsLanguage(settings.source, true) || !IsLanguage(settings.target, false)) { error = L"请选择支持的源语言和目标语言。"; return false; }
    std::wstring source = settings.source;
    if (source == L"auto" && settings.provider == Provider::MyMemory) {
        source = DetectLanguage(text);
        if (source.empty()) { error = L"无法确定原文语言，请手动选择源语言后重试。"; return false; }
    }
    result.source = source;
    if (source == settings.target) { result.translated = text; return true; }
    try {
        const auto chunks = SplitText(text, settings.provider == Provider::Google ? 3500 : 500);
        size_t requests = 0;
        for (const auto& chunk : chunks) if (!chunk.text.empty()) ++requests;
        if (requests > 64) { error = L"原文段落过多，请分段翻译。"; return false; }
        std::wstring translated, detected;
        for (const auto& chunk : chunks) {
            if (cancelled) { error = L"已取消翻译。"; return false; }
            if (chunk.text.empty()) { translated += chunk.separator; continue; }
            Request request{settings.provider, L"", L""};
            const auto query = std::wstring(winrt::to_hstring(FormEncode(winrt::to_string(chunk.text))).c_str());
            if (settings.provider == Provider::Google) {
                request.host = L"translate.googleapis.com";
                request.path = L"/translate_a/single?client=gtx&sl=" + RemoteLanguage(source) +
                    L"&tl=" + RemoteLanguage(settings.target) + L"&dt=t&q=" + query;
            } else {
                request.host = L"api.mymemory.translated.net";
                request.path = L"/get?q=" + query + L"&langpair=" + RemoteLanguage(source) + L"%7C" + RemoteLanguage(settings.target);
            }
            std::string response;
            const bool sent = transport ? transport(request, response, error, cancelled) : Send(request, response, error, cancelled);
            if (cancelled) { error = L"已取消翻译。"; return false; }
            if (!sent) return false;
            Result part;
            if (!ParseResponse(settings.provider, response, part, error)) return false;
            translated += part.translated + chunk.separator;
            if (detected.empty() && !part.source.empty()) detected = part.source;
        }
        if (cancelled) { error = L"已取消翻译。"; return false; }
        result.translated = std::move(translated);
        result.source = source == L"auto" ? detected : source;
        return true;
    } catch (...) { error = L"翻译失败，请缩短原文或切换引擎后重试。"; return false; }
}
bool TranslationService::TranslateBitmap(HBITMAP bitmap, const Settings& settings,
    Result& result, std::wstring& error, const std::atomic<bool>& cancelled, const Transport& transport) {
    RuntimeScope runtime;
    result = {};
    error.clear();
    if (cancelled) { error = L"已取消翻译。"; return false; }
    std::wstring text;
    if (!OcrService::RecognizeBitmapForLanguage(bitmap, text, error, settings.source)) return false;
    return TranslateText(text, settings, result, error, cancelled, transport);
}
