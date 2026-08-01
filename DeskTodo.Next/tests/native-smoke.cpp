#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "Models/Fence.h"
#include "Models/PomodoroTimer.h"
#include "Models/TodoList.h"
#include "Models/CaptureRegion.h"
#include "Models/CaptureToolbar.h"
#include "Components/BlackHole/BlackHoleComponent.h"
#include "Window/SettingsWindowLayout.h"
#include "Services/FileMoveService.h"
#include "Services/GridLayoutService.h"
#include "Services/ConfigService.h"
#include "Services/DesktopIconLayoutService.h"
#include "Services/MonitorService.h"
#include "Services/SnapManager.h"
#include "Services/ShellIntegrationService.h"
#include "Utils/JsonUtils.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void WriteText(const fs::path& path, const std::string& value) {
    std::ofstream stream(path, std::ios::binary);
    stream << value;
}

std::string ReadText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

class FakeBlackHoleBackend final : public BlackHole::IBlackHoleRenderBackend {
public:
    bool Initialize(const BlackHole::SurfaceDescriptor&) override {
        initialized = true;
        return true;
    }
    bool Resize(uint32_t, uint32_t, float) override { return true; }
    bool Render(const BlackHole::Settings& value, const BlackHole::FrameContext&,
                BlackHole::FrameStats& stats) override {
        ++renderCount;
        lastSettings = value;
        stats.gpuMilliseconds = 4.0f;
        stats.effectiveRenderScale = value.renderScale;
        return true;
    }
    bool CopyFramePixels(std::vector<uint8_t>&, uint32_t&, uint32_t&) override {
        return false;
    }
    void Suspend() override { suspended = true; }
    void Resume() override { suspended = false; }
    void Shutdown() override { shutdown = true; }

    bool initialized = false;
    bool suspended = false;
    bool shutdown = false;
    int renderCount = 0;
    BlackHole::Settings lastSettings{};
};

struct SettingsMonitorCheck {
    int monitorCount = 0;
    bool allFit = true;
};

BOOL CALLBACK CheckSettingsOnMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* check = reinterpret_cast<SettingsMonitorCheck*>(data);
    MONITORINFO info{ sizeof(info) };
    if (!check || !GetMonitorInfoW(monitor, &info)) return TRUE;
    ++check->monitorCount;
    const POINT center{ (info.rcWork.left + info.rcWork.right) / 2,
                        (info.rcWork.top + info.rcWork.bottom) / 2 };
    const auto placement = SettingsWindowLayout::FitToWorkArea(info.rcWork, center);
    check->allFit = check->allFit &&
        placement.rect.left >= info.rcWork.left &&
        placement.rect.right <= info.rcWork.right &&
        placement.rect.top >= info.rcWork.top &&
        placement.rect.bottom <= info.rcWork.bottom;
    return TRUE;
}

}

int main() {
    const fs::path root = fs::current_path() / "DeskTodo.Next" / "build-current" / "runtime-native";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "source", ec);
    fs::create_directories(root / "target", ec);
    Check(!ec, "test directories are created");

    FileMoveService mover;
    std::wstring movedPath;

    const fs::path sourceFile = root / "source" / "move.txt";
    WriteText(sourceFile, "move-data");
    FileMoveResult result = mover.MoveToDirectory(
        sourceFile.wstring(), (root / "target").wstring(), false, &movedPath);
    Check(result == FileMoveResult::Success, "regular file move succeeds");
    Check(!fs::exists(sourceFile), "source disappears after successful move");
    Check(ReadText(root / "target" / "move.txt") == "move-data", "target content is preserved");

    result = mover.MoveToDirectory(
        (root / "target" / "move.txt").wstring(), (root / "target").wstring(), false, &movedPath);
    Check(result == FileMoveResult::SameLocation, "same-directory move performs no IO");
    Check(fs::exists(root / "target" / "move.txt"), "same-directory source remains intact");

    const fs::path conflictSource = root / "source" / "conflict.txt";
    const fs::path conflictTarget = root / "target" / "conflict.txt";
    WriteText(conflictSource, "new-content");
    WriteText(conflictTarget, "old-content");
    result = mover.MoveToDirectory(
        conflictSource.wstring(), (root / "target").wstring(), false, &movedPath);
    Check(result == FileMoveResult::Conflict, "same-name target returns conflict");
    Check(ReadText(conflictSource) == "new-content", "conflict keeps source unchanged");
    Check(ReadText(conflictTarget) == "old-content", "conflict never overwrites target");

    result = mover.MoveToDirectory(
        conflictSource.wstring(), (root / "target").wstring(), true, &movedPath);
    Check(result == FileMoveResult::Success, "explicit overwrite succeeds");
    Check(!fs::exists(conflictSource), "overwrite removes source only after success");
    Check(ReadText(conflictTarget) == "new-content", "overwrite installs source content");

    const fs::path shortcutSource = root / "source" / "sample.lnk";
    WriteText(shortcutSource, "shortcut-file-bytes");
    result = mover.MoveToDirectory(
        shortcutSource.wstring(), (root / "target").wstring(), false, &movedPath);
    Check(result == FileMoveResult::Success, "lnk file itself is moved");
    Check(ReadText(root / "target" / "sample.lnk") == "shortcut-file-bytes",
          "lnk contents are not dereferenced");

    const fs::path lockedSource = root / "source" / "locked.txt";
    WriteText(lockedSource, "locked-data");
    HANDLE lock = CreateFileW(lockedSource.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    result = mover.MoveToDirectory(
        lockedSource.wstring(), (root / "target").wstring(), false, &movedPath);
    Check(result == FileMoveResult::Failed, "IO failure is returned to caller");
    Check(fs::exists(lockedSource), "IO failure leaves source intact");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);

    const fs::path desktopRoot = root / "desktop";
    const fs::path fenceRoot = root / "physical-fence";
    fs::create_directories(desktopRoot / "Project" / "nested", ec);
    fs::create_directories(fenceRoot, ec);
    WriteText(desktopRoot / "Project" / "nested" / "visible.txt", "path-visible");
    SetEnvironmentVariableW(L"DESKTODO_DESKTOP_ROOT", desktopRoot.c_str());
    result = mover.MoveToDirectory(
        (desktopRoot / "Project").wstring(), fenceRoot.wstring(), false, &movedPath);
    Check(result == FileMoveResult::Success,
          "desktop folder moves into the real fence directory");
    Check(!fs::exists(desktopRoot / "Project") &&
          fs::exists(fenceRoot / "Project" / "nested" / "visible.txt"),
          "fence path contains the real folder and desktop has no duplicate");
    result = mover.MoveToDirectory(
        (fenceRoot / "Project").wstring(), desktopRoot.wstring(), false, &movedPath);
    Check(result == FileMoveResult::Success &&
          fs::exists(desktopRoot / "Project" / "nested" / "visible.txt") &&
          !fs::exists(fenceRoot / "Project"),
          "exit-style move restores the complete folder to desktop");
    SetEnvironmentVariableW(L"DESKTODO_DESKTOP_ROOT", nullptr);

    Fence fence;
    fence.id = L"test-fence";
    fence.title = L"Test";
    fence.rect = { 10, 20, 310, 260 };
    fence.folderPath = L"C:\\Fence";
    fence.itemOrder = { L"first.txt", L"shortcut.lnk", L"last.md" };
    fence.memberPaths = { L"C:\\Desktop\\DeskTodo", L"E:\\Projects\\locked.exe" };
    fence.itemDesktopOrigins = { POINT{ 120, 240 }, POINT{ 360, 240 }, POINT{ LONG_MIN, LONG_MIN } };
    fence.itemDesktopOriginPlacements = {
        { L"secondary-device", 100, 100, true },
        { L"primary-device", -32, 48, true }
    };
    fence.itemOffsets = { POINT{ 10, 12 }, POINT{ 94, 12 }, POINT{ 178, 108 } };
    fence.kind = FenceKind::Game;
    fence.monitorPlacement = {
        L"monitor-device-path", 12, 24, 300, 240, 240, true
    };
    const std::wstring json = JsonSerialize({ fence });
    std::vector<Fence> restored;
    int configVersion = 0;
    Check(JsonDeserialize(json, restored, &configVersion), "serialized layout can be parsed");
    Check(configVersion == 9, "per-item monitor-aware layout uses config version nine");
    Check(restored.size() == 1, "one fence is restored");
    Check(restored.size() == 1 && restored[0].itemOrder == fence.itemOrder,
          "item grid order survives round trip");
    Check(restored.size() == 1 && restored[0].memberPaths == fence.memberPaths,
          "real logical member paths survive round trip");
    Check(restored.size() == 1 && restored[0].itemDesktopOrigins.size() == 3 &&
          restored[0].itemDesktopOrigins[1].x == 360 &&
          restored[0].itemDesktopOrigins[2].x == LONG_MIN,
          "original physical desktop positions survive round trip");
    Check(restored.size() == 1 &&
          restored[0].itemDesktopOriginPlacements.size() ==
              restored[0].itemOrder.size() &&
          restored[0].itemDesktopOriginPlacements[0].valid &&
          restored[0].itemDesktopOriginPlacements[0].monitorId ==
              L"secondary-device" &&
          restored[0].itemDesktopOriginPlacements[0].x96 == 100 &&
          restored[0].itemDesktopOriginPlacements[0].y96 == 100 &&
          restored[0].itemDesktopOriginPlacements[1].valid &&
          restored[0].itemDesktopOriginPlacements[1].monitorId ==
              L"primary-device" &&
          restored[0].itemDesktopOriginPlacements[1].x96 == -32 &&
          restored[0].itemDesktopOriginPlacements[1].y96 == 48 &&
          !restored[0].itemDesktopOriginPlacements[2].valid &&
          restored[0].itemDesktopOriginPlacements[2].monitorId.empty(),
          "per-item monitor-relative origins round-trip aligned with item order");
    std::vector<Fence> overlongPlacements;
    Check(JsonDeserialize(
              L"{\"version\":9,\"fences\":[{"
              L"\"itemOrder\":[\"only-item\"],"
              L"\"itemDesktopOriginMonitorIds\":[\"secondary\",\"surplus\"],"
              L"\"itemDesktopOriginX96\":[12,999],"
              L"\"itemDesktopOriginY96\":[34,999],"
              L"\"itemDesktopOriginPlacementValid\":[true,true]}]}",
              overlongPlacements, nullptr) &&
          overlongPlacements.size() == 1 &&
          overlongPlacements[0].itemDesktopOriginPlacements.size() == 1 &&
          overlongPlacements[0].itemDesktopOriginPlacements[0].monitorId ==
              L"secondary" &&
          overlongPlacements[0].itemDesktopOriginPlacements[0].x96 == 12 &&
          overlongPlacements[0].itemDesktopOriginPlacements[0].y96 == 34 &&
          overlongPlacements[0].itemDesktopOriginPlacements[0].valid,
          "surplus per-item origin placements are truncated to item order");
    std::vector<Fence> incompletePlacements;
    Check(JsonDeserialize(
              L"{\"version\":9,\"fences\":[{"
              L"\"itemOrder\":[\"item\"],"
              L"\"monitorPlacementValid\":true,"
              L"\"itemDesktopOriginPlacementValid\":[true]}]}",
              incompletePlacements, nullptr) &&
          incompletePlacements.size() == 1 &&
          !incompletePlacements[0].monitorPlacement.valid &&
          incompletePlacements[0].itemDesktopOriginPlacements.size() == 1 &&
          !incompletePlacements[0].itemDesktopOriginPlacements[0].valid,
          "monitor-relative placements without a stable monitor id are ignored");
    // 新行为：itemOffsets 不再持久化（只保存顺序），验证序列化后为空
    Check(restored.size() == 1 && restored[0].itemOffsets.empty(),
          "item offsets are no longer persisted - only order is saved");
    Check(restored.size() == 1 && restored[0].rect.left == 10 && restored[0].rect.top == 20,
          "fence coordinates survive round trip");
    Check(restored.size() == 1 && restored[0].kind == FenceKind::Game &&
          restored[0].monitorPlacement.valid &&
          restored[0].monitorPlacement.monitorId == L"monitor-device-path" &&
          restored[0].monitorPlacement.x96 == 12,
          "game kind and monitor-relative placement survive round trip");
    Check(json.find(L"\"coordinateSpace\": \"monitorRelative96Dpi\"") != std::wstring::npos,
          "configuration declares monitor-relative 96-DPI coordinates");
    std::vector<Fence> rejectedFences{ fence };
    int rejectedVersion = 99;
    Check(!JsonDeserialize(
              L"{\"version\":8,\"fences\":[{\"id\":\"broken\","
              L"\"memberPaths\":oops}]}",
              rejectedFences, &rejectedVersion) &&
          rejectedFences.empty() && rejectedVersion == 0,
          "complete outer JSON rejects an invalid nested fence value");
    Check(!JsonDeserialize(json + L" trailing-data", rejectedFences, nullptr),
          "configuration rejects non-whitespace trailing data");
    Check(!JsonDeserialize(
              L"{\"version\":8,\"fences\":[{\"id\":\"broken\"}}",
              rejectedFences, nullptr),
          "configuration rejects an unclosed fences array");

    const fs::path configRoot = root / "layout-config";
    SetEnvironmentVariableW(L"DESKTODO_DATA_ROOT", configRoot.c_str());
    ConfigService configService;
    configService.SaveFences({ fence });
    const fs::path backupPath = configRoot / "chosen-layout.bak";
    std::vector<DesktopPositionRecord> desktopSnapshot{
        { L"C:\\Users\\Test\\Desktop\\loose.txt", POINT{ 640, 360 } }
    };
    Check(configService.BackupLayout({ fence }, desktopSnapshot, backupPath.wstring()),
          "layout backup file is created at the chosen bak path");
    Fence changedFence = fence;
    changedFence.rect = { 900, 800, 1200, 1040 };
    configService.SaveFences({ changedFence });
    WriteText(configRoot / "config.json", "{\"version\":8,\"fences\":[");
    ConfigService recoveredConfigService;
    const auto recoveredFromLastGood =
        recoveredConfigService.LoadFencesWithStatus();
    Check(recoveredFromLastGood.status == ConfigLoadStatus::Loaded &&
          recoveredFromLastGood.loadedFromLastGood &&
          recoveredFromLastGood.fences.size() == 1 &&
          recoveredFromLastGood.fences[0].rect.left == changedFence.rect.left &&
          recoveredFromLastGood.version == 9,
          "truncated primary config recovers the last complete atomic snapshot");
    WriteText(
        configRoot / "config.json",
        "{\"version\":8,\"fences\":[{\"id\":\"broken\","
        "\"memberPaths\":oops}]}");
    ConfigService nestedCorruptionService;
    const auto nestedCorruptionResult =
        nestedCorruptionService.LoadFencesWithStatus();
    Check(nestedCorruptionResult.status == ConfigLoadStatus::Loaded &&
          nestedCorruptionResult.loadedFromLastGood &&
          nestedCorruptionResult.fences.size() == 1 &&
          nestedCorruptionResult.fences[0].rect.left == changedFence.rect.left,
          "invalid nested primary config falls back to last-good");
    WriteText(
        configRoot / "config.json",
        "{\"version\":8,\"fences\":[]} trailing-data");
    ConfigService trailingGarbageService;
    const auto trailingGarbageResult =
        trailingGarbageService.LoadFencesWithStatus();
    Check(trailingGarbageResult.status == ConfigLoadStatus::Loaded &&
          trailingGarbageResult.loadedFromLastGood &&
          trailingGarbageResult.fences.size() == 1,
          "primary config with trailing garbage falls back to last-good");
    std::vector<Fence> restoredBackup;
    std::vector<DesktopPositionRecord> restoredDesktopSnapshot;
    Check(configService.RestoreLayout(
              backupPath.wstring(), restoredBackup, restoredDesktopSnapshot),
          "chosen layout backup can be restored");
    Check(restoredBackup.size() == 1 && restoredBackup[0].rect.left == fence.rect.left &&
          restoredBackup[0].rect.top == fence.rect.top,
          "restored layout matches the backed up coordinates");
    Check(restoredDesktopSnapshot.size() == 1 &&
          restoredDesktopSnapshot[0].position.x == 640 &&
          restoredDesktopSnapshot[0].path.find(L"loose.txt") != std::wstring::npos,
          "desktop file positions survive the bak round trip");

    const fs::path missingConfigRoot = root / "missing-layout-config";
    SetEnvironmentVariableW(
        L"DESKTODO_DATA_ROOT", missingConfigRoot.c_str());
    ConfigService missingConfigService;
    const auto missingConfigResult =
        missingConfigService.LoadFencesWithStatus();
    Check(missingConfigResult.status == ConfigLoadStatus::Missing &&
          missingConfigResult.fences.empty(),
          "absent primary and last-good report Missing");

    const fs::path corruptConfigRoot = root / "corrupt-layout-config";
    SetEnvironmentVariableW(
        L"DESKTODO_DATA_ROOT", corruptConfigRoot.c_str());
    ConfigService corruptConfigService;
    WriteText(
        corruptConfigRoot / "config.json",
        "{\"version\":8,\"fences\":[{\"memberPaths\":oops}]}");
    const auto corruptConfigResult =
        corruptConfigService.LoadFencesWithStatus();
    Check(corruptConfigResult.status == ConfigLoadStatus::Corrupt &&
          corruptConfigResult.fences.empty(),
          "invalid primary without last-good reports Corrupt");

    const fs::path ioErrorConfigRoot = root / "io-error-layout-config";
    SetEnvironmentVariableW(
        L"DESKTODO_DATA_ROOT", ioErrorConfigRoot.c_str());
    ConfigService ioErrorConfigService;
    fs::create_directory(ioErrorConfigRoot / "config.json", ec);
    const auto ioErrorConfigResult =
        ioErrorConfigService.LoadFencesWithStatus();
    Check(ioErrorConfigResult.status == ConfigLoadStatus::IoError &&
          ioErrorConfigResult.fences.empty(),
          "inaccessible primary reports IoError instead of Missing");
    SetEnvironmentVariableW(L"DESKTODO_DATA_ROOT", nullptr);

    RECT content{ 100, 100, 500, 500 };
    Check(GridLayoutService::GetDesktopIconSize(content) == 40,
          "fence icon size remains compact and stable");
    Check(GridLayoutService::GetNearestItemIndex(content, { 112, 112 }, 4) == 0,
          "nearest grid index resolves first slot");
    const POINT snapped = GridLayoutService::SnapPointToDesktopGrid({ 347, 283 });
    Check(snapped.x != 347 || snapped.y != 283, "desktop ghost point is forced onto a grid");
    const RECT dragSourceIcon{ 100, 100, 150, 150 };
    POINT desktopTopLeft = GridLayoutService::GetDraggedDesktopIconTopLeft(
        { 125, 125 }, dragSourceIcon, { 400, 300 }, 40);
    Check(desktopTopLeft.x == 380 && desktopTopLeft.y == 280,
          "fence-to-desktop drag preserves the icon grab offset");
    POINT labelDragTopLeft = GridLayoutService::GetDraggedDesktopIconTopLeft(
        { 125, 170 }, dragSourceIcon, { 400, 300 }, 40);
    Check(labelDragTopLeft.x == 380 && labelDragTopLeft.y == 244,
          "dragging from a label keeps the cursor below the desktop icon");
    POINT negativeScreenTopLeft = GridLayoutService::GetDraggedDesktopIconTopLeft(
        { -1930, 225 }, { -1950, 200, -1900, 250 }, { -1500, 520 }, 40);
    Check(negativeScreenTopLeft.x == -1516 && negativeScreenTopLeft.y == 500,
          "fence-to-desktop drag offset works on negative-coordinate monitors");

    // 关键修复：即使设置了 hasPersistedOffset 和自由坐标，也必须强制对齐到网格
    DesktopItem freelyPlaced;
    freelyPlaced.name = L"free.txt";
    freelyPlaced.relativeOffset = { 137, 73 };
    freelyPlaced.hasPersistedOffset = true;
    std::vector<DesktopItem> freeItems{ freelyPlaced };
    GridLayoutService::LayoutItems({ 500, 300, 1000, 800 }, freeItems);
    // 期望：第一个项被放到网格起点 (510, 310)，而不是 (637, 373)
    Check(freeItems[0].hitRect.left == 510 && freeItems[0].hitRect.top == 310,
          "persisted offset is ignored - item snaps to grid start");
    Check(!freeItems[0].hasPersistedOffset,
          "hasPersistedOffset is cleared after layout to enforce grid alignment");

    // 验证两个文件也必须整齐排列（不错位）
    DesktopItem itemA, itemB;
    itemA.name = L"a.txt";
    itemB.name = L"b.txt";
    itemB.relativeOffset = { 200, 150 }; // 试图错位
    itemB.hasPersistedOffset = true;
    std::vector<DesktopItem> twoItems{ itemA, itemB };
    GridLayoutService::LayoutItems({ 500, 300, 1000, 800 }, twoItems);
    // 两个项应该在第一行相邻列，y 坐标相同
    Check(twoItems[0].hitRect.top == twoItems[1].hitRect.top,
          "two items share the same row - no vertical misalignment");
    const int enlargedCellWidth = twoItems[1].hitRect.left - twoItems[0].hitRect.left;
    Check(enlargedCellWidth >= 96 &&
          twoItems[0].hitRect.right - twoItems[0].hitRect.left == enlargedCellWidth,
          "desktop icons use evenly distributed cells");
    std::vector<DesktopItem> fullRowItems(8);
    GridLayoutService::LayoutItems({ 500, 300, 1000, 800 }, fullRowItems);
    int firstRowRight = 0;
    for (const auto& item : fullRowItems) {
        if (item.hitRect.top != fullRowItems.front().hitRect.top) break;
        firstRowRight = item.hitRect.right;
    }
    Check(firstRowRight == 990,
          "adaptive icon row fills the fence content width to the right padding");

    // 验证重排序功能（不涉及坐标，只改变顺序）
    std::vector<DesktopItem> overflowItems(5);
    RECT narrowContent{ 100, 140, 280, 300 };
    const int overflowHeight = GridLayoutService::GetContentHeight(
        narrowContent, overflowItems, FenceViewMode::Icon);
    Check(overflowHeight > narrowContent.bottom - narrowContent.top,
          "narrow fence reports overflowing icon content");
    GridLayoutService::LayoutByMode(
        narrowContent, overflowItems, FenceViewMode::Icon, 0);
    const int firstItemTop = overflowItems[0].hitRect.top;
    GridLayoutService::LayoutByMode(
        narrowContent, overflowItems, FenceViewMode::Icon, 96);
    Check(overflowItems[0].hitRect.top == firstItemTop - 96,
          "mouse wheel offset scrolls fence items as one viewport");

    std::vector<DesktopItem> listItems(2);
    GridLayoutService::LayoutItemsList({ 100, 100, 500, 300 }, listItems);
    Check(listItems[0].iconRect.right - listItems[0].iconRect.left == 20,
          "list mode uses the enlarged twenty pixel icon");
    Check(listItems[1].hitRect.top - listItems[0].hitRect.top == 34,
          "list mode uses the enlarged thirty-four pixel row height");

    std::vector<DesktopItem> reorderItems;
    DesktopItem r1; r1.name = L"first.txt";
    DesktopItem r2; r2.name = L"second.txt";
    DesktopItem r3; r3.name = L"third.txt";
    reorderItems = { r1, r2, r3 };
    Check(GridLayoutService::ReorderItemToIndex(reorderItems, L"third.txt", 0),
          "reorder moves item to target index");
    Check(reorderItems[0].name == L"third.txt" && reorderItems[1].name == L"first.txt",
          "reorder result is correct - only order changes, no coordinates");

    MonitorService::MonitorSnapshot secondary;
    secondary.stableId = L"secondary-device";
    secondary.gdiDeviceName = L"\\\\.\\DISPLAY2";
    secondary.monitorRect = { 1920, 0, 4480, 1440 };
    secondary.workArea = { 1920, 0, 4480, 1400 };
    secondary.dpiX = 144;
    secondary.dpiY = 144;
    RECT secondaryFence{ 2070, 150, 2520, 510 };
    const auto monitorPlacement = MonitorService::CapturePlacement96(
        secondaryFence, 360, secondary);
    int restoredExpandedHeight = 0;
    const RECT restoredSecondaryFence = MonitorService::MaterializePlacement96(
        monitorPlacement, secondary, &restoredExpandedHeight);
    Check(EqualRect(&secondaryFence, &restoredSecondaryFence) &&
          restoredExpandedHeight == 360,
          "monitor-relative placement round-trips on a mixed-DPI secondary display");
    MonitorService::DisplayTopologySnapshot secondaryTopology;
    secondaryTopology.virtualScreen = { 0, 0, 4480, 1440 };
    secondaryTopology.monitors.push_back(secondary);
    secondaryTopology.signature =
        MonitorService::BuildTopologySignature(secondaryTopology);
    auto renumberedSecondary = secondaryTopology;
    renumberedSecondary.monitors[0].gdiDeviceName = L"\\\\.\\DISPLAY7";
    Check(MonitorService::BuildTopologySignature(renumberedSecondary) ==
              secondaryTopology.signature,
          "GDI display renumbering does not create a false topology change");
    auto transientIdentitySecondary = secondaryTopology;
    transientIdentitySecondary.monitors[0].stableId =
        L"gdi-temporary-display2";
    Check(MonitorService::BuildTopologySignature(
              transientIdentitySecondary) == secondaryTopology.signature,
          "transient CCD/GDI identity changes do not rebuild the layout");
    Check(MonitorService::FindMonitorById(
              secondaryTopology, L"secondary-device") != nullptr &&
          MonitorService::FindMonitorById(
              secondaryTopology, L"offline-device") == nullptr,
          "offline monitor identity is not substituted with the primary display");

    const fs::path iconDesktopRoot = root / "desktop-icon-layout";
    fs::create_directories(iconDesktopRoot, ec);
    const fs::path primaryIcon = iconDesktopRoot / "Primary.txt";
    const fs::path secondaryIcon = iconDesktopRoot / "Secondary.txt";
    WriteText(primaryIcon, "primary");
    WriteText(secondaryIcon, "secondary");
    const fs::path iconStateRoot = root / "desktop-icon-state";
    SetEnvironmentVariableW(
        L"DESKTODO_DATA_ROOT", iconStateRoot.c_str());
    SetEnvironmentVariableW(
        L"DESKTODO_DESKTOP_ROOT", iconDesktopRoot.c_str());

    MonitorService::MonitorSnapshot primary;
    primary.stableId = L"primary-device";
    primary.gdiDeviceName = L"\\\\.\\DISPLAY1";
    primary.monitorRect = { 0, 0, 1920, 1080 };
    primary.workArea = { 0, 0, 1920, 1040 };
    primary.primary = true;
    MonitorService::DisplayTopologySnapshot dualTopology;
    dualTopology.virtualScreen = { 0, 0, 4480, 1440 };
    dualTopology.monitors = { primary, secondary };
    dualTopology.signature =
        MonitorService::BuildTopologySignature(dualTopology);

    DesktopIconLayoutService iconLayout;
    Check(iconLayout.Load(), "missing desktop icon snapshot starts cleanly");
    Check(iconLayout.UpdateFromPositions(
              dualTopology,
              { { primaryIcon.wstring(), POINT{ 120, 160 } },
                { secondaryIcon.wstring(), POINT{ 2070, 150 } } }) &&
          iconLayout.Anchors().size() == 2,
          "desktop files capture stable monitor-relative icon anchors");

    MonitorService::DisplayTopologySnapshot primaryOnly;
    primaryOnly.virtualScreen = primary.monitorRect;
    primaryOnly.monitors = { primary };
    primaryOnly.signature =
        MonitorService::BuildTopologySignature(primaryOnly);
    POINT offlinePoint{};
    const auto secondaryAnchor = std::find_if(
        iconLayout.Anchors().begin(), iconLayout.Anchors().end(),
        [&](const DesktopIconAnchor& anchor) {
            return _wcsicmp(
                anchor.path.c_str(), secondaryIcon.c_str()) == 0;
        });
    Check(secondaryAnchor != iconLayout.Anchors().end() &&
          !DesktopIconLayoutService::ResolveOnlinePoint(
              *secondaryAnchor, primaryOnly, offlinePoint),
          "a disconnected monitor icon is never projected onto primary");
    Check(iconLayout.UpdateFromPositions(
              primaryOnly,
              { { primaryIcon.wstring(), POINT{ 180, 220 } },
                { secondaryIcon.wstring(), POINT{ 420, 300 } } }),
          "temporary primary projections can be evaluated safely");
    const auto projectedSecondaryAnchor = std::find_if(
        iconLayout.Anchors().begin(), iconLayout.Anchors().end(),
        [&](const DesktopIconAnchor& anchor) {
            return _wcsicmp(
                anchor.path.c_str(), secondaryIcon.c_str()) == 0;
        });
    Check(projectedSecondaryAnchor != iconLayout.Anchors().end() &&
          projectedSecondaryAnchor->placement.valid &&
          projectedSecondaryAnchor->placement.monitorId ==
              L"secondary-device",
          "Explorer's temporary primary projection does not overwrite an offline secondary anchor");

    DesktopIconLayoutService reloadedIconLayout;
    Check(reloadedIconLayout.Load() &&
          reloadedIconLayout.Anchors().size() == 2,
          "desktop icon anchors persist atomically across restarts");
    const auto reloadedSecondaryAnchor = std::find_if(
        reloadedIconLayout.Anchors().begin(),
        reloadedIconLayout.Anchors().end(),
        [&](const DesktopIconAnchor& anchor) {
            return _wcsicmp(
                anchor.path.c_str(), secondaryIcon.c_str()) == 0;
        });
    POINT reconnectedIconPoint{};
    Check(reloadedSecondaryAnchor !=
              reloadedIconLayout.Anchors().end() &&
          DesktopIconLayoutService::ResolveOnlinePoint(
              *reloadedSecondaryAnchor, dualTopology,
              reconnectedIconPoint) &&
          reconnectedIconPoint.x == 2070 &&
          reconnectedIconPoint.y == 150,
          "reconnected mixed-DPI monitor restores the saved icon position");
    SetEnvironmentVariableW(L"DESKTODO_DESKTOP_ROOT", nullptr);
    SetEnvironmentVariableW(L"DESKTODO_DATA_ROOT", nullptr);

    std::vector<DesktopItem> gameItems(1);
    gameItems[0].name = L"Game";
    GridLayoutService::LayoutGameLauncher({ 100, 100, 460, 340 }, gameItems);
    Check(gameItems[0].hitRect.left == 108 &&
          gameItems[0].hitRect.right == 452 &&
          gameItems[0].iconRect.right > gameItems[0].iconRect.left,
          "game container lays out one large launch card");

    Check(SNAP_THRESHOLD == 10, "snap threshold is ten physical pixels");

    PomodoroTimerState timer;
    Check(timer.remainingSeconds == PomodoroTimer::FocusSeconds && timer.focusPhase,
          "pomodoro starts with a twenty-five minute focus session");
    PomodoroTimer::Tick(timer);
    Check(timer.remainingSeconds == PomodoroTimer::FocusSeconds,
          "paused pomodoro does not count down");
    timer.running = true;
    timer.remainingSeconds = 1;
    Check(PomodoroTimer::Tick(timer) && !timer.focusPhase &&
          timer.remainingSeconds == PomodoroTimer::BreakSeconds &&
          timer.completedFocusSessions == 1,
          "completed focus session switches to a five-minute break");
    PomodoroTimer::Reset(timer);
    Check(!timer.running && timer.remainingSeconds == PomodoroTimer::BreakSeconds,
          "pomodoro reset restores the current phase duration");

    std::vector<TodoItem> todos;
    Check(!TodoList::Add(todos, L"   "), "todo list rejects an empty task");
    Check(TodoList::Add(todos, L"  Review layout  ") &&
          todos[0].text == L"Review layout", "todo list trims and adds a task");
    Check(TodoList::Toggle(todos, 0) && TodoList::Remaining(todos) == 0,
          "todo item can be marked complete");
    Check(TodoList::Update(todos, 0, L"  Review the complete long task text  ") &&
          todos[0].text == L"Review the complete long task text",
          "todo item text can be edited and trimmed");
    Check(!TodoList::Update(todos, 0, L"   ") &&
          todos[0].text == L"Review the complete long task text",
          "todo edit rejects empty text without losing the task");
    Check(TodoList::Remove(todos, 0) && todos.empty(),
          "todo item can be deleted");
    Check(TodoList::Add(todos, L"Normal", TodoPriority::Normal, L"2026-08-03") &&
          TodoList::Add(todos, L"Urgent", TodoPriority::High, L"2026-07-29") &&
          TodoList::Add(todos, L"Someday", TodoPriority::Low),
          "todo items accept priority and optional due dates");
    TodoList::Sort(todos, TodoSortMode::Priority);
    Check(todos.front().text == L"Urgent" &&
          TodoList::IsValidDueDate(todos.front().dueDate),
          "todo priority sorting keeps the high-priority task first");
    TodoList::Sort(todos, TodoSortMode::DueDate);
    Check(todos.front().dueDate == L"2026-07-29" &&
          !TodoList::IsValidDueDate(L"2026-02-30"),
          "todo due-date sorting and calendar validation are deterministic");

    const RECT capture = CaptureRegion::Normalize({ 400, 300 }, { 100, 80 });
    Check(capture.left == 100 && capture.top == 80 &&
          capture.right == 400 && capture.bottom == 300,
          "screenshot selection normalizes reverse dragging");
    Check(CaptureRegion::IsValid(capture), "screenshot selection accepts a visible region");
    const SIZE preview = CaptureRegion::FitPreview({ 0, 0, 1600, 1200 });
    Check(preview.cx == 800 && preview.cy == 600,
          "pinned screenshot preview preserves aspect ratio");
    const CaptureToolbarLayout toolbar = CaptureToolbar::Build(
        { 1700, 900, 1910, 1070 }, { 0, 0, 1920, 1080 });
    Check(toolbar.bounds.bottom <= 1080 && toolbar.bounds.top < 900,
          "capture toolbar flips above a selection near the screen bottom");
    Check(toolbar.bounds.left >= 0 && toolbar.bounds.right <= 1920,
          "capture toolbar remains inside the physical screen bounds");
    Check(toolbar.buttons.size() == 12,
          "capture toolbar contains only the twelve non-recording actions");
    Check(CaptureToolbar::HitTest(toolbar,
          { toolbar.buttons[0].left + 2, toolbar.buttons[0].top + 2 }) ==
          CaptureAction::Cancel, "capture toolbar exposes the cancel action");
    Check(CaptureToolbar::HitTest(toolbar,
          { toolbar.buttons[4].left + 2, toolbar.buttons[4].top + 2 }) ==
          CaptureAction::Ocr, "capture toolbar exposes the OCR action");
    Check(CaptureToolbar::HitTest(toolbar,
          { toolbar.buttons[5].left + 2, toolbar.buttons[5].top + 2 }) ==
          CaptureAction::LatexOcr, "capture toolbar exposes the LaTeX OCR action");
    Check(CaptureToolbar::HitTest(toolbar,
          { toolbar.buttons[6].left + 2, toolbar.buttons[6].top + 2 }) ==
          CaptureAction::Move, "capture toolbar exposes the move action");
    Check(CaptureToolbar::HitTest(toolbar,
          { toolbar.buttons[10].left + 2, toolbar.buttons[10].top + 2 }) ==
          CaptureAction::Rectangle, "capture toolbar exposes the rectangle action");
    Check(CaptureToolbar::HitTest(toolbar,
          { toolbar.buttons[11].left + 2, toolbar.buttons[11].top + 2 }) ==
          CaptureAction::Clear, "capture toolbar exposes clear without a recording action");
    const CapturePropertyLayout textProperties = CaptureToolbar::BuildProperty(
        toolbar, { 0, 0, 1920, 1080 }, true);
    Check(textProperties.bounds.left >= 0 && textProperties.bounds.right <= 1920 &&
          textProperties.bounds.top >= 0 && textProperties.bounds.bottom <= 1080,
          "capture text property panel remains inside the physical screen bounds");
    Check(PtInRect(&textProperties.fontButton,
                   { textProperties.fontButton.left + 2,
                     textProperties.fontButton.top + 2 }) &&
          PtInRect(&textProperties.colorButtons.back(),
                   { textProperties.colorButtons.back().left + 2,
                     textProperties.colorButtons.back().top + 2 }),
          "capture property panel exposes font and color controls");
    Check(!ShellIntegrationService::IsProtectedDesktopItem(
          L"C:\\Users\\test\\Desktop\\DeskTodo.lnk"),
          "DeskTodo desktop item can be collected explicitly");
    Check(!ShellIntegrationService::IsProtectedDesktopItem(
          L"C:\\Users\\test\\Desktop\\Other.lnk"),
          "other desktop shortcuts remain eligible for collection");

    BlackHole::Settings unsafeBlackHoleSettings;
    unsafeBlackHoleSettings.renderScale = 8.0f;
    unsafeBlackHoleSettings.fovScale = 10.0f;
    unsafeBlackHoleSettings.adiskHeight = 5.0f;
    unsafeBlackHoleSettings.bloomIterations = 20;
    unsafeBlackHoleSettings.gamma = 0.5f;
    const BlackHole::Settings safeBlackHoleSettings =
        BlackHole::Settings::Sanitize(unsafeBlackHoleSettings);
    Check(safeBlackHoleSettings.renderScale == 1.0f &&
          safeBlackHoleSettings.fovScale == 4.0f &&
          safeBlackHoleSettings.adiskHeight == 1.0f &&
          safeBlackHoleSettings.bloomIterations == 8 &&
          safeBlackHoleSettings.gamma == 1.0f,
          "black hole settings are clamped to safe rendering limits");

    auto fakeBlackHoleBackend = std::make_unique<FakeBlackHoleBackend>();
    FakeBlackHoleBackend* fakeBlackHoleBackendView = fakeBlackHoleBackend.get();
    BlackHole::Component blackHole(std::move(fakeBlackHoleBackend));
    BlackHole::SurfaceDescriptor blackHoleSurface;
    blackHoleSurface.device = reinterpret_cast<void*>(1);
    blackHoleSurface.target = reinterpret_cast<void*>(2);
    blackHoleSurface.width = 640;
    blackHoleSurface.height = 360;
    Check(blackHole.Initialize(blackHoleSurface) && fakeBlackHoleBackendView->initialized,
          "black hole component initializes with a bounded host surface");
    blackHole.UpdateSettings(unsafeBlackHoleSettings);
    BlackHole::FrameContext blackHoleFrame;
    BlackHole::FrameStats blackHoleStats;
    Check(blackHole.RenderFrame(blackHoleFrame, blackHoleStats) &&
          fakeBlackHoleBackendView->renderCount == 1 &&
          fakeBlackHoleBackendView->lastSettings.renderScale == 1.0f,
          "black hole component sends sanitized settings to its backend");
    blackHoleFrame.hostVisible = false;
    Check(blackHole.RenderFrame(blackHoleFrame, blackHoleStats) &&
          fakeBlackHoleBackendView->suspended,
          "hidden black hole component suspends without rendering");
    blackHoleFrame.hostVisible = true;
    blackHoleFrame.onBatterySaver = true;
    Check(blackHole.RenderFrame(blackHoleFrame, blackHoleStats) &&
          !fakeBlackHoleBackendView->lastSettings.bloom &&
          fakeBlackHoleBackendView->lastSettings.renderScale <= 0.5f,
          "battery saver disables bloom and reduces black hole resolution");
    blackHole.NotifyDeviceLost();
    Check(!blackHole.RenderFrame(blackHoleFrame, blackHoleStats),
          "device loss blocks rendering until reinitialization");
    blackHole.Shutdown();
    Check(fakeBlackHoleBackendView->shutdown &&
          blackHole.State() == BlackHole::ComponentState::Stopped,
          "black hole component releases its backend on shutdown");

    const RECT secondaryWorkArea{ -1600, 0, 0, 900 };
    const auto secondarySettings = SettingsWindowLayout::FitToWorkArea(
        secondaryWorkArea, { -800, 450 });
    Check(secondarySettings.rect.left >= secondaryWorkArea.left &&
          secondarySettings.rect.right <= secondaryWorkArea.right &&
          secondarySettings.rect.top >= secondaryWorkArea.top &&
          secondarySettings.rect.bottom <= secondaryWorkArea.bottom,
          "settings window remains fully visible on a negative-coordinate monitor");
    const RECT compactWorkArea{ 1920, -480, 2560, 0 };
    const auto compactSettings = SettingsWindowLayout::FitToWorkArea(
        compactWorkArea, { 2500, -40 });
    Check(compactSettings.scale < 1.0f &&
          compactSettings.rect.left >= compactWorkArea.left &&
          compactSettings.rect.right <= compactWorkArea.right &&
          compactSettings.rect.top >= compactWorkArea.top &&
          compactSettings.rect.bottom <= compactWorkArea.bottom,
          "settings controls scale down to fit a smaller secondary monitor");
    SettingsMonitorCheck attachedMonitors;
    EnumDisplayMonitors(nullptr, nullptr, CheckSettingsOnMonitor,
                        reinterpret_cast<LPARAM>(&attachedMonitors));
    Check(attachedMonitors.monitorCount >= 1 && attachedMonitors.allFit,
          "settings window fits every attached monitor work area");

    Fence movingFence;
    movingFence.id = L"moving";
    movingFence.rect = { 0, 0, 100, 100 };
    Fence targetFence;
    targetFence.id = L"target";
    targetFence.rect = { 109, 0, 209, 100 };
    // A reconnect can leave the persisted/runtime online flag stale even
    // though both rectangles are visibly on the recovered monitor. Snapping
    // must use the immutable interaction topology instead of that flag.
    targetFence.monitorOnline = false;
    MonitorService::MonitorSnapshot snapPrimary;
    snapPrimary.stableId = L"snap-primary";
    snapPrimary.monitorRect = { -1000, -1000, 1000, 1000 };
    snapPrimary.workArea = snapPrimary.monitorRect;
    snapPrimary.primary = true;
    MonitorService::MonitorSnapshot snapSecondary;
    snapSecondary.stableId = L"snap-secondary";
    snapSecondary.monitorRect = { 1000, -1000, 2000, 1000 };
    snapSecondary.workArea = snapSecondary.monitorRect;
    MonitorService::DisplayTopologySnapshot snapTopology;
    snapTopology.virtualScreen = { -1000, -1000, 2000, 1000 };
    snapTopology.monitors = { snapPrimary, snapSecondary };
    snapTopology.signature =
        MonitorService::BuildTopologySignature(snapTopology);
    SnapManager snapManager;
    const SnapResult snapResult = snapManager.ApplySnap(
        movingFence.id, movingFence.rect, { movingFence, targetFence },
        snapTopology);
    Check(snapResult.snappedX && !snapResult.lines.empty(),
          "visible reconnect fence snaps even when monitorOnline is stale");
    if (!snapResult.lines.empty()) {
        const auto& line = snapResult.lines.front();
        Check(line.start.y >= 0 && line.end.y <= 100,
              "fence snap guide stays within the two participating fences");
    }
    const SnapResult desktopOnlySnap = snapManager.ApplySnap(
        movingFence.id, movingFence.rect, { movingFence },
        snapTopology);
    Check(!desktopOnlySnap.snappedX && !desktopOnlySnap.snappedY &&
              desktopOnlySnap.lines.empty(),
          "desktop edges do not create magnetic snap candidates");

    Fence boundaryMoving;
    boundaryMoving.id = L"boundary-moving";
    boundaryMoving.rect = { 891, 0, 991, 100 };
    Fence otherMonitorTarget;
    otherMonitorTarget.id = L"other-monitor-target";
    otherMonitorTarget.rect = { 1000, 0, 1100, 100 };
    const SnapResult crossMonitorSnap = snapManager.ApplySnap(
        boundaryMoving.id, boundaryMoving.rect,
        { boundaryMoving, otherMonitorTarget }, snapTopology);
    Check(!crossMonitorSnap.snappedX && !crossMonitorSnap.snappedY &&
              crossMonitorSnap.lines.empty(),
          "fence snapping never crosses monitor boundaries");

    fs::remove_all(root, ec);
    std::cout << "DeskTodo.Next native checks: " << checks
              << ", failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
