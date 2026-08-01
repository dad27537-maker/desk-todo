#include "JsonUtils.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <limits>
#include <sstream>
#include <utility>

static std::wstring Indent(int depth) {
    return std::wstring((size_t)depth * 2, L' ');
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (auto c : s) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b"; break;
            case L'\f': out += L"\\f"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    wchar_t escaped[7]{};
                    swprintf_s(escaped, L"\\u%04X",
                               static_cast<unsigned int>(c));
                    out += escaped;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static std::wstring ColorToHex(COLORREF c) {
    wchar_t buf[8];
    swprintf_s(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

static COLORREF HexToColor(const std::wstring& s) {
    if (s.empty() || s[0] != L'#') return 0x00202020;
    unsigned int r = 0, g = 0, b = 0;
    swscanf_s(s.c_str() + 1, L"%02x%02x%02x", &r, &g, &b);
    return RGB((BYTE)r, (BYTE)g, (BYTE)b);
}

namespace {

class JsonReader {
public:
    explicit JsonReader(const std::wstring& text, size_t position = 0)
        : text_(text), position_(position) {}

    bool Consume(wchar_t expected) {
        SkipWhitespace();
        if (position_ >= text_.size() || text_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool ParseString(std::wstring& value) {
        value.clear();
        if (!Consume(L'"')) return false;

        while (position_ < text_.size()) {
            const wchar_t character = text_[position_++];
            if (character == L'"') return true;
            if (character < 0x20) return false;
            if (character != L'\\') {
                value.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) return false;

            const wchar_t escaped = text_[position_++];
            switch (escaped) {
            case L'"': value.push_back(L'"'); break;
            case L'\\': value.push_back(L'\\'); break;
            case L'/': value.push_back(L'/'); break;
            case L'b': value.push_back(L'\b'); break;
            case L'f': value.push_back(L'\f'); break;
            case L'n': value.push_back(L'\n'); break;
            case L'r': value.push_back(L'\r'); break;
            case L't': value.push_back(L'\t'); break;
            case L'u': {
                unsigned int codeUnit = 0;
                if (!ParseHexCodeUnit(codeUnit)) return false;
                if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF) {
                    if (position_ + 2 > text_.size() ||
                        text_[position_] != L'\\' ||
                        text_[position_ + 1] != L'u') {
                        return false;
                    }
                    position_ += 2;
                    unsigned int lowSurrogate = 0;
                    if (!ParseHexCodeUnit(lowSurrogate) ||
                        lowSurrogate < 0xDC00 || lowSurrogate > 0xDFFF) {
                        return false;
                    }
                    value.push_back(static_cast<wchar_t>(codeUnit));
                    value.push_back(static_cast<wchar_t>(lowSurrogate));
                } else {
                    if (codeUnit >= 0xDC00 && codeUnit <= 0xDFFF) return false;
                    value.push_back(static_cast<wchar_t>(codeUnit));
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseNumber(double& value) {
        SkipWhitespace();
        const size_t start = position_;
        if (position_ < text_.size() && text_[position_] == L'-') ++position_;
        if (position_ >= text_.size()) return false;

        if (text_[position_] == L'0') {
            ++position_;
        } else if (text_[position_] >= L'1' && text_[position_] <= L'9') {
            do {
                ++position_;
            } while (position_ < text_.size() &&
                     text_[position_] >= L'0' && text_[position_] <= L'9');
        } else {
            return false;
        }

        if (position_ < text_.size() && text_[position_] == L'.') {
            ++position_;
            const size_t fractionStart = position_;
            while (position_ < text_.size() &&
                   text_[position_] >= L'0' && text_[position_] <= L'9') {
                ++position_;
            }
            if (position_ == fractionStart) return false;
        }

        if (position_ < text_.size() &&
            (text_[position_] == L'e' || text_[position_] == L'E')) {
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == L'+' || text_[position_] == L'-')) {
                ++position_;
            }
            const size_t exponentStart = position_;
            while (position_ < text_.size() &&
                   text_[position_] >= L'0' && text_[position_] <= L'9') {
                ++position_;
            }
            if (position_ == exponentStart) return false;
        }

        try {
            size_t consumed = 0;
            value = std::stod(text_.substr(start, position_ - start), &consumed);
            return consumed == position_ - start && std::isfinite(value);
        } catch (...) {
            return false;
        }
    }

    bool ParseBoolean(bool& value) {
        SkipWhitespace();
        if (MatchLiteral(L"true")) {
            value = true;
            return true;
        }
        if (MatchLiteral(L"false")) {
            value = false;
            return true;
        }
        return false;
    }

    bool SkipValue() {
        SkipWhitespace();
        if (position_ >= text_.size()) return false;
        if (text_[position_] == L'"') {
            std::wstring ignored;
            return ParseString(ignored);
        }
        if (text_[position_] == L'{') return SkipObject();
        if (text_[position_] == L'[') return SkipArray();
        if (MatchLiteral(L"true") || MatchLiteral(L"false") ||
            MatchLiteral(L"null")) {
            return true;
        }
        double ignored = 0;
        return ParseNumber(ignored);
    }

    bool AtEnd() {
        SkipWhitespace();
        return position_ == text_.size();
    }

private:
    void SkipWhitespace() {
        while (position_ < text_.size() &&
               (text_[position_] == L' ' || text_[position_] == L'\t' ||
                text_[position_] == L'\n' || text_[position_] == L'\r')) {
            ++position_;
        }
    }

    bool MatchLiteral(const wchar_t* literal) {
        const size_t length = wcslen(literal);
        if (position_ + length > text_.size() ||
            text_.compare(position_, length, literal) != 0) {
            return false;
        }
        position_ += length;
        return true;
    }

    bool ParseHexCodeUnit(unsigned int& value) {
        value = 0;
        if (position_ + 4 > text_.size()) return false;
        for (int index = 0; index < 4; ++index) {
            const wchar_t digit = text_[position_++];
            value <<= 4;
            if (digit >= L'0' && digit <= L'9') {
                value |= static_cast<unsigned int>(digit - L'0');
            } else if (digit >= L'a' && digit <= L'f') {
                value |= static_cast<unsigned int>(digit - L'a' + 10);
            } else if (digit >= L'A' && digit <= L'F') {
                value |= static_cast<unsigned int>(digit - L'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool SkipObject() {
        if (!Consume(L'{')) return false;
        if (Consume(L'}')) return true;
        while (true) {
            std::wstring key;
            if (!ParseString(key) || !Consume(L':') || !SkipValue()) {
                return false;
            }
            if (Consume(L'}')) return true;
            if (!Consume(L',')) return false;
        }
    }

    bool SkipArray() {
        if (!Consume(L'[')) return false;
        if (Consume(L']')) return true;
        while (true) {
            if (!SkipValue()) return false;
            if (Consume(L']')) return true;
            if (!Consume(L',')) return false;
        }
    }

    const std::wstring& text_;
    size_t position_ = 0;
};

bool ParseInteger(JsonReader& reader, int& value) {
    double number = 0;
    if (!reader.ParseNumber(number) || std::trunc(number) != number ||
        number < static_cast<double>((std::numeric_limits<int>::min)()) ||
        number > static_cast<double>((std::numeric_limits<int>::max)())) {
        return false;
    }
    value = static_cast<int>(number);
    return true;
}

bool ParseStringArray(JsonReader& reader, std::vector<std::wstring>& values) {
    values.clear();
    if (!reader.Consume(L'[')) return false;
    if (reader.Consume(L']')) return true;
    while (true) {
        std::wstring value;
        if (!reader.ParseString(value)) return false;
        values.push_back(std::move(value));
        if (reader.Consume(L']')) return true;
        if (!reader.Consume(L',')) return false;
    }
}

bool ParseIntegerArray(JsonReader& reader, std::vector<int>& values) {
    values.clear();
    if (!reader.Consume(L'[')) return false;
    if (reader.Consume(L']')) return true;
    while (true) {
        int value = 0;
        if (!ParseInteger(reader, value)) return false;
        values.push_back(value);
        if (reader.Consume(L']')) return true;
        if (!reader.Consume(L',')) return false;
    }
}

bool ParseBooleanArray(JsonReader& reader, std::vector<bool>& values) {
    values.clear();
    if (!reader.Consume(L'[')) return false;
    if (reader.Consume(L']')) return true;
    while (true) {
        bool value = false;
        if (!reader.ParseBoolean(value)) return false;
        values.push_back(value);
        if (reader.Consume(L']')) return true;
        if (!reader.Consume(L',')) return false;
    }
}

} // namespace

std::wstring JsonSerialize(const std::vector<Fence>& fences) {
    std::wostringstream os;
    os << L"{\n";
    os << L"  \"version\": 9,\n";
    os << L"  \"coordinateSpace\": \"monitorRelative96Dpi\",\n";
    os << L"  \"fences\": [\n";
    for (size_t i = 0; i < fences.size(); ++i) {
        auto& f = fences[i];
        os << L"    {\n";
        os << L"      \"id\": \"" << JsonEscape(f.id) << L"\",\n";
        os << L"      \"title\": \"" << JsonEscape(f.title) << L"\",\n";
        os << L"      \"x\": " << f.rect.left << L",\n";
        os << L"      \"y\": " << f.rect.top << L",\n";
        os << L"      \"width\": " << (f.rect.right - f.rect.left) << L",\n";
        os << L"      \"height\": " << (f.rect.bottom - f.rect.top) << L",\n";
        os << L"      \"color\": \"" << ColorToHex(f.color) << L"\",\n";
        os << L"      \"opacity\": " << f.opacity << L",\n";
        os << L"      \"locked\": " << (f.locked ? L"true" : L"false") << L",\n";
        os << L"      \"collapsed\": " << (f.collapsed ? L"true" : L"false") << L",\n";
        os << L"      \"sortByExtension\": " << (f.sortByExtension ? L"true" : L"false") << L",\n";
        os << L"      \"expandedHeight\": " << f.expandedHeight << L",\n";
        os << L"      \"zOrder\": " << f.zOrder << L",\n";
        os << L"      \"folderPath\": \"" << JsonEscape(f.folderPath) << L"\",\n";
        os << L"      \"isMappedFolder\": " << (f.isMappedFolder ? L"true" : L"false") << L",\n";
        const wchar_t* kind = f.kind == FenceKind::Game
            ? L"game"
            : (f.kind == FenceKind::Clock ? L"clock" : L"standard");
        os << L"      \"kind\": \"" << kind << L"\",\n";
        os << L"      \"monitorId\": \"" << JsonEscape(f.monitorPlacement.monitorId) << L"\",\n";
        os << L"      \"monitorX96\": " << f.monitorPlacement.x96 << L",\n";
        os << L"      \"monitorY96\": " << f.monitorPlacement.y96 << L",\n";
        os << L"      \"monitorWidth96\": " << f.monitorPlacement.width96 << L",\n";
        os << L"      \"monitorHeight96\": " << f.monitorPlacement.height96 << L",\n";
        os << L"      \"monitorExpandedHeight96\": " << f.monitorPlacement.expandedHeight96 << L",\n";
        os << L"      \"monitorPlacementValid\": "
           << (f.monitorPlacement.valid ? L"true" : L"false") << L",\n";
        os << L"      \"viewMode\": \"" << (f.viewMode == FenceViewMode::List ? L"list" : L"icon") << L"\",\n";
        os << L"      \"itemOrder\": [";
        for (size_t itemIndex = 0; itemIndex < f.itemOrder.size(); ++itemIndex) {
            if (itemIndex > 0) os << L", ";
            os << L"\"" << JsonEscape(f.itemOrder[itemIndex]) << L"\"";
        }
        os << L"],\n";
        os << L"      \"memberPaths\": [";
        for (size_t itemIndex = 0; itemIndex < f.memberPaths.size(); ++itemIndex) {
            if (itemIndex > 0) os << L", ";
            os << L"\"" << JsonEscape(f.memberPaths[itemIndex]) << L"\"";
        }
        os << L"],\n";
        os << L"      \"itemDesktopOriginX\": [";
        for (size_t itemIndex = 0; itemIndex < f.itemDesktopOrigins.size(); ++itemIndex) {
            if (itemIndex > 0) os << L", ";
            os << f.itemDesktopOrigins[itemIndex].x;
        }
        os << L"],\n";
        os << L"      \"itemDesktopOriginY\": [";
        for (size_t itemIndex = 0; itemIndex < f.itemDesktopOrigins.size(); ++itemIndex) {
            if (itemIndex > 0) os << L", ";
            os << f.itemDesktopOrigins[itemIndex].y;
        }
        os << L"],\n";
        os << L"      \"itemDesktopOriginMonitorIds\": [";
        for (size_t itemIndex = 0; itemIndex < f.itemOrder.size(); ++itemIndex) {
            if (itemIndex > 0) os << L", ";
            const std::wstring monitorId =
                itemIndex < f.itemDesktopOriginPlacements.size()
                    ? f.itemDesktopOriginPlacements[itemIndex].monitorId
                    : std::wstring{};
            os << L"\"" << JsonEscape(monitorId) << L"\"";
        }
        os << L"],\n";
        os << L"      \"itemDesktopOriginX96\": [";
        for (size_t itemIndex = 0; itemIndex < f.itemOrder.size(); ++itemIndex) {
            if (itemIndex > 0) os << L", ";
            os << (itemIndex < f.itemDesktopOriginPlacements.size()
                       ? f.itemDesktopOriginPlacements[itemIndex].x96
                       : 0);
        }
        os << L"],\n";
        os << L"      \"itemDesktopOriginY96\": [";
        for (size_t itemIndex = 0; itemIndex < f.itemOrder.size(); ++itemIndex) {
            if (itemIndex > 0) os << L", ";
            os << (itemIndex < f.itemDesktopOriginPlacements.size()
                       ? f.itemDesktopOriginPlacements[itemIndex].y96
                       : 0);
        }
        os << L"],\n";
        os << L"      \"itemDesktopOriginPlacementValid\": [";
        for (size_t itemIndex = 0; itemIndex < f.itemOrder.size(); ++itemIndex) {
            if (itemIndex > 0) os << L", ";
            os << (itemIndex < f.itemDesktopOriginPlacements.size() &&
                           f.itemDesktopOriginPlacements[itemIndex].valid
                       ? L"true"
                       : L"false");
        }
        os << L"]\n";
        os << L"    }";
        if (i + 1 < fences.size()) os << L",";
        os << L"\n";
    }
    os << L"  ]\n";
    os << L"}\n";
    return os.str();
}

namespace {

bool ParseFence(JsonReader& reader, Fence& fence) {
    if (!reader.Consume(L'{')) return false;
    if (reader.Consume(L'}')) return true;

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    while (true) {
        std::wstring key;
        if (!reader.ParseString(key) || !reader.Consume(L':')) return false;

        if (key == L"id") {
            if (!reader.ParseString(fence.id)) return false;
        } else if (key == L"title") {
            if (!reader.ParseString(fence.title)) return false;
        } else if (key == L"x") {
            if (!ParseInteger(reader, x)) return false;
        } else if (key == L"y") {
            if (!ParseInteger(reader, y)) return false;
        } else if (key == L"width") {
            if (!ParseInteger(reader, width)) return false;
        } else if (key == L"height") {
            if (!ParseInteger(reader, height)) return false;
        } else if (key == L"color") {
            std::wstring color;
            if (!reader.ParseString(color)) return false;
            fence.color = HexToColor(color);
        } else if (key == L"opacity") {
            double opacity = 0;
            if (!reader.ParseNumber(opacity) ||
                opacity < -static_cast<double>((std::numeric_limits<float>::max)()) ||
                opacity > static_cast<double>((std::numeric_limits<float>::max)())) {
                return false;
            }
            fence.opacity = static_cast<float>(opacity);
        } else if (key == L"locked") {
            if (!reader.ParseBoolean(fence.locked)) return false;
        } else if (key == L"collapsed") {
            if (!reader.ParseBoolean(fence.collapsed)) return false;
        } else if (key == L"sortByExtension") {
            if (!reader.ParseBoolean(fence.sortByExtension)) return false;
        } else if (key == L"expandedHeight") {
            if (!ParseInteger(reader, fence.expandedHeight)) return false;
        } else if (key == L"zOrder") {
            if (!ParseInteger(reader, fence.zOrder)) return false;
        } else if (key == L"folderPath") {
            if (!reader.ParseString(fence.folderPath)) return false;
        } else if (key == L"isMappedFolder") {
            if (!reader.ParseBoolean(fence.isMappedFolder)) return false;
        } else if (key == L"kind") {
            std::wstring kind;
            if (!reader.ParseString(kind)) return false;
            fence.kind = kind == L"game"
                ? FenceKind::Game
                : (kind == L"clock" ? FenceKind::Clock : FenceKind::Standard);
        } else if (key == L"monitorId") {
            if (!reader.ParseString(fence.monitorPlacement.monitorId)) return false;
        } else if (key == L"monitorX96") {
            if (!ParseInteger(reader, fence.monitorPlacement.x96)) return false;
        } else if (key == L"monitorY96") {
            if (!ParseInteger(reader, fence.monitorPlacement.y96)) return false;
        } else if (key == L"monitorWidth96") {
            if (!ParseInteger(reader, fence.monitorPlacement.width96)) return false;
        } else if (key == L"monitorHeight96") {
            if (!ParseInteger(reader, fence.monitorPlacement.height96)) return false;
        } else if (key == L"monitorExpandedHeight96") {
            if (!ParseInteger(reader, fence.monitorPlacement.expandedHeight96)) {
                return false;
            }
        } else if (key == L"monitorPlacementValid") {
            if (!reader.ParseBoolean(fence.monitorPlacement.valid)) return false;
        } else if (key == L"viewMode") {
            std::wstring viewMode;
            if (!reader.ParseString(viewMode)) return false;
            fence.viewMode =
                viewMode == L"list" ? FenceViewMode::List : FenceViewMode::Icon;
        } else if (key == L"itemOrder") {
            if (!ParseStringArray(reader, fence.itemOrder)) return false;
        } else if (key == L"memberPaths") {
            if (!ParseStringArray(reader, fence.memberPaths)) return false;
        } else if (key == L"itemDesktopOriginX") {
            std::vector<int> values;
            if (!ParseIntegerArray(reader, values)) return false;
            if (fence.itemDesktopOrigins.size() < values.size()) {
                fence.itemDesktopOrigins.resize(
                    values.size(), POINT{ LONG_MIN, LONG_MIN });
            }
            for (size_t index = 0; index < values.size(); ++index) {
                fence.itemDesktopOrigins[index].x = values[index];
            }
        } else if (key == L"itemDesktopOriginY") {
            std::vector<int> values;
            if (!ParseIntegerArray(reader, values)) return false;
            if (fence.itemDesktopOrigins.size() < values.size()) {
                fence.itemDesktopOrigins.resize(
                    values.size(), POINT{ LONG_MIN, LONG_MIN });
            }
            for (size_t index = 0; index < values.size(); ++index) {
                fence.itemDesktopOrigins[index].y = values[index];
            }
        } else if (key == L"itemDesktopOriginMonitorIds") {
            std::vector<std::wstring> values;
            if (!ParseStringArray(reader, values)) return false;
            if (fence.itemDesktopOriginPlacements.size() < values.size()) {
                fence.itemDesktopOriginPlacements.resize(values.size());
            }
            for (size_t index = 0; index < values.size(); ++index) {
                fence.itemDesktopOriginPlacements[index].monitorId =
                    std::move(values[index]);
            }
        } else if (key == L"itemDesktopOriginX96") {
            std::vector<int> values;
            if (!ParseIntegerArray(reader, values)) return false;
            if (fence.itemDesktopOriginPlacements.size() < values.size()) {
                fence.itemDesktopOriginPlacements.resize(values.size());
            }
            for (size_t index = 0; index < values.size(); ++index) {
                fence.itemDesktopOriginPlacements[index].x96 = values[index];
            }
        } else if (key == L"itemDesktopOriginY96") {
            std::vector<int> values;
            if (!ParseIntegerArray(reader, values)) return false;
            if (fence.itemDesktopOriginPlacements.size() < values.size()) {
                fence.itemDesktopOriginPlacements.resize(values.size());
            }
            for (size_t index = 0; index < values.size(); ++index) {
                fence.itemDesktopOriginPlacements[index].y96 = values[index];
            }
        } else if (key == L"itemDesktopOriginPlacementValid") {
            std::vector<bool> values;
            if (!ParseBooleanArray(reader, values)) return false;
            if (fence.itemDesktopOriginPlacements.size() < values.size()) {
                fence.itemDesktopOriginPlacements.resize(values.size());
            }
            for (size_t index = 0; index < values.size(); ++index) {
                fence.itemDesktopOriginPlacements[index].valid = values[index];
            }
        } else if (key == L"itemOffsetX") {
            std::vector<int> values;
            if (!ParseIntegerArray(reader, values)) return false;
            if (fence.itemOffsets.size() < values.size()) {
                fence.itemOffsets.resize(values.size());
            }
            for (size_t index = 0; index < values.size(); ++index) {
                fence.itemOffsets[index].x = values[index];
            }
        } else if (key == L"itemOffsetY") {
            std::vector<int> values;
            if (!ParseIntegerArray(reader, values)) return false;
            if (fence.itemOffsets.size() < values.size()) {
                fence.itemOffsets.resize(values.size());
            }
            for (size_t index = 0; index < values.size(); ++index) {
                fence.itemOffsets[index].y = values[index];
            }
        } else if (!reader.SkipValue()) {
            return false;
        }

        if (reader.Consume(L'}')) break;
        if (!reader.Consume(L',')) return false;
    }

    const long long right = static_cast<long long>(x) + width;
    const long long bottom = static_cast<long long>(y) + height;
    if (right < (std::numeric_limits<LONG>::min)() ||
        right > (std::numeric_limits<LONG>::max)() ||
        bottom < (std::numeric_limits<LONG>::min)() ||
        bottom > (std::numeric_limits<LONG>::max)()) {
        return false;
    }
    fence.rect = {
        static_cast<LONG>(x), static_cast<LONG>(y),
        static_cast<LONG>(right), static_cast<LONG>(bottom)
    };
    // All per-item monitor-relative origins are positional records. Keep their
    // length exact so missing fields default safely and surplus values cannot
    // become associated with a later item.
    fence.itemDesktopOriginPlacements.resize(fence.itemOrder.size());
    if (fence.monitorPlacement.valid &&
        fence.monitorPlacement.monitorId.empty()) {
        fence.monitorPlacement.valid = false;
    }
    for (auto& placement : fence.itemDesktopOriginPlacements) {
        if (placement.valid && placement.monitorId.empty()) {
            placement.valid = false;
        }
    }
    return true;
}

bool ParseFenceArray(JsonReader& reader, std::vector<Fence>& fences) {
    fences.clear();
    if (!reader.Consume(L'[')) return false;
    if (reader.Consume(L']')) return true;
    while (true) {
        Fence fence;
        if (!ParseFence(reader, fence)) return false;
        fences.push_back(std::move(fence));
        if (reader.Consume(L']')) return true;
        if (!reader.Consume(L',')) return false;
    }
}

} // namespace

bool JsonDeserialize(const std::wstring& json, std::vector<Fence>& outFences,
                     int* outVersion) {
    outFences.clear();
    if (outVersion) *outVersion = 0;

    const size_t start = !json.empty() && json[0] == 0xFEFF ? 1 : 0;
    JsonReader reader(json, start);
    if (!reader.Consume(L'{')) return false;

    bool foundFences = false;
    int parsedVersion = 0;
    std::vector<Fence> parsedFences;

    if (!reader.Consume(L'}')) {
        while (true) {
            std::wstring key;
            if (!reader.ParseString(key) || !reader.Consume(L':')) return false;

            if (key == L"version") {
                if (!ParseInteger(reader, parsedVersion)) return false;
            } else if (key == L"fences") {
                // Multiple authoritative arrays are ambiguous and could hide a
                // partially overwritten first value.
                if (foundFences || !ParseFenceArray(reader, parsedFences)) {
                    return false;
                }
                foundFences = true;
            } else if (!reader.SkipValue()) {
                return false;
            }

            if (reader.Consume(L'}')) break;
            if (!reader.Consume(L',')) return false;
        }
    }

    if (!foundFences || !reader.AtEnd()) return false;
    outFences = std::move(parsedFences);
    if (outVersion) *outVersion = parsedVersion;
    return true;
}
