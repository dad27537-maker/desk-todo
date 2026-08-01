#include <Windows.h>

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
    fence.itemOffsets = { POINT{ 10, 12 }, POINT{ 94, 12 }, POINT{ 178, 108 } };
    const std::wstring json = JsonSerialize({ fence });
    std::vector<Fence> restored;
    int configVersion = 0;
    Check(JsonDeserialize(json, restored, &configVersion), "serialized layout can be parsed");
    Check(configVersion == 7, "logical membership layout uses config version seven");
    Check(restored.size() == 1, "one fence is restored");
    Check(restored.size() == 1 && restored[0].itemOrder == fence.itemOrder,
          "item grid order survives round trip");
    Check(restored.size() == 1 && restored[0].memberPaths == fence.memberPaths,
          "real logical member paths survive round trip");
    Check(restored.size() == 1 && restored[0].itemDesktopOrigins.size() == 3 &&
          restored[0].itemDesktopOrigins[1].x == 360 &&
          restored[0].itemDesktopOrigins[2].x == LONG_MIN,
          "original physical desktop positions survive round trip");
    // 新行为：itemOffsets 不再持久化（只保存顺序），验证序列化后为空
    Check(restored.size() == 1 && restored[0].itemOffsets.empty(),
          "item offsets are no longer persisted - only order is saved");
    Check(restored.size() == 1 && restored[0].rect.left == 10 && restored[0].rect.top == 20,
          "fence coordinates survive round trip");
    Check(json.find(L"\"coordinateSpace\": \"physicalPixels\"") != std::wstring::npos,
          "configuration declares physical pixel coordinates");

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

    RECT disconnected{ 500000, 500000, 500300, 500240 };
    Check(MonitorService::CenterFenceInPrimaryWorkArea(disconnected),
          "disconnected monitor fence is repaired");
    const RECT primaryWorkArea = MonitorService::GetPrimaryWorkArea();
    Check(MonitorService::IsFenceInAnyWorkingArea(disconnected),
          "repaired fence is inside a working area");
    Check((disconnected.left + disconnected.right) / 2 ==
              (primaryWorkArea.left + primaryWorkArea.right) / 2 &&
          (disconnected.top + disconnected.bottom) / 2 ==
              (primaryWorkArea.top + primaryWorkArea.bottom) / 2,
          "disconnected fence is centered on primary work area");

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
    SnapManager snapManager;
    const SnapResult snapResult = snapManager.ApplySnap(
        movingFence.id, movingFence.rect, { movingFence, targetFence },
        { -1000, -1000, 2000, 2000 });
    Check(snapResult.snappedX && !snapResult.lines.empty(),
          "fence edges under ten pixels produce a snap line");

    fs::remove_all(root, ec);
    std::cout << "DeskTodo.Next native checks: " << checks
              << ", failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
