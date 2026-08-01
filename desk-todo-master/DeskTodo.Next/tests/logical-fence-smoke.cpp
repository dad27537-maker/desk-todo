#include "Managers/FenceManager.h"
#include "Services/DesktopPathService.h"
#include "Services/FileService.h"
#include "Services/FileMoveService.h"

#include <Windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>

int wmain() {
    namespace fs = std::filesystem;
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
    manager.Shutdown();
    FenceManager reloaded;
    const bool reloadInitialized = reloaded.Initialize(GetModuleHandleW(nullptr));
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
    CoUninitialize();
    CloseHandle(lock);
    fs::remove_all(root, error);

    const bool ok = hiddenWhileLocked && hiddenMemberStillDescribed && shownWhileLocked &&
        added && stayedInPlaceAfterAdd && stayedVisibleInDesktopFolder && grouped &&
        duplicateAddIsIdempotent && lockedMembershipCount == 1 && movedBetweenFences &&
        movedMembershipOnly && externalAdded && externalStayedInPlace &&
        externalFullPathVisible && twinsAdded && twinOriginsSet && twinOriginsDistinct &&
        publicDesktopAdded && reloadInitialized && persistedLocked && persistedExternal &&
        persistedTwinOrigins && persistedPublic &&
        removed && stayedInPlaceAfterRemove && mappedRejectsLogical &&
        mappedStillMovesPhysically && oneSourceAfterOrganize &&
        releasedOnExit && shortcutCount == 0;
    std::wcout << L"logical fence locked-file smoke: " << (ok ? L"PASS" : L"FAIL")
               << L", desktop-visible=" << stayedVisibleInDesktopFolder
               << L", idempotent=" << (lockedMembershipCount == 1)
               << L", external-path=" << externalFullPathVisible
               << L", same-name-origins=" << persistedTwinOrigins
               << L", public-desktop=" << persistedPublic
               << L", persisted=" << (persistedLocked && persistedExternal)
               << L", mapped-move=" << mappedStillMovesPhysically
               << L", logical-organize=" << oneSourceAfterOrganize
               << L", exit-restored=" << releasedOnExit
               << L", shortcuts=" << shortcutCount << std::endl;
    return ok ? 0 : 20;
}
