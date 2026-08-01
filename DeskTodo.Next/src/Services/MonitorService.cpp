#include "MonitorService.h"

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <iterator>
#include <limits>
#include <shellscalingapi.h>
#include <sstream>
#include <utility>

namespace MonitorService {

RECT GetPrimaryWorkArea() {
    POINT origin{ 0, 0 };
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{ sizeof(info) };
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;

    RECT fallback{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
    return fallback;
}

RECT GetVirtualScreen() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return { x, y, x + cx, y + cy };
}

RECT GetNearestMonitorWorkArea(const RECT& rect) {
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
    return GetPrimaryWorkArea();
}

namespace {

struct CcdMonitorIdentity {
    std::wstring gdiDeviceName;
    std::wstring stableId;
};

struct WorkAreaHitContext {
    const RECT* fenceRect = nullptr;
    bool intersects = false;
};

BOOL CALLBACK CheckWorkAreaIntersection(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* context = reinterpret_cast<WorkAreaHitContext*>(data);
    if (!context || !context->fenceRect) return FALSE;

    MONITORINFO info{ sizeof(info) };
    RECT intersection{};
    if (GetMonitorInfoW(monitor, &info) &&
        IntersectRect(&intersection, context->fenceRect, &info.rcWork) &&
        !IsRectEmpty(&intersection)) {
        context->intersects = true;
        return FALSE;
    }
    return TRUE;
}

bool EqualInsensitive(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::wstring BuildCcdFallbackId(const LUID& adapterId,
                                UINT32 targetId,
                                const std::wstring& gdiDeviceName) {
    std::wostringstream stream;
    stream << L"ccd:"
           << std::hex << std::setfill(L'0')
           << std::setw(8) << static_cast<unsigned long>(adapterId.HighPart)
           << std::setw(8) << static_cast<unsigned long>(adapterId.LowPart)
           << L":" << std::dec << targetId
           << L":" << Lowercase(gdiDeviceName);
    return stream.str();
}

std::wstring BuildGdiFallbackId(const std::wstring& gdiDeviceName) {
    if (gdiDeviceName.empty()) return L"gdi:unknown";
    return L"gdi:" + Lowercase(gdiDeviceName);
}

std::vector<CcdMonitorIdentity> CaptureCcdMonitorIdentities() {
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    LONG result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 8 && result == ERROR_INSUFFICIENT_BUFFER;
         ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        result = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS) break;

        paths.resize(pathCount);
        modes.resize(modeCount);
        result = QueryDisplayConfig(
            flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_SUCCESS) {
            paths.resize(pathCount);
            modes.resize(modeCount);
        }
    }
    if (result != ERROR_SUCCESS) return {};

    std::vector<CcdMonitorIdentity> identities;
    identities.reserve(paths.size());
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
            sourceName.viewGdiDeviceName[0] == L'\0') {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;

        CcdMonitorIdentity identity;
        identity.gdiDeviceName = sourceName.viewGdiDeviceName;
        if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS &&
            targetName.monitorDevicePath[0] != L'\0') {
            identity.stableId = targetName.monitorDevicePath;
        } else {
            identity.stableId = BuildCcdFallbackId(
                path.targetInfo.adapterId,
                path.targetInfo.id,
                identity.gdiDeviceName);
        }
        identities.push_back(std::move(identity));
    }

    // Clone paths can expose more than one target for the same GDI source.
    // Use a deterministic identity in that case rather than path priority.
    std::sort(identities.begin(), identities.end(),
        [](const CcdMonitorIdentity& left, const CcdMonitorIdentity& right) {
            const int gdiOrder = _wcsicmp(
                left.gdiDeviceName.c_str(), right.gdiDeviceName.c_str());
            if (gdiOrder != 0) return gdiOrder < 0;
            return _wcsicmp(left.stableId.c_str(), right.stableId.c_str()) < 0;
        });
    return identities;
}

std::wstring FindStableIdForGdiName(
    const std::vector<CcdMonitorIdentity>& identities,
    const std::wstring& gdiDeviceName) {
    const auto found = std::find_if(
        identities.begin(), identities.end(),
        [&](const CcdMonitorIdentity& identity) {
            return EqualInsensitive(identity.gdiDeviceName, gdiDeviceName);
        });
    return found != identities.end()
        ? found->stableId
        : BuildGdiFallbackId(gdiDeviceName);
}

struct MonitorEnumerationContext {
    const std::vector<CcdMonitorIdentity>* identities = nullptr;
    std::vector<MonitorSnapshot>* monitors = nullptr;
};

BOOL CALLBACK CaptureMonitorSnapshot(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* context = reinterpret_cast<MonitorEnumerationContext*>(data);
    if (!context || !context->identities || !context->monitors) return FALSE;

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;

    MonitorSnapshot snapshot;
    snapshot.gdiDeviceName = info.szDevice;
    snapshot.stableId = FindStableIdForGdiName(
        *context->identities, snapshot.gdiDeviceName);
    snapshot.monitorRect = info.rcMonitor;
    snapshot.workArea = info.rcWork;
    snapshot.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (FAILED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI, &snapshot.dpiX, &snapshot.dpiY))) {
        snapshot.dpiX = 96;
        snapshot.dpiY = 96;
    }
    if (snapshot.dpiX == 0) snapshot.dpiX = 96;
    if (snapshot.dpiY == 0) snapshot.dpiY = 96;
    context->monitors->push_back(std::move(snapshot));
    return TRUE;
}

bool MonitorSnapshotLess(const MonitorSnapshot& left,
                         const MonitorSnapshot& right) {
    const int idOrder = _wcsicmp(left.stableId.c_str(), right.stableId.c_str());
    if (idOrder != 0) return idOrder < 0;
    if (left.monitorRect.left != right.monitorRect.left) {
        return left.monitorRect.left < right.monitorRect.left;
    }
    if (left.monitorRect.top != right.monitorRect.top) {
        return left.monitorRect.top < right.monitorRect.top;
    }
    if (left.monitorRect.right != right.monitorRect.right) {
        return left.monitorRect.right < right.monitorRect.right;
    }
    if (left.monitorRect.bottom != right.monitorRect.bottom) {
        return left.monitorRect.bottom < right.monitorRect.bottom;
    }
    const int gdiOrder = _wcsicmp(
        left.gdiDeviceName.c_str(), right.gdiDeviceName.c_str());
    return gdiOrder < 0;
}

bool MonitorGeometryLess(const MonitorSnapshot& left,
                         const MonitorSnapshot& right) {
    const LONG leftValues[] = {
        left.monitorRect.left, left.monitorRect.top,
        left.monitorRect.right, left.monitorRect.bottom,
        left.workArea.left, left.workArea.top,
        left.workArea.right, left.workArea.bottom
    };
    const LONG rightValues[] = {
        right.monitorRect.left, right.monitorRect.top,
        right.monitorRect.right, right.monitorRect.bottom,
        right.workArea.left, right.workArea.top,
        right.workArea.right, right.workArea.bottom
    };
    for (size_t index = 0; index < std::size(leftValues); ++index) {
        if (leftValues[index] != rightValues[index]) {
            return leftValues[index] < rightValues[index];
        }
    }
    if (left.dpiX != right.dpiX) return left.dpiX < right.dpiX;
    if (left.dpiY != right.dpiY) return left.dpiY < right.dpiY;
    if (left.primary != right.primary) return left.primary < right.primary;
    return false;
}

void AppendRect(std::wostringstream& stream, const RECT& rect) {
    stream << rect.left << L"," << rect.top << L","
           << rect.right << L"," << rect.bottom;
}

int To96Dpi(int physicalValue, UINT dpi) {
    return MulDiv(physicalValue, 96, static_cast<int>(dpi ? dpi : 96));
}

int From96Dpi(int logicalValue, UINT dpi) {
    return MulDiv(logicalValue, static_cast<int>(dpi ? dpi : 96), 96);
}

}

bool IsFenceInAnyWorkingArea(const RECT& fenceRect) {
    WorkAreaHitContext context{ &fenceRect, false };
    EnumDisplayMonitors(nullptr, nullptr, CheckWorkAreaIntersection,
                        reinterpret_cast<LPARAM>(&context));
    return context.intersects;
}

bool CenterFenceInPrimaryWorkArea(RECT& fenceRect) {
    if (IsFenceInAnyWorkingArea(fenceRect)) return false;

    const RECT workArea = GetPrimaryWorkArea();
    const int width = (std::max)(1L, fenceRect.right - fenceRect.left);
    const int height = (std::max)(1L, fenceRect.bottom - fenceRect.top);
    const int left = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int top = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
    fenceRect = { left, top, left + width, top + height };
    return true;
}

void ApplyFenceBounds(RECT& rect) {
    const RECT workArea = GetNearestMonitorWorkArea(rect);

    if (rect.right < workArea.left + MIN_VISIBLE_WIDTH) {
        OffsetRect(&rect, workArea.left + MIN_VISIBLE_WIDTH - rect.right, 0);
    }
    if (rect.left > workArea.right - MIN_VISIBLE_WIDTH) {
        OffsetRect(&rect, workArea.right - MIN_VISIBLE_WIDTH - rect.left, 0);
    }
    if (rect.top < workArea.top) {
        OffsetRect(&rect, 0, workArea.top - rect.top);
    }
    if (rect.top > workArea.bottom - MIN_VISIBLE_TITLE_HEIGHT) {
        OffsetRect(&rect, 0, workArea.bottom - MIN_VISIBLE_TITLE_HEIGHT - rect.top);
    }
}

void ApplyFenceBounds(
    RECT& rect, const DisplayTopologySnapshot& topology) {
    RECT workArea = GetPrimaryWorkArea();
    const MonitorSnapshot* nearest = nullptr;
    long long nearestDistance = (std::numeric_limits<long long>::max)();

    for (const auto& monitor : topology.monitors) {
        const RECT& bounds = monitor.monitorRect;
        const long long dx = rect.right < bounds.left
            ? static_cast<long long>(bounds.left) - rect.right
            : rect.left > bounds.right
                ? static_cast<long long>(rect.left) - bounds.right
                : 0;
        const long long dy = rect.bottom < bounds.top
            ? static_cast<long long>(bounds.top) - rect.bottom
            : rect.top > bounds.bottom
                ? static_cast<long long>(rect.top) - bounds.bottom
                : 0;
        const long long distance = dx * dx + dy * dy;
        if (!nearest || distance < nearestDistance) {
            nearest = &monitor;
            nearestDistance = distance;
        }
    }
    if (nearest) workArea = nearest->workArea;

    if (rect.right < workArea.left + MIN_VISIBLE_WIDTH) {
        OffsetRect(&rect, workArea.left + MIN_VISIBLE_WIDTH - rect.right, 0);
    }
    if (rect.left > workArea.right - MIN_VISIBLE_WIDTH) {
        OffsetRect(&rect, workArea.right - MIN_VISIBLE_WIDTH - rect.left, 0);
    }
    if (rect.top < workArea.top) {
        OffsetRect(&rect, 0, workArea.top - rect.top);
    }
    if (rect.top > workArea.bottom - MIN_VISIBLE_TITLE_HEIGHT) {
        OffsetRect(&rect, 0, workArea.bottom - MIN_VISIBLE_TITLE_HEIGHT - rect.top);
    }
}

DisplayTopologySnapshot CaptureTopology() {
    DisplayTopologySnapshot topology;
    topology.virtualScreen = GetVirtualScreen();

    const std::vector<CcdMonitorIdentity> identities =
        CaptureCcdMonitorIdentities();
    MonitorEnumerationContext context{ &identities, &topology.monitors };
    EnumDisplayMonitors(nullptr, nullptr, CaptureMonitorSnapshot,
                        reinterpret_cast<LPARAM>(&context));

    if (topology.monitors.empty()) {
        MonitorSnapshot fallback;
        fallback.stableId = L"gdi:primary";
        fallback.gdiDeviceName = L"Primary";
        fallback.monitorRect = topology.virtualScreen;
        fallback.workArea = GetPrimaryWorkArea();
        fallback.primary = true;
        topology.monitors.push_back(std::move(fallback));
    }

    std::sort(topology.monitors.begin(), topology.monitors.end(),
              MonitorSnapshotLess);
    topology.signature = BuildTopologySignature(topology);
    return topology;
}

std::wstring BuildTopologySignature(
    const DisplayTopologySnapshot& topology) {
    std::vector<MonitorSnapshot> sorted = topology.monitors;
    // This signature represents the coordinate space used for layout and
    // rendering, not the transient identity source returned by
    // QueryDisplayConfig. During a USB-C reconnect the same physical monitor
    // can alternate between a CCD device path and a GDI fallback name. If the
    // identity is included here, every alternation looks like a new topology
    // and repeatedly rebuilds the full-screen layered window while dragging.
    std::sort(sorted.begin(), sorted.end(), MonitorGeometryLess);

    std::wostringstream stream;
    stream << L"virtual:";
    AppendRect(stream, topology.virtualScreen);
    stream << L";count:" << sorted.size();
    for (const auto& monitor : sorted) {
        stream << L";monitor:";
        AppendRect(stream, monitor.monitorRect);
        stream << L";work:";
        AppendRect(stream, monitor.workArea);
        stream << L";dpi:" << monitor.dpiX << L"," << monitor.dpiY;
        stream << L";primary:" << (monitor.primary ? 1 : 0);
    }
    return stream.str();
}

const MonitorSnapshot* FindMonitorById(
    const DisplayTopologySnapshot& topology,
    const std::wstring& monitorId) {
    if (monitorId.empty()) return nullptr;
    const auto found = std::find_if(
        topology.monitors.begin(), topology.monitors.end(),
        [&](const MonitorSnapshot& monitor) {
            return EqualInsensitive(monitor.stableId, monitorId);
        });
    return found != topology.monitors.end() ? &*found : nullptr;
}

const MonitorSnapshot* FindMonitorForPoint(
    const DisplayTopologySnapshot& topology,
    POINT physicalPoint) {
    const auto inWorkArea = std::find_if(
        topology.monitors.begin(), topology.monitors.end(),
        [&](const MonitorSnapshot& monitor) {
            return PtInRect(&monitor.workArea, physicalPoint) != FALSE;
        });
    if (inWorkArea != topology.monitors.end()) return &*inWorkArea;

    const auto inMonitor = std::find_if(
        topology.monitors.begin(), topology.monitors.end(),
        [&](const MonitorSnapshot& monitor) {
            return PtInRect(&monitor.monitorRect, physicalPoint) != FALSE;
        });
    return inMonitor != topology.monitors.end() ? &*inMonitor : nullptr;
}

MonitorRelativePlacement CapturePlacement96(
    const RECT& physicalRect,
    int expandedHeightPhysical,
    const MonitorSnapshot& monitor) {
    MonitorRelativePlacement placement;
    if (monitor.stableId.empty()) return placement;

    const int width = (std::max)(1L, physicalRect.right - physicalRect.left);
    const int height = (std::max)(1L, physicalRect.bottom - physicalRect.top);
    placement.monitorId = monitor.stableId;
    placement.x96 = To96Dpi(physicalRect.left - monitor.workArea.left,
                            monitor.dpiX);
    placement.y96 = To96Dpi(physicalRect.top - monitor.workArea.top,
                            monitor.dpiY);
    placement.width96 = (std::max)(1, To96Dpi(width, monitor.dpiX));
    placement.height96 = (std::max)(1, To96Dpi(height, monitor.dpiY));
    placement.expandedHeight96 = (std::max)(
        1, To96Dpi((std::max)(1, expandedHeightPhysical), monitor.dpiY));
    placement.valid = true;
    return placement;
}

RECT MaterializePlacement96(
    const MonitorRelativePlacement& placement,
    const MonitorSnapshot& monitor,
    int* expandedHeightPhysical) {
    if (expandedHeightPhysical) *expandedHeightPhysical = 0;
    if (!placement.valid) return {};

    const int left = monitor.workArea.left +
        From96Dpi(placement.x96, monitor.dpiX);
    const int top = monitor.workArea.top +
        From96Dpi(placement.y96, monitor.dpiY);
    const int width = (std::max)(
        1, From96Dpi((std::max)(1, placement.width96), monitor.dpiX));
    const int height = (std::max)(
        1, From96Dpi((std::max)(1, placement.height96), monitor.dpiY));
    if (expandedHeightPhysical) {
        *expandedHeightPhysical = (std::max)(
            1, From96Dpi(
                (std::max)(1, placement.expandedHeight96), monitor.dpiY));
    }
    return { left, top, left + width, top + height };
}

MonitorRelativePoint CapturePoint96(
    POINT physicalPoint,
    const MonitorSnapshot& monitor) {
    MonitorRelativePoint placement;
    if (monitor.stableId.empty()) return placement;
    placement.monitorId = monitor.stableId;
    placement.x96 = To96Dpi(
        physicalPoint.x - monitor.workArea.left, monitor.dpiX);
    placement.y96 = To96Dpi(
        physicalPoint.y - monitor.workArea.top, monitor.dpiY);
    placement.valid = true;
    return placement;
}

POINT MaterializePoint96(
    const MonitorRelativePoint& placement,
    const MonitorSnapshot& monitor) {
    if (!placement.valid) return {};
    return {
        monitor.workArea.left + From96Dpi(placement.x96, monitor.dpiX),
        monitor.workArea.top + From96Dpi(placement.y96, monitor.dpiY)
    };
}

}
