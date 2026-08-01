#pragma once
#include <string>
#include <vector>
#include <optional>
#include <Windows.h>
#include "Models/SnapCommon.h"
#include "Models/Fence.h"
#include "Services/MonitorService.h"

constexpr int SNAP_THRESHOLD = 10;

class SnapManager {
public:
    SnapResult ApplySnap(
        const std::wstring& movingFenceId,
        const RECT& proposedRect,
        const std::vector<Fence>& fences,
        const MonitorService::DisplayTopologySnapshot& topology
    );

    void Clear();

private:
    std::optional<SnapCandidate> FindBestVerticalSnap(
        const std::wstring& movingFenceId,
        const RECT& proposedRect,
        const std::vector<Fence>& fences,
        const MonitorService::DisplayTopologySnapshot& topology
    );

    std::optional<SnapCandidate> FindBestHorizontalSnap(
        const std::wstring& movingFenceId,
        const RECT& proposedRect,
        const std::vector<Fence>& fences,
        const MonitorService::DisplayTopologySnapshot& topology
    );

    SnapLine BuildSnapLine(
        const SnapCandidate& candidate,
        const RECT& snappedRect
    ) const;

    void CollectFenceVerticalCandidates(
        const RECT& proposedRect,
        const Fence& targetFence,
        std::vector<SnapCandidate>& candidates
    ) const;

    void CollectFenceHorizontalCandidates(
        const RECT& proposedRect,
        const Fence& targetFence,
        std::vector<SnapCandidate>& candidates
    ) const;

    void CollectDesktopVerticalCandidates(
        const RECT& proposedRect,
        const RECT& desktopBounds,
        std::vector<SnapCandidate>& candidates
    ) const;

    void CollectDesktopHorizontalCandidates(
        const RECT& proposedRect,
        const RECT& desktopBounds,
        std::vector<SnapCandidate>& candidates
    ) const;

    std::optional<SnapCandidate> SelectBestCandidate(
        std::vector<SnapCandidate>& candidates
    ) const;
};
