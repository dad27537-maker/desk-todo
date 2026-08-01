#pragma once

#include <Windows.h>
#include <string>
#include <vector>

namespace MonitorService {

constexpr int MIN_VISIBLE_WIDTH = 80;
constexpr int MIN_VISIBLE_TITLE_HEIGHT = 32;

// A physical monitor in the current active display topology.
//
// stableId prefers the CCD monitor device path because the GDI alias
// (for example, "\\.\DISPLAY2") can be renumbered when displays are
// connected or disconnected. gdiDeviceName is retained only for matching
// Win32 monitor enumeration with QueryDisplayConfig paths.
struct MonitorSnapshot {
    std::wstring stableId;
    std::wstring gdiDeviceName;
    RECT monitorRect{};
    RECT workArea{};
    UINT dpiX = 96;
    UINT dpiY = 96;
    bool primary = false;
};

// A deterministic snapshot of all active desktop monitors. monitors is
// sorted by stable identity so enumeration order cannot create a false
// topology change.
struct DisplayTopologySnapshot {
    RECT virtualScreen{};
    std::vector<MonitorSnapshot> monitors;
    std::wstring signature;
};

// A monitor-relative rectangle in 96-DPI units. The rectangle is kept
// separate from a fence's runtime pixel rectangle so a temporarily offline
// monitor never forces the persisted placement onto another display.
struct MonitorRelativePlacement {
    std::wstring monitorId;
    int x96 = 0;
    int y96 = 0;
    int width96 = 0;
    int height96 = 0;
    int expandedHeight96 = 0;
    bool valid = false;
};

// A desktop icon position anchored to one physical monitor in 96-DPI units.
// Keeping the monitor identity prevents a disconnected display from being
// silently substituted with the current primary monitor.
struct MonitorRelativePoint {
    std::wstring monitorId;
    int x96 = 0;
    int y96 = 0;
    bool valid = false;
};

RECT GetPrimaryWorkArea();
RECT GetVirtualScreen();
RECT GetNearestMonitorWorkArea(const RECT& rect);
bool IsFenceInAnyWorkingArea(const RECT& fenceRect);
bool CenterFenceInPrimaryWorkArea(RECT& fenceRect);
void ApplyFenceBounds(RECT& rect);
void ApplyFenceBounds(
    RECT& rect, const DisplayTopologySnapshot& topology);

DisplayTopologySnapshot CaptureTopology();
std::wstring BuildTopologySignature(const DisplayTopologySnapshot& topology);
const MonitorSnapshot* FindMonitorById(const DisplayTopologySnapshot& topology,
                                       const std::wstring& monitorId);
const MonitorSnapshot* FindMonitorForPoint(
    const DisplayTopologySnapshot& topology, POINT physicalPoint);

MonitorRelativePlacement CapturePlacement96(
    const RECT& physicalRect,
    int expandedHeightPhysical,
    const MonitorSnapshot& monitor);
RECT MaterializePlacement96(
    const MonitorRelativePlacement& placement,
    const MonitorSnapshot& monitor,
    int* expandedHeightPhysical = nullptr);
MonitorRelativePoint CapturePoint96(
    POINT physicalPoint, const MonitorSnapshot& monitor);
POINT MaterializePoint96(
    const MonitorRelativePoint& placement,
    const MonitorSnapshot& monitor);

}
