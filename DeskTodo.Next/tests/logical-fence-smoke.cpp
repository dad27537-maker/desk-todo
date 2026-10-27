#include "Managers/FenceManager.h"
#include "Services/DesktopPathService.h"
#include "Services/FileService.h"
#include "Services/FileMoveService.h"
#include "Services/ConfigService.h"
#include "Services/ShellIntegrationService.h"

#include <Windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    namespace fs = std::filesystem;
    if (argc == 3 &&
        wcscmp(argv[1], L"--repair-config-copy") == 0) {
        const fs::path probeRoot =
            fs::temp_directory_path() /
            L"DeskTodoRealConfigRepairProbe";
        std::error_code probeError;
        fs::remove_all(probeRoot, probeError);
        fs::create_directories(probeRoot, probeError);
        if (probeError) return 30;
        fs::copy_file(
            argv[2], probeRoot / L"config.json",
            fs::copy_options::overwrite_existing,
            probeError);
        if (probeError) return 31;
        const std::wstring realDesktop =
            GetUserDesktopPath();
        const std::wstring realPublicDesktop =
            GetPublicDesktopPath();
        SetEnvironmentVariableW(
            L"DESKTODO_DATA_ROOT",
            probeRoot.c_str());
        SetEnvironmentVariableW(
            L"DESKTODO_DESKTOP_ROOT",
            realDesktop.c_str());
        SetEnvironmentVariableW(
            L"DESKTODO_PUBLIC_DESKTOP_ROOT",
            realPublicDesktop.c_str());
        CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED);
        FenceManager probeManager;
        if (!probeManager.Initialize(
                GetModuleHandleW(nullptr))) {
            CoUninitialize();
            return 32;
        }
        for (const auto& fence :
             probeManager.GetAllFencesRef()) {
            for (const auto& member :
                 fence.memberPaths) {
                std::wcout << member << L"\n";
            }
        }
        probeManager.Shutdown();
        CoUninitialize();
        return 0;
    }

    const fs::path root = fs::temp_directory_path() / L"DeskTodoLogicalFenceSmoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root, error);
    if (error) return 10;
    SetEnvironmentVariableW(L"DESKTODO_DATA_ROOT", (root / L"data").c_str());

    const fs::path desktop = root / L"Desktop";
    fs::create_directories(desktop, error);
    if (error) return 10;
    SetEnvironmentVariableW(L"DESKTODO_DESKTOP_ROOT", desktop.c_str());
    const fs::path publicDesktop = root / L"PublicDesktop";
    fs::create_directories(publicDesktop, error);
    SetEnvironmentVariableW(L"DESKTODO_PUBLIC_DESKTOP_ROOT", publicDesktop.c_str());

    const fs::path lockedFile = desktop / L"DeskTodo.Next.exe";
    { std::ofstream stream(lockedFile, std::ios::binary); stream << "locked executable"; }
    HANDLE lock = CreateFileW(lockedFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock == INVALID_HANDLE_VALUE) return 11;

    const bool hiddenWhileLocked =
        FileService::SetLogicalGroupingHidden(lockedFile.wstring(), true) &&
        (GetFileAttributesW(lockedFile.c_str()) &
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ==
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    DesktopItem hiddenDescription;
    const bool hiddenMemberStillDescribed =
        FileService::DescribePath(lockedFile.wstring(), hiddenDescription, true);
    const bool shownWhileLocked =
        FileService::SetLogicalGroupingHidden(lockedFile.wstring(), false) &&
        (GetFileAttributesW(lockedFile.c_str()) &
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    FenceManager manager;
    if (!manager.Initialize(GetModuleHandleW(nullptr))) return 12;
    auto fences = manager.GetAllFences();
    if (fences.size() < 2) return 13;

    const std::wstring sourceFence = fences[0].id;
    const std::wstring targetFence = fences[1].id;
    const fs::path legacyStoredFile =
        fs::path(fences[0].folderPath) / L"legacy-stored.txt";
    fs::create_directories(legacyStoredFile.parent_path(), error);
    {
        std::ofstream stream(legacyStoredFile, std::ios::binary);
        stream << "legacy physical fence item";
    }
    const bool added = manager.AddLogicalItem(sourceFence, lockedFile.wstring());
    const bool stayedInPlaceAfterAdd = fs::exists(lockedFile);
    const DWORD attributesAfterAdd = GetFileAttributesW(lockedFile.c_str());
    const bool stayedVisibleInDesktopFolder =
        attributesAfterAdd != INVALID_FILE_ATTRIBUTES &&
        (attributesAfterAdd & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0 &&
        lockedFile.parent_path() == desktop;
    const bool grouped = manager.IsLogicalItem(sourceFence, lockedFile.wstring());
    const bool duplicateAddIsIdempotent = manager.AddLogicalItem(
        sourceFence, (desktop / L"." / lockedFile.filename()).wstring());
    size_t lockedMembershipCount = 0;
    for (const auto& fence : manager.GetAllFences()) {
        for (const auto& member : fence.memberPaths) {
            if (_wcsicmp(NormalizePathIdentity(member).c_str(),
                         NormalizePathIdentity(lockedFile.wstring()).c_str()) == 0) {
                ++lockedMembershipCount;
            }
        }
    }
    const bool movedBetweenFences = manager.MoveLogicalItem(
        sourceFence, targetFence, lockedFile.wstring());
    const bool movedMembershipOnly = !manager.IsLogicalItem(sourceFence, lockedFile.wstring()) &&
        manager.IsLogicalItem(targetFence, lockedFile.wstring()) && fs::exists(lockedFile);

    const fs::path externalProject = root / L"Projects" / L"desk-todo-master";
    fs::create_directories(externalProject, error);
    { std::ofstream stream(externalProject / L"README.md", std::ios::binary);
      stream << "external project stays at its real path"; }
    const bool externalAdded = manager.AddLogicalItem(
        sourceFence, externalProject.wstring());
    const bool externalStayedInPlace = fs::exists(externalProject / L"README.md");
    bool externalFullPathVisible = false;
    if (const Fence* fence = manager.GetFenceById(sourceFence)) {
        for (const auto& item : fence->items) {
            if (_wcsicmp(NormalizePathIdentity(item.fullPath).c_str(),
                         NormalizePathIdentity(externalProject.wstring()).c_str()) == 0) {
                externalFullPathVisible = true;
                break;
            }
        }
    }

    const fs::path twinA = root / L"A" / L"same.txt";
    const fs::path twinB = root / L"B" / L"same.txt";
    fs::create_directories(twinA.parent_path(), error);
    fs::create_directories(twinB.parent_path(), error);
    { std::ofstream(twinA) << "a"; std::ofstream(twinB) << "b"; }
    const bool twinsAdded = manager.AddLogicalItem(sourceFence, twinA.wstring()) &&
        manager.AddLogicalItem(sourceFence, twinB.wstring());
    const POINT twinAOrigin{ 101, 202 };
    const POINT twinBOrigin{ 303, 404 };
    const bool twinOriginsSet = manager.SetItemDesktopOrigin(
        sourceFence, twinA.wstring(), twinAOrigin) &&
        manager.SetItemDesktopOrigin(sourceFence, twinB.wstring(), twinBOrigin);
    POINT readTwinA{}, readTwinB{};
    const bool twinOriginsDistinct = manager.GetItemDesktopOrigin(
        sourceFence, twinA.wstring(), readTwinA) &&
        manager.GetItemDesktopOrigin(sourceFence, twinB.wstring(), readTwinB) &&
        readTwinA.x == twinAOrigin.x && readTwinB.x == twinBOrigin.x;

    const fs::path publicItem = publicDesktop / L"Public.lnk";
    { std::ofstream(publicItem) << "public desktop shortcut"; }
    const bool publicDesktopAdded = manager.AddLogicalItem(
        sourceFence, publicItem.wstring()) && fs::exists(publicItem);

    // Config reload must retain normalized real paths without populating the
    // ordinary fence storage directory.
    manager.SaveConfig();
    auto stalePathFences = manager.GetAllFences();
    manager.Shutdown();
    const fs::path repairedDesktopItem = desktop / L"relocated.zip";
    { std::ofstream(repairedDesktopItem) << "relocated desktop item"; }
    const fs::path sharedShortcutTarget =
        root / L"Apps" / L"DeskTodo.exe";
    fs::create_directories(
        sharedShortcutTarget.parent_path(), error);
    { std::ofstream(sharedShortcutTarget) << "shortcut target"; }
    const fs::path externalShortcutDirectory =
        root / L"ExternalShortcuts";
    std::wstring externalShortcut;
    std::wstring desktopShortcut;
    const bool mirrorShortcutsCreated =
        ShellIntegrationService::CreateItemShortcut(
            sharedShortcutTarget.wstring(),
            externalShortcutDirectory.wstring(),
            &externalShortcut) &&
        ShellIntegrationService::CreateItemShortcut(
            sharedShortcutTarget.wstring(),
            desktop.wstring(),
            &desktopShortcut);
    for (auto& fence : stalePathFences) {
        if (fence.id == sourceFence) {
            fence.memberPaths.push_back(
                (root / L"OldDesktop" /
                 repairedDesktopItem.filename()).wstring());
            if (mirrorShortcutsCreated) {
                fence.memberPaths.push_back(
                    externalShortcut);
            }
            break;
        }
    }
    ConfigService stalePathConfig;
    stalePathConfig.SaveFences(stalePathFences);
    FenceManager reloaded;
    const bool reloadInitialized = reloaded.Initialize(GetModuleHandleW(nullptr));
    const bool staleDesktopPathRepaired =
        reloaded.IsLogicalItem(
            sourceFence, repairedDesktopItem.wstring());
    const bool shortcutMirrorCoalesced =
        mirrorShortcutsCreated &&
        reloaded.IsLogicalItem(
            sourceFence, desktopShortcut) &&
        !reloaded.IsLogicalItem(
            sourceFence, externalShortcut);
    const fs::path migratedLegacyFile =
        desktop / legacyStoredFile.filename();
    const bool legacyStoredItemMigrated =
        fs::exists(migratedLegacyFile) &&
        !fs::exists(legacyStoredFile) &&
        reloaded.IsLogicalItem(
            sourceFence, migratedLegacyFile.wstring()) &&
        (GetFileAttributesW(migratedLegacyFile.c_str()) &
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0;
    const bool persistedLocked = reloaded.IsLogicalItem(targetFence, lockedFile.wstring());
    const bool persistedExternal = reloaded.IsLogicalItem(sourceFence, externalProject.wstring());
    POINT persistedTwinA{}, persistedTwinB{};
    const bool persistedTwinOrigins = reloaded.GetItemDesktopOrigin(
        sourceFence, twinA.wstring(), persistedTwinA) &&
        reloaded.GetItemDesktopOrigin(sourceFence, twinB.wstring(), persistedTwinB) &&
        persistedTwinA.x == twinAOrigin.x && persistedTwinB.x == twinBOrigin.x;
    const bool persistedPublic = reloaded.IsLogicalItem(sourceFence, publicItem.wstring());
    const bool removed = reloaded.RemoveLogicalItem(targetFence, lockedFile.wstring());
    const bool stayedInPlaceAfterRemove = fs::exists(lockedFile);

    const fs::path mappedFolder = root / L"Mapped";
    fs::create_directories(mappedFolder, error);
    Fence* mappedFence = reloaded.CreateFenceAt(
        POINT{ 40, 40 }, mappedFolder.wstring(), mappedFolder.wstring(), true);
    const fs::path mappedSource = root / L"Inbox" / L"mapped.txt";
    fs::create_directories(mappedSource.parent_path(), error);
    { std::ofstream stream(mappedSource, std::ios::binary); stream << "mapped move"; }
    const bool mappedRejectsLogical = mappedFence &&
        !reloaded.AddLogicalItem(mappedFence->id, mappedSource.wstring());
    FileMoveService mappedMover;
    std::wstring mappedPath;
    const bool mappedStillMovesPhysically = mappedFence &&
        mappedMover.MoveToDirectory(mappedSource.wstring(), mappedFence->folderPath,
            false, &mappedPath) == FileMoveResult::Success &&
        !fs::exists(mappedSource) && fs::exists(mappedPath);

    const fs::path gameLauncher = desktop / L"SampleGame.exe";
    const fs::path secondLauncher = desktop / L"SecondGame.lnk";
    const fs::path unsupportedLauncher = desktop / L"notes.txt";
    { std::ofstream(gameLauncher) << "launcher";
      std::ofstream(secondLauncher) << "shortcut";
      std::ofstream(unsupportedLauncher) << "not a launcher"; }
    Fence* gameFence = reloaded.CreateGameFenceAt(POINT{ 160, 160 }, L"Games");
    const bool gameAcceptsOneLauncher = gameFence &&
        reloaded.AddLogicalItem(gameFence->id, gameLauncher.wstring()) &&
        fs::exists(gameLauncher);
    const bool gameRejectsUnsupportedAndSecond = gameFence &&
        !reloaded.AddLogicalItem(gameFence->id, unsupportedLauncher.wstring()) &&
        !reloaded.AddLogicalItem(gameFence->id, secondLauncher.wstring()) &&
        gameFence->kind == FenceKind::Game &&
        gameFence->memberPaths.size() == 1;

    Fence* topologyFence = reloaded.GetFenceById(sourceFence);
    if (topologyFence) {
        // Keep the synthetic fence rectangle consistent with its secondary
        // monitor placement so the disconnect sample cannot be mistaken for
        // a primary-monitor match.
        topologyFence->rect = { 2020, 80, 2340, 320 };
        topologyFence->monitorPlacement = {
            L"secondary-device", 100, 80, 320, 240, 240, true
        };
        topologyFence->monitorOnline = true;
    }
    const RECT beforeMonitorDisconnect =
        topologyFence ? topologyFence->rect : RECT{};
    MonitorService::MonitorSnapshot primaryMonitor;
    primaryMonitor.stableId = L"primary-device";
    primaryMonitor.gdiDeviceName = L"\\\\.\\DISPLAY1";
    primaryMonitor.monitorRect = { 0, 0, 1920, 1080 };
    primaryMonitor.workArea = { 0, 0, 1920, 1040 };
    primaryMonitor.primary = true;
    MonitorService::DisplayTopologySnapshot primaryOnlyTopology;
    primaryOnlyTopology.virtualScreen = primaryMonitor.monitorRect;
    primaryOnlyTopology.monitors = { primaryMonitor };
    primaryOnlyTopology.signature =
        MonitorService::BuildTopologySignature(primaryOnlyTopology);

    bool seededSecondaryItemOrigin = false;
    if (topologyFence) {
        const std::wstring twinAKey =
            L"@path:" + NormalizePathIdentity(twinA.wstring());
        for (size_t index = 0; index < topologyFence->itemOrder.size(); ++index) {
            if (_wcsicmp(topologyFence->itemOrder[index].c_str(),
                         twinAKey.c_str()) != 0) {
                continue;
            }
            if (topologyFence->itemDesktopOriginPlacements.size() <
                topologyFence->itemOrder.size()) {
                topologyFence->itemDesktopOriginPlacements.resize(
                    topologyFence->itemOrder.size());
            }
            topologyFence->itemDesktopOriginPlacements[index] = {
                L"secondary-device", 100, 80, true
            };
            seededSecondaryItemOrigin = true;
            break;
        }
    }
    POINT movedOfflineItemPoint{};
    const bool offlineAnchorSurvivesFenceMove =
        seededSecondaryItemOrigin &&
        reloaded.MoveLogicalItem(
            sourceFence, targetFence, twinA.wstring()) &&
        reloaded.ResolveItemDesktopOrigin(
            targetFence, twinA.wstring(), primaryOnlyTopology,
            movedOfflineItemPoint) == DesktopOriginResolution::Offline &&
        reloaded.MoveLogicalItem(
            targetFence, sourceFence, twinA.wstring()) &&
        reloaded.ResolveItemDesktopOrigin(
            sourceFence, twinA.wstring(), primaryOnlyTopology,
            movedOfflineItemPoint) == DesktopOriginResolution::Offline;
    POINT offlineItemPoint{};
    const bool offlineItemOriginIsDeferred = seededSecondaryItemOrigin &&
        reloaded.ResolveItemDesktopOrigin(
            sourceFence, twinA.wstring(), primaryOnlyTopology,
            offlineItemPoint) == DesktopOriginResolution::Offline;

    reloaded.ApplyDisplayTopology(primaryOnlyTopology);
    topologyFence = reloaded.GetFenceById(sourceFence);
    const bool disconnectPreservesAnchor = topologyFence &&
        !topologyFence->monitorOnline &&
        EqualRect(&topologyFence->rect, &beforeMonitorDisconnect) &&
        topologyFence->monitorPlacement.monitorId == L"secondary-device";

    MonitorService::MonitorSnapshot secondaryMonitor;
    secondaryMonitor.stableId = L"secondary-device";
    secondaryMonitor.gdiDeviceName = L"\\\\.\\DISPLAY2";
    secondaryMonitor.monitorRect = { 1920, 0, 4480, 1440 };
    secondaryMonitor.workArea = { 1920, 0, 4480, 1400 };
    secondaryMonitor.dpiX = 144;
    secondaryMonitor.dpiY = 144;
    MonitorService::DisplayTopologySnapshot reconnectedTopology;
    reconnectedTopology.virtualScreen = { 0, 0, 4480, 1440 };
    reconnectedTopology.monitors = { primaryMonitor, secondaryMonitor };
    reconnectedTopology.signature =
        MonitorService::BuildTopologySignature(reconnectedTopology);
    reloaded.ApplyDisplayTopology(reconnectedTopology);
    topologyFence = reloaded.GetFenceById(sourceFence);
    const RECT expectedReconnectedRect{ 2070, 120, 2550, 480 };
    POINT reconnectedItemPoint{};
    const bool reconnectRestoresItemOrigin =
        reloaded.ResolveItemDesktopOrigin(
            sourceFence, twinA.wstring(), reconnectedTopology,
            reconnectedItemPoint) == DesktopOriginResolution::Online &&
        reconnectedItemPoint.x == 2070 &&
        reconnectedItemPoint.y == 120;
    const bool reconnectRestoresRelativePlacement = topologyFence &&
        topologyFence->monitorOnline &&
        EqualRect(&topologyFence->rect, &expectedReconnectedRect) &&
        topologyFence->expandedHeight == 360 &&
        topologyFence->monitorPlacement.monitorId == L"secondary-device";

    // A USB-C reconnect may keep the same work area but expose a new CCD
    // target path.  Verify that an already-offline fence is recovered by
    // geometry and that the replacement identity is persisted in memory.
    MonitorService::MonitorSnapshot replacementSecondary = secondaryMonitor;
    replacementSecondary.stableId = L"secondary-device-reconnected";
    MonitorService::DisplayTopologySnapshot replacementTopology =
        reconnectedTopology;
    replacementTopology.monitors = { primaryMonitor, replacementSecondary };
    replacementTopology.signature =
        MonitorService::BuildTopologySignature(replacementTopology);
    bool replacementMonitorRecovered = false;
    topologyFence = reloaded.GetFenceById(sourceFence);
    if (topologyFence) {
        topologyFence->monitorPlacement.monitorId = L"stale-secondary-device";
        topologyFence->monitorOnline = false;
        reloaded.ApplyDisplayTopology(replacementTopology);
        topologyFence = reloaded.GetFenceById(sourceFence);
        replacementMonitorRecovered = topologyFence &&
            topologyFence->monitorOnline &&
            topologyFence->monitorPlacement.monitorId ==
                L"secondary-device-reconnected" &&
            EqualRect(&topologyFence->rect, &expectedReconnectedRect);
    }

    bool removedPrimaryAnchorPreserved = false;
    topologyFence = reloaded.GetFenceById(sourceFence);
    if (topologyFence) {
        const Fence savedFence = *topologyFence;
        topologyFence->rect = { 100, 80, 420, 320 };
        topologyFence->monitorPlacement = {
            L"removed-primary-device", 100, 80, 320, 240, 240, true
        };
        topologyFence->monitorOnline = true;
        const RECT removedPrimaryRect = topologyFence->rect;
        reloaded.ApplyDisplayTopology(primaryOnlyTopology);
        topologyFence = reloaded.GetFenceById(sourceFence);
        removedPrimaryAnchorPreserved = topologyFence &&
            !topologyFence->monitorOnline &&
            topologyFence->monitorPlacement.monitorId == L"removed-primary-device" &&
            EqualRect(&topologyFence->rect, &removedPrimaryRect);
        if (topologyFence) *topologyFence = savedFence;
    }

    const fs::path projectFolder = desktop / L"Project";
    fs::create_directories(projectFolder, error);
    { std::ofstream stream(projectFolder / L"visible.txt", std::ios::binary);
      stream << "visible through fence path"; }
    const int organizedCount = reloaded.AutoOrganizeDesktop();
    bool copiedIntoStorage = false;
    const fs::path storage = root / L"data" / L"Fences";
    if (fs::exists(storage)) {
        for (const auto& entry : fs::recursive_directory_iterator(storage, error)) {
            if (!error && entry.path().filename() == L"visible.txt") {
                copiedIntoStorage = true;
                break;
            }
        }
    }
    const bool oneSourceAfterOrganize = organizedCount >= 1 &&
        fs::exists(projectFolder / L"visible.txt") && !copiedIntoStorage;
    const bool releasedOnExit = reloaded.ReleaseFenceItemsToDesktop() &&
        fs::exists(projectFolder / L"visible.txt");

    // The temporary desktop override is only for the filesystem invariant.
    // Do not ask the real Explorer desktop view to add this synthetic path.
    SetEnvironmentVariableW(L"DESKTODO_DESKTOP_ROOT", nullptr);
    SetEnvironmentVariableW(L"DESKTODO_PUBLIC_DESKTOP_ROOT", nullptr);

    size_t shortcutCount = 0;
    if (fs::exists(storage)) {
        for (const auto& entry : fs::recursive_directory_iterator(storage, error)) {
            if (!error && _wcsicmp(entry.path().extension().c_str(), L".lnk") == 0) ++shortcutCount;
        }
    }

    reloaded.Shutdown();

    const fs::path hiddenMigrationRoot =
        root / L"legacy-hidden-migration";
    const fs::path hiddenMigrationDesktop =
        hiddenMigrationRoot / L"Desktop";
    const fs::path hiddenMigrationPublic =
        hiddenMigrationRoot / L"PublicDesktop";
    const fs::path hiddenMigrationData =
        hiddenMigrationRoot / L"data";
    fs::create_directories(hiddenMigrationDesktop, error);
    fs::create_directories(hiddenMigrationPublic, error);
    const fs::path legacyHiddenPath =
        hiddenMigrationDesktop / L"legacy-hidden.txt";
    {
        std::ofstream stream(legacyHiddenPath, std::ios::binary);
        stream << "legacy hidden logical member";
    }
    FileService::SetLogicalGroupingHidden(
        legacyHiddenPath.wstring(), true);
    SetEnvironmentVariableW(
        L"DESKTODO_DATA_ROOT", hiddenMigrationData.c_str());
    SetEnvironmentVariableW(
        L"DESKTODO_DESKTOP_ROOT", hiddenMigrationDesktop.c_str());
    SetEnvironmentVariableW(
        L"DESKTODO_PUBLIC_DESKTOP_ROOT", hiddenMigrationPublic.c_str());
    Fence legacyHiddenFence;
    legacyHiddenFence.id = L"legacy-hidden-fence";
    legacyHiddenFence.title = L"Legacy";
    legacyHiddenFence.rect = { 80, 80, 400, 320 };
    legacyHiddenFence.itemOrder = { legacyHiddenPath.filename().wstring() };
    legacyHiddenFence.memberPaths = { legacyHiddenPath.wstring() };
    ConfigService legacyHiddenConfig;
    legacyHiddenConfig.SaveFences({ legacyHiddenFence });
    const fs::path legacyHiddenConfigPath =
        hiddenMigrationData / L"config.json";
    std::ifstream legacyHiddenConfigInput(
        legacyHiddenConfigPath, std::ios::binary);
    std::string legacyHiddenConfigText(
        (std::istreambuf_iterator<char>(legacyHiddenConfigInput)),
        std::istreambuf_iterator<char>());
    legacyHiddenConfigInput.close();
    const std::string currentVersion = "\"version\": 9";
    const auto versionPosition =
        legacyHiddenConfigText.find(currentVersion);
    if (versionPosition != std::string::npos) {
        legacyHiddenConfigText.replace(
            versionPosition, currentVersion.size(), "\"version\": 7");
    }
    {
        std::ofstream stream(
            legacyHiddenConfigPath,
            std::ios::binary | std::ios::trunc);
        stream << legacyHiddenConfigText;
    }
    bool legacyHiddenMigrationInitialized = false;
    {
        FenceManager legacyHiddenManager;
        legacyHiddenMigrationInitialized =
            legacyHiddenManager.Initialize(GetModuleHandleW(nullptr));
        legacyHiddenManager.Shutdown();
    }
    const DWORD legacyHiddenAttributes =
        GetFileAttributesW(legacyHiddenPath.c_str());
    const bool legacyHiddenAttributesCleared =
        legacyHiddenMigrationInitialized &&
        legacyHiddenAttributes != INVALID_FILE_ATTRIBUTES &&
        (legacyHiddenAttributes &
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) == 0;
    SetEnvironmentVariableW(L"DESKTODO_DESKTOP_ROOT", nullptr);
    SetEnvironmentVariableW(L"DESKTODO_PUBLIC_DESKTOP_ROOT", nullptr);

    const fs::path corruptManagerRoot = root / L"corrupt-manager-config";
    fs::create_directories(corruptManagerRoot, error);
    const fs::path corruptManagerConfig =
        corruptManagerRoot / L"config.json";
    const std::string corruptManagerText =
        "{\"version\":9,\"fences\":[{\"id\":\"unfinished\"}";
    {
        std::ofstream stream(corruptManagerConfig, std::ios::binary);
        stream << corruptManagerText;
    }
    SetEnvironmentVariableW(
        L"DESKTODO_DATA_ROOT", corruptManagerRoot.c_str());
    bool corruptManagerRejected = false;
    {
        FenceManager corruptManager;
        corruptManagerRejected =
            !corruptManager.Initialize(GetModuleHandleW(nullptr));
    }
    std::ifstream corruptManagerStream(
        corruptManagerConfig, std::ios::binary);
    const std::string corruptManagerAfter(
        (std::istreambuf_iterator<char>(corruptManagerStream)),
        std::istreambuf_iterator<char>());
    const bool corruptManagerPreserved =
        corruptManagerRejected &&
        corruptManagerAfter == corruptManagerText;
    SetEnvironmentVariableW(L"DESKTODO_DATA_ROOT", nullptr);

    CoUninitialize();
    CloseHandle(lock);
    fs::remove_all(root, error);

    const bool ok = hiddenWhileLocked && hiddenMemberStillDescribed && shownWhileLocked &&
        added && stayedInPlaceAfterAdd && stayedVisibleInDesktopFolder && grouped &&
        duplicateAddIsIdempotent && lockedMembershipCount == 1 && movedBetweenFences &&
        movedMembershipOnly && externalAdded && externalStayedInPlace &&
        externalFullPathVisible && twinsAdded && twinOriginsSet && twinOriginsDistinct &&
        publicDesktopAdded && reloadInitialized && legacyStoredItemMigrated &&
        staleDesktopPathRepaired && shortcutMirrorCoalesced &&
        persistedLocked && persistedExternal &&
        persistedTwinOrigins && persistedPublic &&
        removed && stayedInPlaceAfterRemove && mappedRejectsLogical &&
        mappedStillMovesPhysically && gameAcceptsOneLauncher &&
        gameRejectsUnsupportedAndSecond && disconnectPreservesAnchor &&
        offlineItemOriginIsDeferred && offlineAnchorSurvivesFenceMove &&
        reconnectRestoresItemOrigin &&
        reconnectRestoresRelativePlacement && oneSourceAfterOrganize &&
        replacementMonitorRecovered && removedPrimaryAnchorPreserved &&
        releasedOnExit && legacyHiddenAttributesCleared &&
        corruptManagerPreserved && shortcutCount == 0;
    std::wcout << L"logical fence locked-file smoke: " << (ok ? L"PASS" : L"FAIL")
               << L", desktop-visible=" << stayedVisibleInDesktopFolder
               << L", idempotent=" << (lockedMembershipCount == 1)
               << L", external-path=" << externalFullPathVisible
               << L", same-name-origins=" << persistedTwinOrigins
               << L", public-desktop=" << persistedPublic
               << L", legacy-storage-migrated=" << legacyStoredItemMigrated
               << L", stale-path-repaired=" << staleDesktopPathRepaired
               << L", shortcut-coalesced=" << shortcutMirrorCoalesced
               << L", persisted=" << (persistedLocked && persistedExternal)
               << L", mapped-move=" << mappedStillMovesPhysically
               << L", game-launcher="
               << (gameAcceptsOneLauncher && gameRejectsUnsupportedAndSecond)
               << L", monitor-reconnect="
               << (disconnectPreservesAnchor &&
                   reconnectRestoresRelativePlacement &&
                   offlineItemOriginIsDeferred &&
                   offlineAnchorSurvivesFenceMove &&
                   reconnectRestoresItemOrigin)
               << L", monitor-id-recovery=" << replacementMonitorRecovered
               << L", removed-primary-anchor=" << removedPrimaryAnchorPreserved
               << L", logical-organize=" << oneSourceAfterOrganize
               << L", exit-restored=" << releasedOnExit
               << L", legacy-hidden-cleared=" << legacyHiddenAttributesCleared
               << L", corrupt-config-preserved=" << corruptManagerPreserved
               << L", shortcuts=" << shortcutCount << std::endl;
    return ok ? 0 : 20;
}
