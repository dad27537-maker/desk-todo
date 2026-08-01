#pragma once

#include "MonitorService.h"

#include <Windows.h>
#include <string>
#include <utility>
#include <vector>

struct DesktopIconAnchor {
    std::wstring path;
    MonitorService::MonitorRelativePoint placement;
};

// Maintains a last-known-good, monitor-relative position for ordinary
// filesystem icons on the Windows desktop. A disconnected monitor's anchors
// remain untouched until that same stable monitor identity returns.
class DesktopIconLayoutService {
public:
    bool Load();
    bool CaptureStable(
        const MonitorService::DisplayTopologySnapshot& topology,
        const std::vector<std::wstring>& excludedPaths = {});
    int RestoreOnline(
        const MonitorService::DisplayTopologySnapshot& topology,
        const std::vector<std::wstring>& excludedPaths = {}) const;

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
    bool Save() const;
    std::vector<DesktopIconAnchor> anchors_;
};
