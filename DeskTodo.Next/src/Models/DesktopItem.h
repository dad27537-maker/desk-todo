#pragma once
#include <string>
#include <Windows.h>

struct DesktopItem {
    std::wstring name;
    std::wstring fullPath;
    std::wstring ext;
    bool isDirectory = false;
    POINT relativeOffset{};
    // 该字段保留兼容，布局不再依赖它（强制网格布局）
    bool hasPersistedOffset = false;

    RECT iconRect{};
    RECT textRect{};
    RECT hitRect{};
    bool hovered = false;
    bool selected = false;

    // 搜索过滤标志：为 true 时表示被搜索过滤掉，不参与显示与命中
    bool filtered = false;

};
