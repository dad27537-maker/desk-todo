#pragma once

#include "MonitorService.h"

#include <Windows.h>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct DesktopIconAnchor {
    std::wstring path;
    MonitorService::MonitorRelativePoint placement;
    // Last committed monitor geometry is a secondary identity. Some USB-C
    // docks expose a new CCD device path after reconnect even though the same
    // physical panel, work area and DPI returned.
    RECT monitorRect{};
    RECT workArea{};
    UINT dpiX = 96;
    UINT dpiY = 96;
    bool primary = false;
    bool hasMonitorSnapshot = false;
};

// Maintains a last-known-good, monitor-relative position for ordinary
// filesystem icons on the Windows desktop. A disconnected monitor's anchors
// remain untouched until that same stable monitor identity returns.
class DesktopIconLayoutService {
public:
    using PositionReader = bool (*)(const std::wstring&, POINT&);
    using PositionWriter = bool (*)(const std::wstring&, POINT);
    explicit DesktopIconLayoutService(PositionReader reader = nullptr,
                                      PositionWriter writer = nullptr)
        : positionReader_(reader), positionWriter_(writer) {}
    bool Load();
    bool CaptureStable(
        const MonitorService::DisplayTopologySnapshot& topology,
        const std::vector<std::wstring>& excludedPaths = {});
    int RestoreOnline(
        const MonitorService::DisplayTopologySnapshot& topology,
        const std::vector<std::wstring>& excludedPaths = {}) const;
    // Commits one per-monitor visibility plan. Icons anchored to an offline
    // monitor are parked instead of being allowed to clutter the remaining
    // desktop; online positions are queued for non-blocking restoration.
    bool BeginTopologyRecovery(
        const MonitorService::DisplayTopologySnapshot& topology,
        const std::vector<std::wstring>& excludedPaths = {});
    size_t ProcessTopologyRecoveryBatch(size_t maximumItems = 6);
    // A confirmed native desktop drag/drop supersedes any queued old position.
    bool RecordUserPlacement(
        const MonitorService::DisplayTopologySnapshot& topology,
        const std::wstring& path, POINT physicalPoint);
    bool HasPendingTopologyRecovery() const {
        return !pendingRestoreTargets_.empty();
    }

    // Pure-data entry point used by capture and native regression tests.
    bool UpdateFromPositions(
        const MonitorService::DisplayTopologySnapshot& topology,
        const std::vector<std::pair<std::wstring, POINT>>& positions,
        const std::vector<std::wstring>& excludedPaths = {});
    static bool ResolveOnlinePoint(
        const DesktopIconAnchor& anchor,
        const MonitorService::DisplayTopologySnapshot& topology,
        POINT& physicalPoint);

    const std::vector<DesktopIconAnchor>& Anchors() const { return anchors_; }

private:
    struct PendingRestoreTarget {
        std::wstring path;
        POINT point{};
        bool completed = false;
    };

    bool Save() const;
    bool ReadPosition(const std::wstring& path, POINT& point) const;
    bool WritePosition(const std::wstring& path, POINT point) const;
    PositionReader positionReader_ = nullptr;
    PositionWriter positionWriter_ = nullptr;
    std::vector<DesktopIconAnchor> anchors_;
    std::vector<PendingRestoreTarget> pendingRestoreTargets_;
    size_t pendingRestoreCursor_ = 0;
    int pendingRestoreRounds_ = 0;
    // Targets that Explorer has not confirmed. After bounded retry rounds,
    // keep only these paths out of capture until verified or the next recovery.
    std::unordered_map<std::wstring, POINT> unrestoredTargets_;
};
