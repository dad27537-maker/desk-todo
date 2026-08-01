#pragma once
#include <string>
#include <Windows.h>

enum class DragSourceType {
    None,
    FenceItem,
    ExternalFiles
};

enum class DropTargetType {
    None,
    Fence,
    Desktop,
    Invalid
};

struct DropTargetInfo {
    DropTargetType type = DropTargetType::None;
    std::wstring fenceId;
};

struct GhostIcon {
    bool visible = false;
    std::wstring name;
    POINT position = {0, 0};
    int iconSize = 40;
    float opacity = 0.75f;
};

struct DragContext {
    bool active = false;
    bool hasMoved = false;

    DragSourceType sourceType = DragSourceType::None;

    std::wstring sourceFenceId;
    int sourceItemIndex = -1;

    std::wstring sourcePath;
    std::wstring originalPath;

    POINT startMouse = {0, 0};
    POINT currentMouse = {0, 0};
    POINT snappedPoint = {0, 0};
    bool hasSnappedPoint = false;

    RECT originalIconRect = {};
    RECT originalTextRect = {};
    RECT originalHitRect = {};

    DropTargetInfo currentTarget;

    bool hideOriginalIcon = false;
    bool dimOriginalIcon = false;
};
