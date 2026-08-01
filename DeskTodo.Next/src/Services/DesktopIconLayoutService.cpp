#include "DesktopIconLayoutService.h"

#include "DesktopPathService.h"
#include "DesktopShellService.h"
#include "FileService.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t StateMagic = 0x49445444;  // "DTDI"
constexpr uint32_t StateVersion = 1;
constexpr uint32_t MaximumAnchorCount = 100000;
constexpr uint32_t MaximumPathLength = 32768;
constexpr uint32_t MaximumMonitorIdLength = 4096;

std::filesystem::path StatePath() {
    return std::filesystem::path(GetAppDataPath()) /
        L"desktop-icon-layout.dat";
}

std::wstring PathKey(const std::wstring& path) {
    std::wstring key = NormalizePathIdentity(path);
    std::transform(key.begin(), key.end(), key.begin(),
        [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
    return key;
}

std::unordered_set<std::wstring> BuildPathSet(
    const std::vector<std::wstring>& paths) {
    std::unordered_set<std::wstring> result;
    result.reserve(paths.size());
    for (const auto& path : paths) {
        if (!path.empty()) result.insert(PathKey(path));
    }
    return result;
}

template <typename T>
bool ReadValue(std::ifstream& stream, T& value) {
    return static_cast<bool>(stream.read(
        reinterpret_cast<char*>(&value), sizeof(value)));
}

bool ReadWideString(std::ifstream& stream, uint32_t length,
                    uint32_t maximumLength, std::wstring& value) {
    if (length > maximumLength) return false;
    value.assign(length, L'\0');
    return length == 0 || static_cast<bool>(stream.read(
        reinterpret_cast<char*>(value.data()),
        static_cast<std::streamsize>(length) * sizeof(wchar_t)));
}

template <typename T>
void AppendBytes(std::vector<uint8_t>& bytes, const T& value) {
    const size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void AppendWideString(std::vector<uint8_t>& bytes,
                      const std::wstring& value) {
    if (value.empty()) return;
    const size_t byteCount = value.size() * sizeof(wchar_t);
    const size_t offset = bytes.size();
    bytes.resize(offset + byteCount);
    std::memcpy(bytes.data() + offset, value.data(), byteCount);
}

bool WriteDurableFile(const std::filesystem::path& target,
                      const std::vector<uint8_t>& bytes) {
    const std::wstring temporary =
        target.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool success = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const size_t remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(remaining, static_cast<size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk,
                       &written, nullptr) ||
            written == 0) {
            success = false;
            break;
        }
        offset += written;
    }
    if (success) success = FlushFileBuffers(file) == TRUE;
    if (!CloseHandle(file)) success = false;
    if (success) {
        success = MoveFileExW(
            temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == TRUE;
    }
    if (!success) DeleteFileW(temporary.c_str());
    return success;
}

bool SamePlacement(const MonitorService::MonitorRelativePoint& left,
                   const MonitorService::MonitorRelativePoint& right) {
    return left.valid == right.valid &&
        _wcsicmp(left.monitorId.c_str(), right.monitorId.c_str()) == 0 &&
        left.x96 == right.x96 && left.y96 == right.y96;
}

}

bool DesktopIconLayoutService::Load() {
    anchors_.clear();
    const auto statePath = StatePath();
    std::error_code existenceError;
    if (!std::filesystem::exists(statePath, existenceError)) {
        return !existenceError;
    }
    std::ifstream stream(statePath, std::ios::binary);
    if (!stream.is_open()) return false;

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t count = 0;
    if (!ReadValue(stream, magic) ||
        !ReadValue(stream, version) ||
        !ReadValue(stream, count) ||
        magic != StateMagic || version != StateVersion ||
        count > MaximumAnchorCount) {
        anchors_.clear();
        return false;
    }

    std::unordered_set<std::wstring> uniquePaths;
    anchors_.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t pathLength = 0;
        uint32_t monitorLength = 0;
        DesktopIconAnchor anchor;
        if (!ReadValue(stream, pathLength) ||
            !ReadValue(stream, monitorLength) ||
            !ReadValue(stream, anchor.placement.x96) ||
            !ReadValue(stream, anchor.placement.y96) ||
            !ReadWideString(stream, pathLength, MaximumPathLength,
                            anchor.path) ||
            !ReadWideString(stream, monitorLength,
                            MaximumMonitorIdLength,
                            anchor.placement.monitorId)) {
            anchors_.clear();
            return false;
        }
        anchor.placement.valid =
            !anchor.path.empty() && !anchor.placement.monitorId.empty();
        const std::wstring key = PathKey(anchor.path);
        if (!anchor.placement.valid || !uniquePaths.insert(key).second) {
            anchors_.clear();
            return false;
        }
        anchors_.push_back(std::move(anchor));
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        anchors_.clear();
        return false;
    }
    return true;
}

bool DesktopIconLayoutService::Save() const {
    if (!EnsureDirectoryExists(GetAppDataPath())) return false;

    std::vector<DesktopIconAnchor> sorted = anchors_;
    std::sort(sorted.begin(), sorted.end(),
        [](const DesktopIconAnchor& left,
           const DesktopIconAnchor& right) {
            return _wcsicmp(left.path.c_str(), right.path.c_str()) < 0;
        });

    std::vector<uint8_t> bytes;
    bytes.reserve(16 + sorted.size() * 128);
    AppendBytes(bytes, StateMagic);
    AppendBytes(bytes, StateVersion);
    const uint32_t count = static_cast<uint32_t>(sorted.size());
    AppendBytes(bytes, count);
    for (const auto& anchor : sorted) {
        if (anchor.path.size() > MaximumPathLength ||
            anchor.placement.monitorId.size() >
                MaximumMonitorIdLength) {
            return false;
        }
        const uint32_t pathLength =
            static_cast<uint32_t>(anchor.path.size());
        const uint32_t monitorLength =
            static_cast<uint32_t>(anchor.placement.monitorId.size());
        AppendBytes(bytes, pathLength);
        AppendBytes(bytes, monitorLength);
        AppendBytes(bytes, anchor.placement.x96);
        AppendBytes(bytes, anchor.placement.y96);
        AppendWideString(bytes, anchor.path);
        AppendWideString(bytes, anchor.placement.monitorId);
    }
    return WriteDurableFile(StatePath(), bytes);
}

bool DesktopIconLayoutService::CaptureStable(
    const MonitorService::DisplayTopologySnapshot& topology,
    const std::vector<std::wstring>& excludedPaths) {
    std::vector<DesktopItem> userItems;
    std::vector<DesktopItem> publicItems;
    const std::wstring userDesktop = GetUserDesktopPath();
    const std::wstring publicDesktop = GetPublicDesktopPath();
    const bool userScanned = !userDesktop.empty() &&
        FileService::TryScanFolder(userDesktop, userItems);
    const bool publicScanned = !publicDesktop.empty() &&
        FileService::TryScanFolder(publicDesktop, publicItems);
    if (!userScanned && !publicScanned) return false;

    const auto excluded = BuildPathSet(excludedPaths);
    size_t eligibleCount = 0;
    std::vector<std::pair<std::wstring, POINT>> positions;
    auto capture = [&](const std::vector<DesktopItem>& items) {
        for (const auto& item : items) {
            if (excluded.find(PathKey(item.fullPath)) != excluded.end()) {
                continue;
            }
            ++eligibleCount;
            POINT position{};
            if (DesktopShellService::GetPathPositionOnDesktop(
                    item.fullPath, position)) {
                positions.emplace_back(item.fullPath, position);
            }
        }
    };
    if (userScanned) capture(userItems);
    if (publicScanned) capture(publicItems);

    // If Explorer discovery failed, keep the last good snapshot instead of
    // treating every file as newly unpositioned.
    if (eligibleCount > 0 && positions.empty()) return false;
    return UpdateFromPositions(topology, positions, excludedPaths);
}

bool DesktopIconLayoutService::UpdateFromPositions(
    const MonitorService::DisplayTopologySnapshot& topology,
    const std::vector<std::pair<std::wstring, POINT>>& positions,
    const std::vector<std::wstring>& excludedPaths) {
    const auto previousAnchors = anchors_;
    const auto excluded = BuildPathSet(excludedPaths);
    bool changed = false;

    for (const auto& position : positions) {
        if (position.first.empty() ||
            excluded.find(PathKey(position.first)) != excluded.end()) {
            continue;
        }
        const auto* monitor = MonitorService::FindMonitorForPoint(
            topology, position.second);
        if (!monitor) continue;
        const auto placement = MonitorService::CapturePoint96(
            position.second, *monitor);
        if (!placement.valid) continue;

        const std::wstring key = PathKey(position.first);
        auto existing = std::find_if(
            anchors_.begin(), anchors_.end(),
            [&](const DesktopIconAnchor& anchor) {
                return PathKey(anchor.path) == key;
            });
        if (existing != anchors_.end() &&
            existing->placement.valid &&
            !MonitorService::FindMonitorById(
                topology, existing->placement.monitorId)) {
            // Explorer temporarily projects icons from a disconnected display
            // onto an online display. Keep the offline monitor anchor intact;
            // otherwise the temporary projection becomes the new permanent
            // position and the icon cannot return when that monitor reconnects.
            continue;
        }
        if (existing == anchors_.end()) {
            anchors_.push_back({ NormalizePathIdentity(position.first),
                                 placement });
            changed = true;
        } else if (!SamePlacement(existing->placement, placement) ||
                   existing->path != NormalizePathIdentity(position.first)) {
            existing->path = NormalizePathIdentity(position.first);
            existing->placement = placement;
            changed = true;
        }
    }

    const auto oldSize = anchors_.size();
    anchors_.erase(
        std::remove_if(
            anchors_.begin(), anchors_.end(),
            [&](const DesktopIconAnchor& anchor) {
                if (excluded.find(PathKey(anchor.path)) != excluded.end()) {
                    return false;
                }
                return GetFileAttributesW(anchor.path.c_str()) ==
                    INVALID_FILE_ATTRIBUTES;
            }),
        anchors_.end());
    changed = changed || anchors_.size() != oldSize;
    if (!changed) return true;
    if (Save()) return true;
    anchors_ = previousAnchors;
    return false;
}

bool DesktopIconLayoutService::ResolveOnlinePoint(
    const DesktopIconAnchor& anchor,
    const MonitorService::DisplayTopologySnapshot& topology,
    POINT& physicalPoint) {
    physicalPoint = {};
    if (!anchor.placement.valid) return false;
    const auto* monitor = MonitorService::FindMonitorById(
        topology, anchor.placement.monitorId);
    if (!monitor) return false;
    physicalPoint = MonitorService::MaterializePoint96(
        anchor.placement, *monitor);
    return true;
}

int DesktopIconLayoutService::RestoreOnline(
    const MonitorService::DisplayTopologySnapshot& topology,
    const std::vector<std::wstring>& excludedPaths) const {
    const auto excluded = BuildPathSet(excludedPaths);
    int restoredCount = 0;
    for (const auto& anchor : anchors_) {
        if (excluded.find(PathKey(anchor.path)) != excluded.end() ||
            GetFileAttributesW(anchor.path.c_str()) ==
                INVALID_FILE_ATTRIBUTES ||
            !IsDesktopFilesystemPath(anchor.path)) {
            continue;
        }
        POINT target{};
        if (!ResolveOnlinePoint(anchor, topology, target)) continue;

        POINT current{};
        if (DesktopShellService::GetPathPositionOnDesktop(
                anchor.path, current) &&
            std::abs(current.x - target.x) <= 1 &&
            std::abs(current.y - target.y) <= 1) {
            continue;
        }
        if (DesktopShellService::RestorePathToDesktopPosition(
                anchor.path, target)) {
            ++restoredCount;
        }
    }
    return restoredCount;
}
