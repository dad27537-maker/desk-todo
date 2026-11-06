#include "OcrService.h"
#include "PaddleOcrService.h"

#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Globalization.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct __declspec(uuid("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D"))
IMemoryBufferByteAccess : IUnknown {
    virtual HRESULT __stdcall GetBuffer(BYTE** value, UINT32* capacity) = 0;
};

class RuntimeScope {
public:
    RuntimeScope() : result_(RoInitialize(RO_INIT_SINGLETHREADED)) {}
    ~RuntimeScope() { if (SUCCEEDED(result_)) RoUninitialize(); }
    bool Ready() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT result_;
};

HBITMAP PrepareWindowsBitmap(HBITMAP source, int maximum) {
    BITMAP details{};
    if (!GetObjectW(source, sizeof(details), &details) || details.bmWidth <= 0 || details.bmHeight <= 0 ||
        static_cast<unsigned long long>(details.bmWidth) * details.bmHeight > 64ull * 1024 * 1024) return nullptr;
    constexpr int padding = 16;
    double scale = (std::min)(4.0, (std::max)(1.0, 96.0 / (std::min)(details.bmWidth, details.bmHeight)));
    scale = (std::min)(scale, (maximum - padding * 2.0) / (std::max)(details.bmWidth, details.bmHeight));
    int width = (std::max)(1, static_cast<int>(std::round(details.bmWidth * scale)));
    int height = (std::max)(1, static_cast<int>(std::round(details.bmHeight * scale)));
    HDC screen = GetDC(nullptr);
    HDC input = CreateCompatibleDC(screen), output = CreateCompatibleDC(screen);
    HBITMAP prepared = CreateCompatibleBitmap(screen, width + padding * 2, height + padding * 2);
    if (!input || !output || !prepared) {
        if (input) DeleteDC(input);
        if (output) DeleteDC(output);
        if (prepared) DeleteObject(prepared);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    auto oldInput = SelectObject(input, source), oldOutput = SelectObject(output, prepared);
    COLORREF background = GetPixel(input, 0, 0);
    if (background == CLR_INVALID) background = RGB(255, 255, 255);
    HBRUSH brush = CreateSolidBrush(background);
    RECT rect{0, 0, width + padding * 2, height + padding * 2};
    FillRect(output, &rect, brush);
    DeleteObject(brush);
    SetStretchBltMode(output, HALFTONE);
    SetBrushOrgEx(output, 0, 0, nullptr);
    BOOL success = StretchBlt(output, padding, padding, width, height, input, 0, 0,
                              details.bmWidth, details.bmHeight, SRCCOPY);
    SelectObject(input, oldInput);
    SelectObject(output, oldOutput);
    DeleteDC(input);
    DeleteDC(output);
    ReleaseDC(nullptr, screen);
    if (!success) { DeleteObject(prepared); return nullptr; }
    return prepared;
}

struct ScopedBitmap {
    HBITMAP handle = nullptr;
    ~ScopedBitmap() { if (handle) DeleteObject(handle); }
};

}

static bool RecognizeWindowsBitmap(HBITMAP bitmap, std::wstring& text,
                                    std::wstring& error) {
    text.clear();
    error.clear();
    if (!bitmap) {
        error = L"截图位图无效。";
        return false;
    }
    RuntimeScope runtime;
    if (!runtime.Ready()) {
        error = L"无法初始化 Windows OCR。";
        return false;
    }
    const wchar_t* stage = L"bitmap";
    try {
        ScopedBitmap prepared{PrepareWindowsBitmap(bitmap,
            static_cast<int>(winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension()))};
        if (!prepared.handle) {
            error = L"无法准备 OCR 截图。";
            return false;
        }
        bitmap = prepared.handle;
        BITMAP details{};
        if (!GetObjectW(bitmap, sizeof(details), &details) ||
            details.bmWidth <= 0 || details.bmHeight <= 0) {
            error = L"无法读取截图尺寸。";
            return false;
        }
        const int width = details.bmWidth;
        const int height = details.bmHeight;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        HDC dc = GetDC(nullptr);
        const int rows = GetDIBits(dc, bitmap, 0, height, pixels.data(), &info,
                                    DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        if (rows != height) {
            error = L"无法读取截图像素。";
            return false;
        }
        const bool invert = (pixels[0] + pixels[1] + pixels[2]) < 3 * 128;
        BYTE darkest = 255, lightest = 0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            BYTE gray = static_cast<BYTE>((29 * pixels[i] + 150 * pixels[i + 1] + 77 * pixels[i + 2]) >> 8);
            if (invert) gray = 255 - gray;
            pixels[i] = pixels[i + 1] = pixels[i + 2] = gray;
            pixels[i + 3] = 255;
            darkest = (std::min)(darkest, gray);
            lightest = (std::max)(lightest, gray);
        }
        const int range = lightest - darkest;
        if (range >= 8 && range < 160) {
            for (size_t i = 0; i < pixels.size(); i += 4)
                pixels[i] = pixels[i + 1] = pixels[i + 2] =
                    static_cast<BYTE>((pixels[i] - darkest) * 255 / range);
        }

        stage = L"software-bitmap";
        using namespace winrt::Windows::Graphics::Imaging;
        SoftwareBitmap software(BitmapPixelFormat::Bgra8, width, height,
                                BitmapAlphaMode::Ignore);
        {
            stage = L"lock-buffer";
            BitmapBuffer buffer = software.LockBuffer(BitmapBufferAccessMode::Write);
            stage = L"buffer-reference";
            auto reference = buffer.CreateReference();
            stage = L"buffer-byte-access";
            auto access = reference.as<IMemoryBufferByteAccess>();
            BYTE* target = nullptr;
            UINT32 capacity = 0;
            stage = L"get-buffer";
            winrt::check_hresult(access->GetBuffer(&target, &capacity));
            const BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
            const size_t rowBytes = static_cast<size_t>(width) * 4;
            if (!target || plane.StartIndex +
                static_cast<UINT32>((height - 1) * plane.Stride + rowBytes) > capacity) {
                error = L"OCR 像素缓冲区无效。";
                return false;
            }
            for (int row = 0; row < height; ++row) {
                std::memcpy(target + plane.StartIndex + row * plane.Stride,
                            pixels.data() + static_cast<size_t>(row) * rowBytes,
                            rowBytes);
            }
        }

        stage = L"ocr-engine";
        using winrt::Windows::Media::Ocr::OcrEngine;
        std::vector<OcrEngine> engines;
        for (const wchar_t* tag : {L"zh-Hans-CN", L"zh-Hant-TW", L"en-US"}) {
            auto language = winrt::Windows::Globalization::Language(tag);
            if (OcrEngine::IsLanguageSupported(language)) {
                auto engine = OcrEngine::TryCreateFromLanguage(language);
                if (engine) engines.push_back(engine);
            }
        }
        if (engines.empty()) {
            auto engine = OcrEngine::TryCreateFromUserProfileLanguages();
            if (engine) engines.push_back(engine);
        }
        if (engines.empty()) {
            error = L"系统未安装可用的中英文 OCR 语言包。";
            return false;
        }
        stage = L"recognize";
        for (const auto& engine : engines) {
            auto result = engine.RecognizeAsync(software).get();
            for (const auto& line : result.Lines()) {
                if (!text.empty()) text += L"\r\n";
                text += line.Text().c_str();
            }
            if (!text.empty()) break;
        }
        if (text.empty()) {
            error = L"截图中没有识别到文字。";
            return false;
        }
        return true;
    } catch (const winrt::hresult_error& exception) {
        wchar_t code[32]{};
        swprintf_s(code, L"0x%08X", static_cast<unsigned int>(exception.code().value));
        error = L"Windows OCR failed at " + std::wstring(stage) + L" (" +
                std::wstring(code) + L"): " +
                std::wstring(exception.message().c_str());
        return false;
    } catch (...) {
        error = L"OCR 识别发生未知错误。";
        return false;
    }
}

bool OcrService::RecognizeBitmap(HBITMAP bitmap, std::wstring& text,
                                 std::wstring& error, Backend backend) {
    text.clear();
    error.clear();
    if (!bitmap) { error = L"截图位图无效。"; return false; }
    std::wstring paddleError;
    if (backend != Backend::Windows) {
        if (PaddleOcrService::RecognizeBitmap(bitmap, text, paddleError)) return true;
        if (backend == Backend::Paddle) { error = std::move(paddleError); return false; }
    }
    if (RecognizeWindowsBitmap(bitmap, text, error)) return true;
    return false;
}

namespace {

void ReplaceAll(std::wstring& value, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return;
    size_t position = 0;
    while ((position = value.find(from, position)) != std::wstring::npos) {
        value.replace(position, from.size(), to);
        position += to.size();
    }
}

wchar_t SuperscriptDigit(wchar_t value) {
    switch (value) {
    case L'⁰': return L'0'; case L'¹': return L'1'; case L'²': return L'2';
    case L'³': return L'3'; case L'⁴': return L'4'; case L'⁵': return L'5';
    case L'⁶': return L'6'; case L'⁷': return L'7'; case L'⁸': return L'8';
    case L'⁹': return L'9'; default: return 0;
    }
}

wchar_t SubscriptDigit(wchar_t value) {
    switch (value) {
    case L'₀': return L'0'; case L'₁': return L'1'; case L'₂': return L'2';
    case L'₃': return L'3'; case L'₄': return L'4'; case L'₅': return L'5';
    case L'₆': return L'6'; case L'₇': return L'7'; case L'₈': return L'8';
    case L'₉': return L'9'; default: return 0;
    }
}

std::wstring NormalizeScripts(const std::wstring& source) {
    std::wstring output;
    for (size_t index = 0; index < source.size();) {
        const wchar_t super = SuperscriptDigit(source[index]);
        const wchar_t sub = SubscriptDigit(source[index]);
        if (!super && !sub) {
            output.push_back(source[index++]);
            continue;
        }
        output += super ? L"^{" : L"_{";
        while (index < source.size()) {
            const wchar_t digit = super ? SuperscriptDigit(source[index])
                                        : SubscriptDigit(source[index]);
            if (!digit) break;
            output.push_back(digit);
            ++index;
        }
        output.push_back(L'}');
    }
    return output;
}

bool IsLatexToken(wchar_t value) {
    return std::iswalnum(value) || value == L'.' || value == L'_' ||
           value == L'^' || value == L'}' || value == L'{';
}

std::wstring NormalizeSimpleFractions(std::wstring value) {
    size_t slash = 0;
    while ((slash = value.find(L'/', slash)) != std::wstring::npos) {
        size_t left = slash;
        while (left > 0 && IsLatexToken(value[left - 1])) --left;
        size_t right = slash + 1;
        while (right < value.size() && IsLatexToken(value[right])) ++right;
        if (left == slash || right == slash + 1) {
            ++slash;
            continue;
        }
        const std::wstring numerator = value.substr(left, slash - left);
        const std::wstring denominator = value.substr(slash + 1, right - slash - 1);
        const std::wstring fraction = L"\\frac{" + numerator + L"}{" + denominator + L"}";
        value.replace(left, right - left, fraction);
        slash = left + fraction.size();
    }
    return value;
}

}

std::wstring OcrService::ConvertTextToLatex(const std::wstring& text) {
    std::wstring latex = NormalizeScripts(text);
    constexpr std::pair<std::wstring_view, std::wstring_view> replacements[] = {
        { L"×", L"\\times " }, { L"÷", L"\\div " },
        { L"≤", L"\\le " }, { L"≥", L"\\ge " }, { L"≠", L"\\ne " },
        { L"±", L"\\pm " }, { L"∞", L"\\infty " },
        { L"π", L"\\pi " }, { L"α", L"\\alpha " }, { L"β", L"\\beta " },
        { L"γ", L"\\gamma " }, { L"δ", L"\\delta " }, { L"θ", L"\\theta " },
        { L"λ", L"\\lambda " }, { L"μ", L"\\mu " }, { L"σ", L"\\sigma " },
        { L"φ", L"\\phi " }, { L"ω", L"\\omega " },
        { L"Σ", L"\\sum " }, { L"∑", L"\\sum " }, { L"∫", L"\\int " },
        { L"→", L"\\to " }, { L"∈", L"\\in " }, { L"≈", L"\\approx " },
        { L"√", L"\\sqrt{}" }
    };
    for (const auto& replacement : replacements)
        ReplaceAll(latex, replacement.first, replacement.second);
    latex = NormalizeSimpleFractions(std::move(latex));
    ReplaceAll(latex, L"\r\n", L" \\\\ \r\n");
    ReplaceAll(latex, L"\n", L" \\\\ \r\n");
    while (!latex.empty() && std::iswspace(latex.front())) latex.erase(latex.begin());
    while (!latex.empty() && std::iswspace(latex.back())) latex.pop_back();
    return L"\\[\r\n" + latex + L"\r\n\\]";
}

bool OcrService::RecognizeLatexBitmap(HBITMAP bitmap, std::wstring& text,
                                      std::wstring& error) {
    std::wstring recognized;
    if (!RecognizeBitmap(bitmap, recognized, error)) return false;
    text = ConvertTextToLatex(recognized);
    return !text.empty();
}
