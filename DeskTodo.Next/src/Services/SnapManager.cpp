#include "SnapManager.h"
#include <algorithm>
#include <limits>

namespace {

long long IntersectionArea(const RECT& first, const RECT& second) {
    const LONG left = (std::max)(first.left, second.left);
    const LONG top = (std::max)(first.top, second.top);
    const LONG right = (std::min)(first.right, second.right);
    const LONG bottom = (std::min)(first.bottom, second.bottom);
    if (right <= left || bottom <= top) return 0;
    return static_cast<long long>(right - left) * (bottom - top);
}

const MonitorService::MonitorSnapshot* FindFenceMonitor(
    const MonitorService::DisplayTopologySnapshot& topology,
    const RECT& rect) {
    const MonitorService::MonitorSnapshot* best = nullptr;
    long long bestArea = 0;
    for (const auto& monitor : topology.monitors) {
        const long long area = IntersectionArea(rect, monitor.monitorRect);
        if (area > bestArea) {
            best = &monitor;
            bestArea = area;
        }
    }
    return bestArea > 0 ? best : nullptr;
}

bool IsSnapTargetOnMovingMonitor(
    const Fence& target,
    const MonitorService::DisplayTopologySnapshot& topology,
    const MonitorService::MonitorSnapshot* movingMonitor) {
    if (!movingMonitor) {
        // A topology-less synthetic caller cannot perform the geometric
        // test. Preserve the legacy runtime flag only for that fallback.
        return target.monitorOnline;
    }
    // monitorOnline can be stale while a hot-plug transaction is replacing a
    // CCD device path. Runtime geometry in the immutable drag topology is the
    // authoritative source for snapping. Requiring the same best-overlap
    // monitor also prevents guide lines and candidates from crossing screens.
    return FindFenceMonitor(topology, target.rect) == movingMonitor;
}

} // namespace

SnapResult SnapManager::ApplySnap(
    const std::wstring& movingFenceId,
    const RECT& proposedRect,
    const std::vector<Fence>& fences,
    const MonitorService::DisplayTopologySnapshot& topology)
{
    // Magnetic placement is intentionally limited to fence-to-fence
    // alignment. Snapping to the outer virtual-desktop rectangle makes a
    // guide line span both monitors after a reconnect and expands the dirty
    // redraw region to the whole desktop.
    SnapResult result;
    result.snappedRect = proposedRect;

    auto vertical = FindBestVerticalSnap(
        movingFenceId, proposedRect, fences, topology);

    if (vertical.has_value()) {
        int offsetX = vertical->targetValue - vertical->sourceValue;
        OffsetRect(&result.snappedRect, offsetX, 0);
        result.snappedX = true;
        result.offsetX = offsetX;
        result.lines.push_back(
            BuildSnapLine(*vertical, result.snappedRect));
    }

    auto horizontal = FindBestHorizontalSnap(
        movingFenceId, result.snappedRect, fences, topology);

    if (horizontal.has_value()) {
        int offsetY = horizontal->targetValue - horizontal->sourceValue;
        OffsetRect(&result.snappedRect, 0, offsetY);
        result.snappedY = true;
        result.offsetY = offsetY;
        result.lines.push_back(
            BuildSnapLine(*horizontal, result.snappedRect));
    }

    return result;
}

void SnapManager::Clear() {
}

std::optional<SnapCandidate> SnapManager::FindBestVerticalSnap(
    const std::wstring& movingFenceId,
    const RECT& proposedRect,
    const std::vector<Fence>& fences,
    const MonitorService::DisplayTopologySnapshot& topology)
{
    std::vector<SnapCandidate> candidates;
    const auto* movingMonitor = FindFenceMonitor(topology, proposedRect);

    for (const auto& f : fences) {
        if (f.id == movingFenceId ||
            !IsSnapTargetOnMovingMonitor(f, topology, movingMonitor)) {
            continue;
        }

        CollectFenceVerticalCandidates(proposedRect, f, candidates);
    }

    return SelectBestCandidate(candidates);
}

std::optional<SnapCandidate> SnapManager::FindBestHorizontalSnap(
    const std::wstring& movingFenceId,
    const RECT& proposedRect,
    const std::vector<Fence>& fences,
    const MonitorService::DisplayTopologySnapshot& topology)
{
    std::vector<SnapCandidate> candidates;
    const auto* movingMonitor = FindFenceMonitor(topology, proposedRect);

    for (const auto& f : fences) {
        if (f.id == movingFenceId ||
            !IsSnapTargetOnMovingMonitor(f, topology, movingMonitor)) {
            continue;
        }

        CollectFenceHorizontalCandidates(proposedRect, f, candidates);
    }

    return SelectBestCandidate(candidates);
}

void SnapManager::CollectFenceVerticalCandidates(
    const RECT& proposedRect,
    const Fence& targetFence,
    std::vector<SnapCandidate>& candidates) const
{
    const RECT& tr = targetFence.rect;
    int sLeft = proposedRect.left;
    int sRight = proposedRect.right;
    int sCenterX = RectCenterX(proposedRect);

    int tLeft = tr.left;
    int tRight = tr.right;
    int tCenterX = RectCenterX(tr);

    auto add = [&](SnapType type, int sv, int tv) {
        int d = std::abs(sv - tv);
        if (d < SNAP_THRESHOLD) {
            SnapCandidate c;
            c.axis = SnapAxis::Vertical;
            c.type = type;
            c.sourceValue = sv;
            c.targetValue = tv;
            c.distance = d;
            c.targetFenceId = targetFence.id;
            c.targetRect = tr;
            candidates.push_back(c);
        }
    };

    add(SnapType::LeftToLeft, sLeft, tLeft);
    add(SnapType::LeftToRight, sLeft, tRight);
    add(SnapType::RightToLeft, sRight, tLeft);
    add(SnapType::RightToRight, sRight, tRight);
    add(SnapType::CenterXToCenterX, sCenterX, tCenterX);
}

void SnapManager::CollectFenceHorizontalCandidates(
    const RECT& proposedRect,
    const Fence& targetFence,
    std::vector<SnapCandidate>& candidates) const
{
    const RECT& tr = targetFence.rect;
    int sTop = proposedRect.top;
    int sBottom = proposedRect.bottom;
    int sCenterY = RectCenterY(proposedRect);

    int tTop = tr.top;
    int tBottom = tr.bottom;
    int tCenterY = RectCenterY(tr);

    auto add = [&](SnapType type, int sv, int tv) {
        int d = std::abs(sv - tv);
        if (d < SNAP_THRESHOLD) {
            SnapCandidate c;
            c.axis = SnapAxis::Horizontal;
            c.type = type;
            c.sourceValue = sv;
            c.targetValue = tv;
            c.distance = d;
            c.targetFenceId = targetFence.id;
            c.targetRect = tr;
            candidates.push_back(c);
        }
    };

    add(SnapType::TopToTop, sTop, tTop);
    add(SnapType::TopToBottom, sTop, tBottom);
    add(SnapType::BottomToTop, sBottom, tTop);
    add(SnapType::BottomToBottom, sBottom, tBottom);
    add(SnapType::CenterYToCenterY, sCenterY, tCenterY);
}

void SnapManager::CollectDesktopVerticalCandidates(
    const RECT& proposedRect,
    const RECT& desktopBounds,
    std::vector<SnapCandidate>& candidates) const
{
    int sLeft = proposedRect.left;
    int sRight = proposedRect.right;
    int sCenterX = RectCenterX(proposedRect);

    int dLeft = desktopBounds.left;
    int dRight = desktopBounds.right;
    int dCenterX = RectCenterX(desktopBounds);

    auto add = [&](SnapType type, int sv, int tv) {
        int d = std::abs(sv - tv);
        if (d < SNAP_THRESHOLD) {
            SnapCandidate c;
            c.axis = SnapAxis::Vertical;
            c.type = type;
            c.sourceValue = sv;
            c.targetValue = tv;
            c.distance = d;
            c.targetFenceId = L"";
            c.targetRect = desktopBounds;
            candidates.push_back(c);
        }
    };

    add(SnapType::DesktopLeft, sLeft, dLeft);
    add(SnapType::DesktopRight, sRight, dRight);
    add(SnapType::DesktopCenterX, sCenterX, dCenterX);
}

void SnapManager::CollectDesktopHorizontalCandidates(
    const RECT& proposedRect,
    const RECT& desktopBounds,
    std::vector<SnapCandidate>& candidates) const
{
    int sTop = proposedRect.top;
    int sBottom = proposedRect.bottom;
    int sCenterY = RectCenterY(proposedRect);

    int dTop = desktopBounds.top;
    int dBottom = desktopBounds.bottom;
    int dCenterY = RectCenterY(desktopBounds);

    auto add = [&](SnapType type, int sv, int tv) {
        int d = std::abs(sv - tv);
        if (d < SNAP_THRESHOLD) {
            SnapCandidate c;
            c.axis = SnapAxis::Horizontal;
            c.type = type;
            c.sourceValue = sv;
            c.targetValue = tv;
            c.distance = d;
            c.targetFenceId = L"";
            c.targetRect = desktopBounds;
            candidates.push_back(c);
        }
    };

    add(SnapType::DesktopTop, sTop, dTop);
    add(SnapType::DesktopBottom, sBottom, dBottom);
    add(SnapType::DesktopCenterY, sCenterY, dCenterY);
}

std::optional<SnapCandidate> SnapManager::SelectBestCandidate(
    std::vector<SnapCandidate>& candidates) const
{
    if (candidates.empty()) {
        return std::nullopt;
    }

    auto best = std::min_element(
        candidates.begin(), candidates.end(),
        [](const SnapCandidate& a, const SnapCandidate& b) {
            return a.distance < b.distance;
        });

    return *best;
}

SnapLine SnapManager::BuildSnapLine(
    const SnapCandidate& candidate,
    const RECT& snappedRect) const
{
    SnapLine line;
    line.visible = true;
    line.axis = candidate.axis;
    line.type = candidate.type;
    line.targetFenceId = candidate.targetFenceId;

    if (candidate.axis == SnapAxis::Vertical) {
        int x = candidate.targetValue;
        line.start = {
            x,
            (std::min)(snappedRect.top, candidate.targetRect.top)
        };
        line.end = {
            x,
            (std::max)(snappedRect.bottom, candidate.targetRect.bottom)
        };
    } else if (candidate.axis == SnapAxis::Horizontal) {
        int y = candidate.targetValue;
        line.start = {
            (std::min)(snappedRect.left, candidate.targetRect.left),
            y
        };
        line.end = {
            (std::max)(snappedRect.right, candidate.targetRect.right),
            y
        };
    }

    return line;
}
