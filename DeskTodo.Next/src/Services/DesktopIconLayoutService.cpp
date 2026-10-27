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
constexpr uint32_t CurrentStateVersion = 2;
constexpr uint32_t MinimumStateVersion = 1;
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

bool SameRect(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
        left.right == right.right && left.bottom == right.bottom;
}

void CaptureMonitorSnapshot(
    DesktopIconAnchor& anchor,
    const MonitorService::MonitorSnapshot& monitor) {
    anchor.monitorRect = monitor.monitorRect;
    anchor.workArea = monitor.workArea;
    anchor.dpiX = monitor.dpiX ? monitor.dpiX : 96;
    anchor.dpiY = monitor.dpiY ? monitor.dpiY : 96;
    anchor.primary = monitor.primary;
    anchor.hasMonitorSnapshot = true;
}

bool SameMonitorSnapshot(
    const DesktopIconAnchor& anchor,
    const MonitorService::MonitorSnapshot& monitor) {
    return anchor.hasMonitorSnapshot &&
        SameRect(anchor.monitorRect, monitor.monitorRect) &&
        SameRect(anchor.workArea, monitor.workArea) &&
        anchor.dpiX == (monitor.dpiX ? monitor.dpiX : 96) &&
        anchor.dpiY == (monitor.dpiY ? monitor.dpiY : 96) &&
        anchor.primary == monitor.primary;
}

const MonitorService::MonitorSnapshot* FindAnchorMonitor(
    const DesktopIconAnchor& anchor,
    const MonitorService::DisplayTopologySnapshot& topology) {
    if (const auto* exact = MonitorService::FindMonitorById(
            topology, anchor.placement.monitorId)) {
        return exact;
    }
    if (!anchor.hasMonitorSnapshot || topology.monitors.size() < 2) {
        // A surviving panel can take the removed primary's exact geometry
        // and primary flag. Geometry alone cannot distinguish that case from
        // a changed device identity; never overwrite the offline anchor.
        return nullptr;
    }

    // Prefer an exact geometry/DPI match. It safely recovers the common case
    // where a dock changes the CCD device path but Windows restores the same
    // physical panel to the same desktop slot.
    const MonitorService::MonitorSnapshot* match = nullptr;
    for (const auto& monitor : topology.monitors) {
        if (!SameMonitorSnapshot(anchor, monitor)) continue;
        if (match) return nullptr;  // Ambiguous identical displays.
        match = &monitor;
    }
    if (match) return match;

    // Coordinates may shift when the primary display changes. Recover only a
    // unique panel with the same physical/work-area size, DPI and role.
    const LONG savedMonitorWidth =
        anchor.monitorRect.right - anchor.monitorRect.left;
    const LONG savedMonitorHeight =
        anchor.monitorRect.bottom - anchor.monitorRect.top;
    const LONG savedWorkWidth = anchor.workArea.right - anchor.workArea.left;
    const LONG savedWorkHeight = anchor.workArea.bottom - anchor.workArea.top;
    for (const auto& monitor : topology.monitors) {
        const bool compatible =
            monitor.monitorRect.right - monitor.monitorRect.left ==
                savedMonitorWidth &&
            monitor.monitorRect.bottom - monitor.monitorRect.top ==
                savedMonitorHeight &&
            monitor.workArea.right - monitor.workArea.left == savedWorkWidth &&
            monitor.workArea.bottom - monitor.workArea.top == savedWorkHeight &&
            (monitor.dpiX ? monitor.dpiX : 96) == anchor.dpiX &&
            (monitor.dpiY ? monitor.dpiY : 96) == anchor.dpiY &&
            monitor.primary == anchor.primary;
        if (!compatible) continue;
        if (match) return nullptr;
        match = &monitor;
    }
    return match;
}

}

bool DesktopIconLayoutService::ReadPosition(const std::wstring& path, POINT& point) const {
    return positionReader_ ? positionReader_(path, point)
        : DesktopShellService::GetPathPositionOnDesktop(path, point);
}

bool DesktopIconLayoutService::WritePosition(const std::wstring& path, POINT point) const {
    return positionWriter_ ? positionWriter_(path, point)
        : DesktopShellService::TryRestorePathToDesktopPosition(path, point);
}

bool DesktopIconLayoutService::Load() {
    anchors_.clear();
    pendingRestoreTargets_.clear();
    pendingRestoreCursor_ = 0;
    pendingRestoreRounds_ = 0;
    unrestoredTargets_.clear();
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
        magic != StateMagic ||
        version < MinimumStateVersion ||
        version > CurrentStateVersion ||
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
        if (version >= 2) {
            uint32_t primary = 0;
            uint32_t hasMonitorSnapshot = 0;
            if (!ReadValue(stream, anchor.monitorRect.left) ||
                !ReadValue(stream, anchor.monitorRect.top) ||
                !ReadValue(stream, anchor.monitorRect.right) ||
                !ReadValue(stream, anchor.monitorRect.bottom) ||
                !ReadValue(stream, anchor.workArea.left) ||
                !ReadValue(stream, anchor.workArea.top) ||
                !ReadValue(stream, anchor.workArea.right) ||
                !ReadValue(stream, anchor.workArea.bottom) ||
                !ReadValue(stream, anchor.dpiX) ||
                !ReadValue(stream, anchor.dpiY) ||
                !ReadValue(stream, primary) ||
                !ReadValue(stream, hasMonitorSnapshot)) {
                anchors_.clear();
                return false;
            }
            anchor.primary = primary != 0;
            anchor.hasMonitorSnapshot = hasMonitorSnapshot != 0;
            if (anchor.dpiX == 0) anchor.dpiX = 96;
            if (anchor.dpiY == 0) anchor.dpiY = 96;
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
    AppendBytes(bytes, CurrentStateVersion);
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
        AppendBytes(bytes, anchor.monitorRect.left);
        AppendBytes(bytes, anchor.monitorRect.top);
        AppendBytes(bytes, anchor.monitorRect.right);
        AppendBytes(bytes, anchor.monitorRect.bottom);
        AppendBytes(bytes, anchor.workArea.left);
        AppendBytes(bytes, anchor.workArea.top);
        AppendBytes(bytes, anchor.workArea.right);
        AppendBytes(bytes, anchor.workArea.bottom);
        AppendBytes(bytes, anchor.dpiX);
        AppendBytes(bytes, anchor.dpiY);
        const uint32_t primary = anchor.primary ? 1u : 0u;
        const uint32_t hasMonitorSnapshot =
            anchor.hasMonitorSnapshot ? 1u : 0u;
        AppendBytes(bytes, primary);
        AppendBytes(bytes, hasMonitorSnapshot);
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
            if (ReadPosition(item.fullPath, position)) {
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
        const std::wstring key = PathKey(position.first);
        const auto unrestored = unrestoredTargets_.find(key);
        if (unrestored != unrestoredTargets_.end()) {
            if (std::abs(position.second.x - unrestored->second.x) > 1 ||
                std::abs(position.second.y - unrestored->second.y) > 1) {
                // A failed bounded recovery must not turn Explorer's
                // temporary projection into the next last-known-good anchor.
                continue;
            }
            unrestoredTargets_.erase(unrestored);
        }
        const auto* monitor = MonitorService::FindMonitorForPoint(
            topology, position.second);
        if (!monitor) continue;
        const auto placement = MonitorService::CapturePoint96(
            position.second, *monitor);
        if (!placement.valid) continue;

        auto existing = std::find_if(
            anchors_.begin(), anchors_.end(),
            [&](const DesktopIconAnchor& anchor) {
                return PathKey(anchor.path) == key;
            });
        if (existing != anchors_.end() &&
            existing->placement.valid &&
            !FindAnchorMonitor(*existing, topology)) {
            // Explorer temporarily projects icons from a disconnected display
            // onto an online display. Keep the offline monitor anchor intact;
            // otherwise the temporary projection becomes the new permanent
            // position and the icon cannot return when that monitor reconnects.
            continue;
        }
        if (existing == anchors_.end()) {
            DesktopIconAnchor anchor;
            anchor.path = NormalizePathIdentity(position.first);
            anchor.placement = placement;
            CaptureMonitorSnapshot(anchor, *monitor);
            anchors_.push_back(std::move(anchor));
            changed = true;
        } else {
            const std::wstring normalizedPath =
                NormalizePathIdentity(position.first);
            const bool anchorChanged =
                !SamePlacement(existing->placement, placement) ||
                existing->path != normalizedPath ||
                !SameMonitorSnapshot(*existing, *monitor);
            existing->path = normalizedPath;
            existing->placement = placement;
            CaptureMonitorSnapshot(*existing, *monitor);
            changed = changed || anchorChanged;
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
    const auto* monitor = FindAnchorMonitor(anchor, topology);
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

bool DesktopIconLayoutService::BeginTopologyRecovery(
    const MonitorService::DisplayTopologySnapshot& topology,
    const std::vector<std::wstring>& excludedPaths) {
    pendingRestoreTargets_.clear();
    pendingRestoreCursor_ = 0;
    pendingRestoreRounds_ = 0;
    unrestoredTargets_.clear();

    const auto excluded = BuildPathSet(excludedPaths);
    std::vector<std::wstring> offlinePaths;
    bool anchorIdentityChanged = false;
    const auto previousAnchors = anchors_;

    for (auto& anchor : anchors_) {
        if (!anchor.placement.valid ||
            excluded.find(PathKey(anchor.path)) != excluded.end() ||
            GetFileAttributesW(anchor.path.c_str()) ==
                INVALID_FILE_ATTRIBUTES ||
            !IsDesktopFilesystemPath(anchor.path)) {
            continue;
        }

        const auto* monitor = FindAnchorMonitor(anchor, topology);
        if (!monitor) {
            // Explorer projects this icon onto a remaining monitor when its
            // owning display disappears. Park it instead and preserve the
            // monitor-relative anchor for the reconnect.
            offlinePaths.push_back(anchor.path);
            continue;
        }

        if (_wcsicmp(
                anchor.placement.monitorId.c_str(),
                monitor->stableId.c_str()) != 0 ||
            !SameMonitorSnapshot(anchor, *monitor)) {
            anchor.placement.monitorId = monitor->stableId;
            CaptureMonitorSnapshot(anchor, *monitor);
            anchorIdentityChanged = true;
        }

        PendingRestoreTarget target;
        target.path = anchor.path;
        target.point = MonitorService::MaterializePoint96(
            anchor.placement, *monitor);
        unrestoredTargets_.insert_or_assign(PathKey(target.path), target.point);
        pendingRestoreTargets_.push_back(std::move(target));
    }

    const bool visibilityReady =
        DesktopShellService::SetOfflineMonitorDesktopPaths(offlinePaths);

    bool saved = true;
    if (anchorIdentityChanged) {
        saved = Save();
        if (!saved) anchors_ = previousAnchors;
    }

    // Explorer may replace its desktop ListView more than once during a
    // hot-plug. Retry complete target sets on later timer ticks, never in a
    // blocking loop on the window thread.
    pendingRestoreRounds_ = pendingRestoreTargets_.empty() ? 0 : 4;
    return visibilityReady && saved;
}

bool DesktopIconLayoutService::RecordUserPlacement(
    const MonitorService::DisplayTopologySnapshot& topology,
    const std::wstring& path, POINT physicalPoint) {
    if (topology.signature.empty() || path.empty()) return false;
    const auto* monitor = MonitorService::FindMonitorForPoint(topology, physicalPoint);
    if (!monitor || !IsDesktopFilesystemPath(path) ||
        GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const auto previousAnchors = anchors_;
    const auto key = PathKey(path);
    auto anchor = std::find_if(anchors_.begin(), anchors_.end(),
        [&](const DesktopIconAnchor& existing) { return PathKey(existing.path) == key; });
    if (anchor == anchors_.end()) {
        anchors_.push_back({});
        anchor = anchors_.end() - 1;
    }
    anchor->path = NormalizePathIdentity(path);
    anchor->placement = MonitorService::CapturePoint96(physicalPoint, *monitor);
    CaptureMonitorSnapshot(*anchor, *monitor);
    if (!Save()) {
        anchors_ = previousAnchors;
        return false;
    }
    unrestoredTargets_.erase(key);
    for (auto& target : pendingRestoreTargets_) {
        if (PathKey(target.path) == key) target.completed = true;
    }
    return true;
}

size_t DesktopIconLayoutService::ProcessTopologyRecoveryBatch(
    size_t maximumItems) {
    if (maximumItems == 0 || pendingRestoreTargets_.empty()) return 0;

    size_t processed = 0;
    while (processed < maximumItems &&
           pendingRestoreCursor_ < pendingRestoreTargets_.size()) {
        auto& target = pendingRestoreTargets_[pendingRestoreCursor_++];
        if (target.completed) continue;
        ++processed;
        if (GetFileAttributesW(target.path.c_str()) ==
                INVALID_FILE_ATTRIBUTES ||
            !IsDesktopFilesystemPath(target.path)) {
            continue;
        }

        POINT current{};
        const bool alreadyPlaced =
            ReadPosition(target.path, current) &&
            std::abs(current.x - target.point.x) <= 1 &&
            std::abs(current.y - target.point.y) <= 1;
        const bool restored = alreadyPlaced ||
            WritePosition(target.path, target.point);
        if (restored) {
            target.completed = true;
            unrestoredTargets_.erase(PathKey(target.path));
        } else {
            unrestoredTargets_.insert_or_assign(PathKey(target.path), target.point);
        }
    }

    if (pendingRestoreCursor_ >= pendingRestoreTargets_.size()) {
        --pendingRestoreRounds_;
        const bool unfinished = std::any_of(
            pendingRestoreTargets_.begin(), pendingRestoreTargets_.end(),
            [](const PendingRestoreTarget& target) { return !target.completed; });
        if (pendingRestoreRounds_ > 0 && unfinished) {
            pendingRestoreCursor_ = 0;
        } else {
            pendingRestoreTargets_.clear();
            pendingRestoreCursor_ = 0;
        }
    }
    return pendingRestoreTargets_.size();
}
