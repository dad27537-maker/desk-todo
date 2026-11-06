#include "FenceManager.h"
#include "Services/ConfigService.h"
#include "Services/MonitorService.h"
#include "Services/FileService.h"
#include "Services/FenceStorageService.h"
#include "Services/GridLayoutService.h"
#include "Services/IconCache.h"
#include "Services/DesktopPathService.h"
#include "Services/DesktopShellService.h"
#include "Services/FileMoveService.h"
#include "Services/ShellIntegrationService.h"
#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <limits>
#include <shobjidl.h>
#include <ShlObj.h>
#include <unordered_map>

namespace {
std::wstring NormalizeMemberPath(const std::wstring& value) {
    return NormalizePathIdentity(value);
}

bool SameMemberPath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(NormalizeMemberPath(a).c_str(), NormalizeMemberPath(b).c_str()) == 0;
}

bool EnvironmentValueMatchesKnownFolder(const wchar_t* variable,
                                        REFKNOWNFOLDERID knownFolder) {
    wchar_t overridePath[32768]{};
    const DWORD length = GetEnvironmentVariableW(
        variable, overridePath, static_cast<DWORD>(std::size(overridePath)));
    if (length == 0 || length >= std::size(overridePath)) return false;

    PWSTR knownPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(knownFolder, 0, nullptr, &knownPath)) ||
        !knownPath) {
        return false;
    }
    const bool matches = _wcsicmp(
        NormalizePathIdentity(overridePath).c_str(),
        NormalizePathIdentity(knownPath).c_str()) == 0;
    CoTaskMemFree(knownPath);
    return matches;
}

bool UsesSyntheticDesktopForTests() {
    // A real deployment may intentionally set DESKTODO_DESKTOP_ROOT to the
    // user's redirected Desktop (for example C:\\Users\\lound\\Desktop).
    // The old test detected any override as synthetic and skipped all native
    // icon suppression, which caused every logical member to remain duplicated
    // on the desktop. Only an override that points away from both known shell
    // Desktop roots is a synthetic test root.
    const bool hasUserOverride =
        GetEnvironmentVariableW(L"DESKTODO_DESKTOP_ROOT", nullptr, 0) > 0;
    const bool hasPublicOverride =
        GetEnvironmentVariableW(L"DESKTODO_PUBLIC_DESKTOP_ROOT", nullptr, 0) > 0;
    if (!hasUserOverride && !hasPublicOverride) return false;

    // Keep an explicit opt-in for tests that use the real known Desktop path
    // while still needing to avoid touching Explorer's native view.
    wchar_t synthetic[32]{};
    const DWORD syntheticLength = GetEnvironmentVariableW(
        L"DESKTODO_SYNTHETIC_DESKTOP", synthetic,
        static_cast<DWORD>(std::size(synthetic)));
    if (syntheticLength > 0 &&
        (_wcsicmp(synthetic, L"1") == 0 ||
         _wcsicmp(synthetic, L"true") == 0)) {
        return true;
    }

    // If either override names the actual shell Desktop, this is a production
    // path override and native suppression must remain enabled.
    if (EnvironmentValueMatchesKnownFolder(
            L"DESKTODO_DESKTOP_ROOT", FOLDERID_Desktop) ||
        EnvironmentValueMatchesKnownFolder(
            L"DESKTODO_PUBLIC_DESKTOP_ROOT", FOLDERID_PublicDesktop)) {
        return false;
    }
    return true;
}

constexpr wchar_t kLogicalOrderPrefix[] = L"@path:";

std::wstring LogicalOrderKey(const std::wstring& path) {
    return std::wstring(kLogicalOrderPrefix) + NormalizeMemberPath(path);
}

bool SavedOrderMatchesItem(const std::wstring& saved, const DesktopItem& item) {
    if (saved.rfind(kLogicalOrderPrefix, 0) == 0) {
        return SameMemberPath(saved.substr(std::size(kLogicalOrderPrefix) - 1), item.fullPath);
    }
    return _wcsicmp(saved.c_str(), item.name.c_str()) == 0;
}

std::wstring ItemOrderKey(const Fence& fence, const DesktopItem& item) {
    const bool logical = std::any_of(fence.memberPaths.begin(), fence.memberPaths.end(),
        [&](const std::wstring& path) { return SameMemberPath(path, item.fullPath); });
    return logical ? LogicalOrderKey(item.fullPath) : item.name;
}

long long IntersectionArea(const RECT& first, const RECT& second) {
    const long left = (std::max)(first.left, second.left);
    const long top = (std::max)(first.top, second.top);
    const long right = (std::min)(first.right, second.right);
    const long bottom = (std::min)(first.bottom, second.bottom);
    if (right <= left || bottom <= top) return 0;
    return static_cast<long long>(right - left) * (bottom - top);
}

const MonitorService::MonitorSnapshot* FindBestMonitorForRect(
    const MonitorService::DisplayTopologySnapshot& topology,
    const RECT& rect) {
    const MonitorService::MonitorSnapshot* best = nullptr;
    long long bestArea = 0;
    for (const auto& monitor : topology.monitors) {
        const long long area = IntersectionArea(rect, monitor.workArea);
        if (area > bestArea) {
            best = &monitor;
            bestArea = area;
        }
    }
    return bestArea > 0 ? best : nullptr;
}

bool IsStrongMonitorMatch(
    const RECT& rect,
    const MonitorService::MonitorSnapshot& monitor) {
    const long long width = (std::max)(0L, rect.right - rect.left);
    const long long height = (std::max)(0L, rect.bottom - rect.top);
    const long long rectArea = width * height;
    if (rectArea <= 0) return false;
    const long long overlap = IntersectionArea(rect, monitor.workArea);
    return overlap > 0 && overlap * 2 >= rectArea;
}

bool IsSafeReconnectMonitorMatch(
    const MonitorService::DisplayTopologySnapshot& topology,
    const RECT& rect,
    const MonitorService::MonitorSnapshot* monitor) {
    // After the primary display is unplugged Windows can move the surviving
    // display to (0,0). Geometric overlap then identifies the wrong panel,
    // even when the old fence is completely inside its new work area. Keep
    // the missing identity until a multi-monitor layout returns.
    if (!monitor || topology.monitors.size() < 2) return false;
    if (IsStrongMonitorMatch(rect, *monitor)) return true;

    // Do not project an offline secondary fence onto the only remaining
    // monitor. Once the multi-monitor topology is back, however, any real
    // overlap with the best matching monitor is sufficient to repair a stale
    // CCD/GDI identity. This covers fences near an edge or resized while the
    // monitor was using a temporary mode, where the old 50% rule left the
    // visible fence permanently marked offline and removed it from snapping.
    return topology.monitors.size() > 1 &&
        FindBestMonitorForRect(topology, rect) == monitor &&
        IntersectionArea(rect, monitor->monitorRect) > 0;
}

void ClampRuntimeRectToWorkArea(RECT& rect, const RECT& workArea) {
    const long workWidth = (std::max)(1L, workArea.right - workArea.left);
    const long workHeight = (std::max)(1L, workArea.bottom - workArea.top);
    const long width = (std::min)((std::max)(1L, rect.right - rect.left), workWidth);
    const long height = (std::min)((std::max)(1L, rect.bottom - rect.top), workHeight);
    rect.left = (std::clamp)(rect.left, workArea.left, workArea.right - width);
    rect.top = (std::clamp)(rect.top, workArea.top, workArea.bottom - height);
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
}

void NormalizeGameFenceGeometry(Fence& fence,
                                const MonitorService::MonitorSnapshot& monitor) {
    if (fence.kind != FenceKind::Game) return;

    const int width = (std::max)(1, MulDiv(GAME_FENCE_WIDTH,
                                           static_cast<int>(monitor.dpiX), 96));
    const int height = (std::max)(1, MulDiv(GAME_FENCE_HEIGHT,
                                            static_cast<int>(monitor.dpiY), 96));
    fence.rect.right = fence.rect.left + width;
    fence.rect.bottom = fence.rect.top + height;
    fence.expandedHeight = height;
    ClampRuntimeRectToWorkArea(fence.rect, monitor.workArea);
}

bool IsGameLauncher(const DesktopItem& item) {
    if (item.isDirectory) return false;
    return _wcsicmp(item.ext.c_str(), L".lnk") == 0 ||
           _wcsicmp(item.ext.c_str(), L".url") == 0 ||
           _wcsicmp(item.ext.c_str(), L".exe") == 0;
}

bool ResolveShortcutTarget(
    const std::wstring& shortcutPath,
    std::wstring& targetPath) {
    targetPath.clear();
    if (_wcsicmp(
            std::filesystem::path(shortcutPath)
                .extension().c_str(),
            L".lnk") != 0) {
        return false;
    }
    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&shellLink))) ||
        !shellLink) {
        return false;
    }
    IPersistFile* persist = nullptr;
    bool resolved = false;
    if (SUCCEEDED(shellLink->QueryInterface(
            IID_PPV_ARGS(&persist))) &&
        persist &&
        SUCCEEDED(persist->Load(
            shortcutPath.c_str(), STGM_READ))) {
        std::vector<wchar_t> buffer(32768, L'\0');
        WIN32_FIND_DATAW findData{};
        if (SUCCEEDED(shellLink->GetPath(
                buffer.data(),
                static_cast<int>(buffer.size()),
                &findData, SLGP_RAWPATH)) &&
            buffer[0] != L'\0') {
            targetPath =
                NormalizeMemberPath(buffer.data());
            resolved = true;
        }
    }
    if (persist) persist->Release();
    shellLink->Release();
    return resolved;
}

bool ShortcutTargetsMatch(
    const std::wstring& first,
    const std::wstring& second) {
    std::wstring firstTarget;
    std::wstring secondTarget;
    return ResolveShortcutTarget(first, firstTarget) &&
        ResolveShortcutTarget(second, secondTarget) &&
        _wcsicmp(
            firstTarget.c_str(),
            secondTarget.c_str()) == 0;
}

bool ResolveDesktopShortcutMirror(
    const std::wstring& memberPath,
    std::wstring& desktopMirror) {
    desktopMirror.clear();
    if (IsDesktopFilesystemPath(memberPath) ||
        _wcsicmp(
            std::filesystem::path(memberPath)
                .extension().c_str(),
            L".lnk") != 0) {
        return false;
    }
    const std::wstring fileName =
        std::filesystem::path(memberPath)
            .filename().wstring();
    if (fileName.empty()) return false;

    std::vector<std::wstring> matches;
    for (const auto& root :
         { GetUserDesktopPath(),
           GetPublicDesktopPath() }) {
        if (root.empty()) continue;
        const std::wstring candidate =
            (std::filesystem::path(root) /
             fileName).wstring();
        if (!std::filesystem::exists(candidate) ||
            !ShortcutTargetsMatch(
                memberPath, candidate)) {
            continue;
        }
        if (std::none_of(
                matches.begin(), matches.end(),
                [&](const std::wstring& existing) {
                    return SameMemberPath(
                        existing, candidate);
                })) {
            matches.push_back(
                NormalizeMemberPath(candidate));
        }
    }
    if (matches.size() != 1) return false;
    desktopMirror = matches.front();
    return true;
}
}

FenceManager::FenceManager() {}
FenceManager::~FenceManager() { Shutdown(); }

bool FenceManager::Initialize(HINSTANCE hInstance) {
    this->hInstance = hInstance;
    configWritesEnabled = false;
    configService = std::make_unique<ConfigService>();
    configService->EnsureDirectories();

    ConfigLoadResult loadResult = configService->LoadFencesWithStatus();
    if (loadResult.status == ConfigLoadStatus::Corrupt ||
        loadResult.status == ConfigLoadStatus::IoError) {
        // Never manufacture defaults over a layout that may become readable
        // again. The caller can report startup failure while the original and
        // last-good files remain untouched.
        return false;
    }
    fences = std::move(loadResult.fences);
    const int loadedVersion = loadResult.version;
    const bool recoveredConfigFromLastGood = loadResult.loadedFromLastGood;
    const bool needsConfigUpgrade = loadedVersion < 9;
    const bool needsLegacySuppressionUpgrade = loadedVersion < 8;
    configWritesEnabled = true;
    bool generatedDefaults = false;
    if (fences.empty()) {
        GenerateDefaultFences();
        generatedDefaults = true;
    } else if (loadedVersion < 4) {
        const UINT dpi = (std::max)(96U, GetDpiForSystem());
        for (auto& fence : fences) {
            fence.rect.left = MulDiv(fence.rect.left, dpi, 96);
            fence.rect.top = MulDiv(fence.rect.top, dpi, 96);
            fence.rect.right = MulDiv(fence.rect.right, dpi, 96);
            fence.rect.bottom = MulDiv(fence.rect.bottom, dpi, 96);
        }
    }

    bool removedProtectedShortcut = false;
    for (auto& f : fences) {
        if (f.folderPath.empty()) {
            f.folderPath = FenceStorageService::GetDefaultFenceFolderPath(f.id);
            f.isMappedFolder = false;
        }
        FenceStorageService::EnsureFenceFolder(f.id);
        if (f.isMappedFolder) f.title = f.folderPath;
        if (f.kind == FenceKind::Game || f.kind == FenceKind::Clock) {
            f.isMappedFolder = false;
            f.searchActive = false;
            f.searchQuery.clear();
            f.viewMode = FenceViewMode::Icon;
        }
        if (f.itemDesktopOrigins.size() < f.itemOrder.size()) {
            f.itemDesktopOrigins.resize(f.itemOrder.size(), POINT{ LONG_MIN, LONG_MIN });
        }
        if (f.itemDesktopOriginPlacements.size() < f.itemOrder.size()) {
            f.itemDesktopOriginPlacements.resize(f.itemOrder.size());
        }
        if (!f.isMappedFolder) {
            std::vector<std::wstring> retainedOrder;
            std::vector<POINT> retainedOrigins;
            std::vector<MonitorService::MonitorRelativePoint>
                retainedOriginPlacements;
            retainedOrder.reserve(f.itemOrder.size());
            retainedOrigins.reserve(f.itemOrder.size());
            retainedOriginPlacements.reserve(f.itemOrder.size());
            for (size_t index = 0; index < f.itemOrder.size(); ++index) {
                if (ShellIntegrationService::IsProtectedDesktopItem(f.itemOrder[index])) {
                    removedProtectedShortcut = true;
                    continue;
                }
                retainedOrder.push_back(f.itemOrder[index]);
                retainedOrigins.push_back(index < f.itemDesktopOrigins.size()
                    ? f.itemDesktopOrigins[index] : POINT{ LONG_MIN, LONG_MIN });
                retainedOriginPlacements.push_back(
                    index < f.itemDesktopOriginPlacements.size()
                        ? f.itemDesktopOriginPlacements[index]
                        : MonitorService::MonitorRelativePoint{});
            }
            f.itemOrder = std::move(retainedOrder);
            f.itemDesktopOrigins = std::move(retainedOrigins);
            f.itemDesktopOriginPlacements =
                std::move(retainedOriginPlacements);
        }
    }

    // Version 7 could leave empty hidden "name reservations" in a fence
    // directory. Clean those compatibility artifacts, but never migrate a
    // memberPath: logical membership deliberately preserves its source path.
    const std::wstring desktopPath = GetUserDesktopPath();
    FileMoveService migrationMover;
    for (auto& fence : fences) {
        if (fence.isMappedFolder || fence.folderPath.empty()) continue;

        for (const auto& managed : FileService::ScanFolder(fence.folderPath)) {
            migrationMover.RemoveDesktopNameReservation(managed.fullPath);
        }
    }

    // Older releases physically moved normal-fence items into the private
    // Fences directory. Migrate each conflict-free item back to its real
    // Desktop path and retain only logical membership. This makes the path
    // visible in an ordinary File Explorer window opened to Desktop while the
    // desktop Shell view itself is filtered to avoid a duplicate icon.
    //
    // A real same-name target or a failed move is never overwritten: the
    // private copy remains untouched and migration is retried on a later start.
    bool migratedLegacyStoredItems = false;
    bool recoveredLegacyFiles = false;
    if (!desktopPath.empty()) {
        for (auto& fence : fences) {
            if (fence.isMappedFolder || fence.folderPath.empty()) continue;
            const auto storedItems = FileService::ScanFolder(fence.folderPath);
            for (const auto& storedItem : storedItems) {
                const std::wstring desktopTarget =
                    (std::filesystem::path(desktopPath) /
                     std::filesystem::path(storedItem.fullPath).filename()).wstring();
                if (std::filesystem::exists(desktopTarget)) continue;

                std::wstring movedPath;
                if (migrationMover.MoveToDirectory(
                        storedItem.fullPath, desktopPath, false,
                        &movedPath, false) != FileMoveResult::Success ||
                    movedPath.empty()) {
                    continue;
                }
                if (std::none_of(
                        fence.memberPaths.begin(), fence.memberPaths.end(),
                        [&](const std::wstring& memberPath) {
                            return SameMemberPath(memberPath, movedPath);
                        })) {
                    fence.memberPaths.push_back(
                        NormalizeMemberPath(movedPath));
                }
                migratedLegacyStoredItems = true;
            }
        }
    }

    // A previous physical-storage build could move a desktop item between
    // known Desktop roots while retaining its old logical path. Repair only
    // an unambiguous same-name match; never guess across arbitrary folders.
    if (!desktopPath.empty()) {
        const std::wstring publicDesktopPath = GetPublicDesktopPath();
        for (auto& fence : fences) {
            if (fence.isMappedFolder) continue;
            for (auto& memberPath : fence.memberPaths) {
                const bool originalExists =
                    std::filesystem::exists(memberPath);

                const std::wstring fileName =
                    std::filesystem::path(memberPath).filename().wstring();
                if (fileName.empty()) continue;
                std::vector<std::wstring> matches;
                for (const auto& root :
                     { desktopPath, publicDesktopPath }) {
                    if (root.empty()) continue;
                    const std::wstring candidate =
                        (std::filesystem::path(root) / fileName).wstring();
                    if (!std::filesystem::exists(candidate)) continue;
                    if (std::none_of(
                            matches.begin(), matches.end(),
                            [&](const std::wstring& existing) {
                                return SameMemberPath(
                                    existing, candidate);
                            })) {
                        matches.push_back(
                            NormalizeMemberPath(candidate));
                    }
                }
                if (matches.size() != 1) continue;

                const std::wstring oldPath = memberPath;
                const std::wstring& repairedPath = matches.front();
                if (originalExists &&
                    !ShortcutTargetsMatch(
                        oldPath, repairedPath)) {
                    continue;
                }
                const bool claimedElsewhere = std::any_of(
                    fences.begin(), fences.end(),
                    [&](const Fence& otherFence) {
                        return &otherFence != &fence &&
                            std::any_of(
                                otherFence.memberPaths.begin(),
                                otherFence.memberPaths.end(),
                                [&](const std::wstring& otherPath) {
                                    return SameMemberPath(
                                        otherPath, repairedPath);
                                });
                    });
                if (claimedElsewhere) continue;

                memberPath = repairedPath;
                const std::wstring oldOrder =
                    LogicalOrderKey(oldPath);
                for (auto& savedOrder : fence.itemOrder) {
                    if (_wcsicmp(
                            savedOrder.c_str(),
                            oldOrder.c_str()) == 0) {
                        savedOrder =
                            LogicalOrderKey(repairedPath);
                    }
                }
                recoveredLegacyFiles = true;
            }
        }
    }

    for (auto& fence : fences) {
        if (fence.isMappedFolder || fence.folderPath.empty()) continue;
        for (size_t index = 0; index < fence.itemOrder.size(); ++index) {
            const std::wstring& itemName = fence.itemOrder[index];
            if (itemName.rfind(kLogicalOrderPrefix, 0) == 0) continue;
            const bool alreadyHasLogicalMember = std::any_of(
                fence.memberPaths.begin(), fence.memberPaths.end(),
                [&](const std::wstring& path) {
                    return _wcsicmp(std::filesystem::path(path).filename().c_str(),
                                    itemName.c_str()) == 0;
                });
            if (alreadyHasLogicalMember) continue;
            const std::wstring target =
                (std::filesystem::path(fence.folderPath) / itemName).wstring();
            if (std::filesystem::exists(target)) continue;
            const std::wstring desktopSource =
                (std::filesystem::path(desktopPath) / itemName).wstring();
            if (!std::filesystem::exists(desktopSource)) continue;
            POINT desktopPosition{};
            if (DesktopShellService::GetPathPositionOnDesktop(desktopSource, desktopPosition)) {
                fence.itemDesktopOrigins[index] = desktopPosition;
            }
            if (std::none_of(fence.memberPaths.begin(), fence.memberPaths.end(),
                [&](const std::wstring& path) { return SameMemberPath(path, desktopSource); })) {
                fence.memberPaths.push_back(NormalizeMemberPath(desktopSource));
                recoveredLegacyFiles = true;
            }
        }
    }

    // Builds before config v8 could suppress logical desktop members by
    // writing HIDDEN|SYSTEM to the real file. Remove that legacy state once
    // so an ordinary File Explorer window opened to Desktop always shows the
    // real path. Current builds never write these attributes.
    if (needsLegacySuppressionUpgrade) {
        for (const auto& fence : fences) {
            if (fence.isMappedFolder) continue;
            for (const auto& memberPath : fence.memberPaths) {
                if (!IsDesktopFilesystemPath(memberPath)) continue;
                const DWORD attributes = GetFileAttributesW(memberPath.c_str());
                constexpr DWORD legacySuppression =
                    FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & legacySuppression) != 0 &&
                    FileService::SetLogicalGroupingHidden(memberPath, false)) {
                    recoveredLegacyFiles = true;
                }
            }
        }
    }

    // Repair origin data written by older builds before any logical member is
    // removed from the desktop view. This is especially important for a
    // secondary monitor: once hidden, its negative physical coordinate can no
    // longer be recovered from Explorer.
    for (auto& fence : fences) {
        if (fence.isMappedFolder) continue;
        if (fence.itemDesktopOrigins.size() < fence.itemOrder.size()) {
            fence.itemDesktopOrigins.resize(
                fence.itemOrder.size(), POINT{ LONG_MIN, LONG_MIN });
        }
        if (fence.itemDesktopOriginPlacements.size() <
            fence.itemOrder.size()) {
            fence.itemDesktopOriginPlacements.resize(
                fence.itemOrder.size());
        }
        for (const auto& memberPath : fence.memberPaths) {
            DesktopItem member;
            if (!FileService::DescribePath(memberPath, member, true) ||
                !IsDesktopFilesystemPath(member.fullPath)) continue;

            auto order = std::find_if(
                fence.itemOrder.begin(), fence.itemOrder.end(),
                [&](const std::wstring& saved) {
                    return SavedOrderMatchesItem(saved, member);
                });
            if (order == fence.itemOrder.end()) {
                fence.itemOrder.push_back(LogicalOrderKey(member.fullPath));
                fence.itemDesktopOrigins.push_back({ LONG_MIN, LONG_MIN });
                fence.itemDesktopOriginPlacements.push_back({});
                order = std::prev(fence.itemOrder.end());
            }
            const size_t index = static_cast<size_t>(
                std::distance(fence.itemOrder.begin(), order));
            POINT& origin = fence.itemDesktopOrigins[index];
            if (origin.x != LONG_MIN && origin.y != LONG_MIN) continue;

            POINT current{};
            if (DesktopShellService::GetPathPositionOnDesktop(
                    member.fullPath, current)) {
                origin = current;
                recoveredLegacyFiles = true;
            }
        }
    }

    std::sort(fences.begin(), fences.end(),
        [](const Fence& a, const Fence& b) { return a.zOrder < b.zOrder; });

    const auto topology = MonitorService::CaptureTopology();
    bool migratedDesktopOrigins = false;
    for (auto& fence : fences) {
        migratedDesktopOrigins =
            MigrateItemDesktopOriginPlacements(fence, topology) ||
            migratedDesktopOrigins;
    }
    bool migratedMonitorPlacements = false;
    for (auto& fence : fences) {
        if (fence.monitorPlacement.valid) {
            const auto* monitor = MonitorService::FindMonitorById(
                topology, fence.monitorPlacement.monitorId);
            if (!monitor) {
                // A USB-C display can come back with a new CCD target path
                // even though it occupies the same desktop rectangle. Recover
                // the monitor by strong geometric overlap so its fences become
                // online again and keep participating in snapping.
                const auto* recovered = FindBestMonitorForRect(
                    topology, fence.rect);
                if (!IsSafeReconnectMonitorMatch(
                        topology, fence.rect, recovered)) {
                    fence.monitorOnline = false;
                    continue;
                }
                fence.monitorPlacement = MonitorService::CapturePlacement96(
                    fence.rect, fence.expandedHeight, *recovered);
                fence.monitorOnline = true;
                migratedMonitorPlacements = true;
                continue;
            }
            int expandedHeight = fence.expandedHeight;
            fence.rect = MonitorService::MaterializePlacement96(
                fence.monitorPlacement, *monitor, &expandedHeight);
            ClampRuntimeRectToWorkArea(fence.rect, monitor->workArea);
            fence.expandedHeight = (std::max)(FENCE_TITLE_BAR_HEIGHT, expandedHeight);
            fence.monitorOnline = true;
            continue;
        }

        // Legacy layouts have absolute coordinates only. Migrate a fence only
        // when it intersects a currently active monitor. An offline secondary
        // monitor cannot be identified safely, so its old rectangle is retained
        // and never projected onto the primary display.
        const auto* monitor = FindBestMonitorForRect(topology, fence.rect);
        if (!monitor) {
            fence.monitorOnline = false;
            continue;
        }
        fence.monitorPlacement = MonitorService::CapturePlacement96(
            fence.rect, fence.expandedHeight, *monitor);
        fence.monitorOnline = true;
        migratedMonitorPlacements = true;
    }

    RefreshAllFenceItems();
    for (const auto& fence : fences) {
        SyncLogicalDesktopPositions(fence.id);
    }
    if (generatedDefaults || migratedMonitorPlacements ||
        migratedDesktopOrigins || recoveredLegacyFiles ||
        migratedLegacyStoredItems ||
        needsConfigUpgrade || recoveredConfigFromLastGood ||
        removedProtectedShortcut) {
        configService->SaveFences(fences);
    }

    iconCache = std::make_unique<IconCache>();
    iconCache->Initialize();

    return !fences.empty();
}

void FenceManager::Shutdown() {
    SaveConfig();
    if (iconCache) iconCache->Shutdown();
    DesktopShellService::ClearSuppressionState();
    fences.clear();
    configWritesEnabled = false;
}

bool FenceManager::ReleaseFenceItemsToDesktop() {
    bool success = true;
    for (const auto& fence : fences) {
        if (fence.isMappedFolder) continue;
        // Normal fences are logical groups. Releasing them only restores the
        // native desktop representation for members that originally came from
        // the desktop; it must never move files from the private Fences
        // directory (or any other source directory) as a side effect.
        if (!RestoreLogicalDesktopItems(fence.id, true)) success = false;
    }
    return success;
}

std::vector<Fence> FenceManager::GetAllFences() const {
    return fences;
}

Fence* FenceManager::GetFenceById(const std::wstring& id) {
    for (auto& f : fences)
        if (f.id == id) return &f;
    return nullptr;
}

bool FenceManager::IsLogicalItem(const std::wstring& fenceId,
                                 const std::wstring& fullPath) const {
    for (const auto& fence : fences) {
        if (fence.id != fenceId) continue;
        return std::any_of(fence.memberPaths.begin(), fence.memberPaths.end(),
            [&](const std::wstring& path) { return SameMemberPath(path, fullPath); });
    }
    return false;
}

bool FenceManager::AddLogicalItem(const std::wstring& fenceId,
                                  const std::wstring& fullPath,
                                  POINT desktopOrigin,
                                  bool deferCommit) {
    Fence* target = GetFenceById(fenceId);
    DesktopItem item;
    if (!target || target->isMappedFolder || target->kind == FenceKind::Clock ||
        !FileService::DescribePath(fullPath, item, true)) return false;
    if (target->kind == FenceKind::Game) {
        if (!IsGameLauncher(item)) return false;
        const bool alreadyContainsPath = std::any_of(
            target->memberPaths.begin(), target->memberPaths.end(),
            [&](const std::wstring& path) { return SameMemberPath(path, item.fullPath); });
        if (!target->memberPaths.empty() && !alreadyContainsPath) return false;
    }
    const std::wstring normalized = NormalizeMemberPath(item.fullPath);
    POINT resolvedDesktopOrigin = desktopOrigin;
    const auto hasOrigin = [](POINT value) {
        return value.x != LONG_MIN && value.y != LONG_MIN;
    };
    POINT inheritedDesktopOrigin{ LONG_MIN, LONG_MIN };
    MonitorService::MonitorRelativePoint inheritedOriginPlacement;
    bool hasInheritedDesktopOrigin = false;

    // Preserve the original desktop anchor when membership moves between
    // fences. In particular, an offline monitor-relative anchor must not be
    // replaced by a fabricated primary-screen coordinate merely because its
    // physical point cannot be materialized right now.
    for (const auto& fence : fences) {
        if (fence.isMappedFolder ||
            std::none_of(
                fence.memberPaths.begin(), fence.memberPaths.end(),
                [&](const std::wstring& path) {
                    return SameMemberPath(path, normalized);
                })) {
            continue;
        }
        for (size_t index = 0; index < fence.itemOrder.size(); ++index) {
            if (!SavedOrderMatchesItem(fence.itemOrder[index], item)) continue;
            if (index < fence.itemDesktopOrigins.size() &&
                hasOrigin(fence.itemDesktopOrigins[index])) {
                inheritedDesktopOrigin = fence.itemDesktopOrigins[index];
                hasInheritedDesktopOrigin = true;
            }
            if (index < fence.itemDesktopOriginPlacements.size() &&
                fence.itemDesktopOriginPlacements[index].valid) {
                inheritedOriginPlacement =
                    fence.itemDesktopOriginPlacements[index];
            }
            break;
        }
        break;
    }

    std::wstring desktopRepresentation = normalized;
    const bool hasDesktopRepresentation =
        IsDesktopFilesystemPath(normalized) ||
        ResolveDesktopShortcutMirror(
            normalized, desktopRepresentation);
    if (!UsesSyntheticDesktopForTests() &&
        hasDesktopRepresentation) {
        // Capture before removing the native desktop representation. This also
        // repairs re-adds whose older order entry had no saved origin.
        if (!hasOrigin(resolvedDesktopOrigin)) {
            DesktopShellService::GetPathPositionOnDesktop(
                desktopRepresentation,
                resolvedDesktopOrigin);
        }
        // Queue the path before committing membership. Explorer can rebuild
        // its view during a drop; DesktopShellService keeps the suppression
        // request and reconciles the replacement view without rejecting the
        // user's drag.
        if (!DesktopShellService::SetPathSuppressedOnDesktop(
                desktopRepresentation, true)) {
            return false;
        }
    }

    // A path belongs to at most one fence. Moving between fences only changes
    // membership; the file itself is never renamed or relocated.
    for (auto& fence : fences) {
        if (fence.isMappedFolder) continue;
        fence.memberPaths.erase(std::remove_if(fence.memberPaths.begin(), fence.memberPaths.end(),
            [&](const std::wstring& path) { return SameMemberPath(path, normalized); }),
            fence.memberPaths.end());
    }
    target = GetFenceById(fenceId);
    if (!target) return false;
    target->memberPaths.push_back(normalized);
    const std::wstring orderKey = LogicalOrderKey(normalized);
    const auto existingOrder = std::find_if(
        target->itemOrder.begin(), target->itemOrder.end(),
        [&](const std::wstring& saved) { return SavedOrderMatchesItem(saved, item); });
    if (existingOrder == target->itemOrder.end()) {
        target->itemOrder.push_back(orderKey);
        target->itemDesktopOrigins.push_back(
            hasOrigin(resolvedDesktopOrigin)
                ? resolvedDesktopOrigin
                : inheritedDesktopOrigin);
        target->itemDesktopOriginPlacements.push_back(
            !hasOrigin(resolvedDesktopOrigin) &&
                inheritedOriginPlacement.valid
                ? inheritedOriginPlacement
                : MonitorService::MonitorRelativePoint{});
        if (hasOrigin(resolvedDesktopOrigin)) {
            CaptureItemDesktopOriginPlacement(
                *target, target->itemOrder.size() - 1,
                resolvedDesktopOrigin);
        }
    } else {
        const size_t index = static_cast<size_t>(
            std::distance(target->itemOrder.begin(), existingOrder));
        if (target->itemDesktopOrigins.size() < target->itemOrder.size()) {
            target->itemDesktopOrigins.resize(
                target->itemOrder.size(), POINT{ LONG_MIN, LONG_MIN });
        }
        if (target->itemDesktopOriginPlacements.size() <
            target->itemOrder.size()) {
            target->itemDesktopOriginPlacements.resize(
                target->itemOrder.size());
        }
        if (hasOrigin(resolvedDesktopOrigin)) {
            target->itemDesktopOrigins[index] = resolvedDesktopOrigin;
            CaptureItemDesktopOriginPlacement(
                *target, index, resolvedDesktopOrigin);
        } else {
            if (hasInheritedDesktopOrigin) {
                target->itemDesktopOrigins[index] =
                    inheritedDesktopOrigin;
            }
            if (inheritedOriginPlacement.valid) {
                target->itemDesktopOriginPlacements[index] =
                    inheritedOriginPlacement;
            }
        }
    }
    if (!deferCommit) {
        RefreshAllFenceItems();
        SyncLogicalDesktopPositions(fenceId);
        SaveConfig();
    }
    return true;
}

bool FenceManager::RemoveLogicalItem(
    const std::wstring& fenceId, const std::wstring& fullPath,
    const POINT* desiredDesktopPosition) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->isMappedFolder) return false;
    bool restored = true;
    DesktopItem described;
    if (FileService::DescribePath(fullPath, described, true)) {
        if (!UsesSyntheticDesktopForTests() && IsDesktopFilesystemPath(described.fullPath)) {
            POINT restorePoint{};
            DesktopOriginResolution originResolution =
                DesktopOriginResolution::Missing;
            if (desiredDesktopPosition) {
                restorePoint = *desiredDesktopPosition;
            } else {
                originResolution = ResolveItemDesktopOrigin(
                    fenceId, described.fullPath,
                    MonitorService::CaptureTopology(), restorePoint);
                if (originResolution == DesktopOriginResolution::Offline) {
                    // Removing the membership while its owning monitor is absent
                    // would let Explorer place it arbitrarily on another screen.
                    return false;
                }
            }
            const bool hasOriginalPosition =
                !desiredDesktopPosition &&
                originResolution == DesktopOriginResolution::Online;
            if (!hasOriginalPosition) {
                if (!desiredDesktopPosition) {
                    const auto existing = std::find_if(
                        fence->items.begin(), fence->items.end(),
                        [&](const DesktopItem& item) {
                            return SameMemberPath(
                                item.fullPath, fullPath);
                        });
                    if (existing != fence->items.end()) {
                        restorePoint = {
                            existing->iconRect.left,
                            existing->iconRect.top
                        };
                    }
                }
            }
            DesktopShellService::SetPathSuppressedOnDesktop(
                described.fullPath, false);
            restored = desiredDesktopPosition
                ? DesktopShellService::PositionPathOnDesktop(
                    described.fullPath, restorePoint)
                : (hasOriginalPosition
                    ? DesktopShellService::RestorePathToDesktopPosition(
                        described.fullPath, restorePoint)
                    : DesktopShellService::PositionPathOnDesktop(
                        described.fullPath, restorePoint));
            if (!restored) {
                // Keep the membership and put the icon back out of view; never
                // forget an item that Explorer failed to restore visibly.
                DesktopShellService::SetPathSuppressedOnDesktop(
                    described.fullPath, true);
                SyncLogicalDesktopPositions(fenceId);
            }
        } else if (!UsesSyntheticDesktopForTests()) {
            std::wstring desktopMirror;
            if (ResolveDesktopShortcutMirror(
                    described.fullPath,
                    desktopMirror)) {
                DesktopShellService::
                    SetPathSuppressedOnDesktop(
                        desktopMirror, false);
            }
        }
    }
    // Never forget a logical member while its native desktop icon is still
    // parked; otherwise a failed restore would make the file appear missing.
    if (!restored) return false;
    const auto oldSize = fence->memberPaths.size();
    fence->memberPaths.erase(std::remove_if(fence->memberPaths.begin(), fence->memberPaths.end(),
        [&](const std::wstring& path) { return SameMemberPath(path, fullPath); }),
        fence->memberPaths.end());
    if (fence->memberPaths.size() == oldSize) return false;
    RefreshFenceItems(fenceId);
    SaveConfig();
    return true;
}

bool FenceManager::MoveLogicalItem(const std::wstring& sourceFenceId,
                                   const std::wstring& targetFenceId,
                                   const std::wstring& fullPath) {
    if (!IsLogicalItem(sourceFenceId, fullPath)) return false;
    Fence* target = GetFenceById(targetFenceId);
    if (!target || target->isMappedFolder) return false;
    POINT origin{ LONG_MIN, LONG_MIN };
    DesktopItem item;
    if (FileService::DescribePath(fullPath, item, true)) {
        POINT resolved{};
        if (ResolveItemDesktopOrigin(
                sourceFenceId, item.fullPath,
                MonitorService::CaptureTopology(), resolved) ==
            DesktopOriginResolution::Online) {
            origin = resolved;
        }
    }
    return AddLogicalItem(targetFenceId, fullPath, origin);
}

void FenceManager::SyncLogicalDesktopPositions(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->isMappedFolder) return;
    if (UsesSyntheticDesktopForTests()) return;
    // memberPaths is the persistent source of truth. Suppress only the native
    // desktop IShellView entry: never hide, rename, copy or move the real path.
    // This keeps C:\Users\...\Desktop truthful in File Explorer while the
    // overlay remains the single desktop representation.
    const auto originalSize = fence->items.size();
    bool capturedMissingOrigin = false;
    for (const auto& memberPath : fence->memberPaths) {
        DesktopItem item;
        if (!FileService::DescribePath(memberPath, item, true)) {
            DesktopShellService::ForgetPathSuppression(memberPath);
            continue;
        }
        std::wstring desktopRepresentation =
            item.fullPath;
        const bool directDesktop = IsDesktopFilesystemPath(desktopRepresentation);
        if (!directDesktop &&
            !ResolveDesktopShortcutMirror(
                item.fullPath,
                desktopRepresentation)) {
            continue;
        }

        // RefreshFenceItems can reorder itemOrder before the initialization
        // migration runs. Bind a missing coordinate to the full path one last
        // time while Explorer still exposes the native item, then persist it
        // before removing that item from the desktop view.
        POINT savedOrigin{};
        if (!GetItemDesktopOrigin(fenceId, item.fullPath, savedOrigin)) {
            POINT visiblePosition{};
            if (DesktopShellService::GetPathPositionOnDesktop(
                    desktopRepresentation,
                    visiblePosition) &&
                SetItemDesktopOrigin(fenceId, item.fullPath, visiblePosition)) {
                capturedMissingOrigin = true;
            }
        }
        if (!DesktopShellService::IsPathSuppressedOnDesktop(
                desktopRepresentation)) {
            DesktopShellService::SetPathSuppressedOnDesktop(
                desktopRepresentation, true);
        }
        const bool alreadyRendered = std::any_of(fence->items.begin(), fence->items.end(),
            [&](const DesktopItem& existing) {
                return SameMemberPath(existing.fullPath, item.fullPath);
            });
        if (!alreadyRendered) fence->items.push_back(std::move(item));
    }
    if (fence->items.size() != originalSize) {
        LayoutFenceItems(fenceId);
        SyncItemOrder(*fence);
    }
    if (capturedMissingOrigin && configService) {
        configService->SaveFences(fences);
    }
}

bool FenceManager::RestoreLogicalDesktopItems(
    const std::wstring& fenceId,
    bool allowOfflineFallback) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->isMappedFolder) return false;
    if (UsesSyntheticDesktopForTests()) return true;
    bool success = true;
    std::vector<std::wstring> attemptedPaths;
    const auto topology = MonitorService::CaptureTopology();
    const auto fallbackDesktopPoint = [&]() {
        const MonitorService::MonitorSnapshot* target = nullptr;
        for (const auto& monitor : topology.monitors) {
            if (monitor.primary) {
                target = &monitor;
                break;
            }
            if (!target) target = &monitor;
        }
        if (!target) {
            return POINT{
                fence->rect.left + 24,
                fence->rect.top + FENCE_TITLE_BAR_HEIGHT + 16
            };
        }
        return POINT{
            target->workArea.left + 24,
            target->workArea.top + FENCE_TITLE_BAR_HEIGHT + 16
        };
    };
    for (const auto& memberPath : fence->memberPaths) {
        DesktopItem item;
        if (!FileService::DescribePath(memberPath, item, true)) continue;
        if (!IsDesktopFilesystemPath(item.fullPath)) continue;
        attemptedPaths.push_back(item.fullPath);

        POINT restorePoint{};
        const auto originResolution = ResolveItemDesktopOrigin(
            fenceId, item.fullPath, topology, restorePoint);
        if (originResolution == DesktopOriginResolution::Offline) {
            if (!allowOfflineFallback) {
                // Normal fence dissolution remains strict: do not silently
                // move an icon to another monitor while its owner is absent.
                success = false;
                continue;
            }
            // During application exit the important invariant is that the
            // real desktop file becomes visible. Its original monitor is
            // temporarily unavailable, so place it on the current primary
            // work area instead of cancelling exit and leaving it hidden.
            restorePoint = fallbackDesktopPoint();
        }
        const bool hasOriginalPosition =
            originResolution == DesktopOriginResolution::Online;
        if (!hasOriginalPosition &&
            originResolution != DesktopOriginResolution::Offline) {
            const auto existing = std::find_if(fence->items.begin(), fence->items.end(),
                [&](const DesktopItem& candidate) {
                    return SameMemberPath(candidate.fullPath, item.fullPath);
                });
            restorePoint = existing != fence->items.end()
                ? POINT{ existing->iconRect.left, existing->iconRect.top }
                : POINT{ fence->rect.left + 24, fence->rect.top + FENCE_TITLE_BAR_HEIGHT + 16 };
        }
        DesktopShellService::SetPathSuppressedOnDesktop(item.fullPath, false);
        const bool restored = hasOriginalPosition
            ? DesktopShellService::RestorePathToDesktopPosition(
                item.fullPath, restorePoint)
            : DesktopShellService::PositionPathOnDesktop(
                item.fullPath, restorePoint);
        if (!restored) success = false;
    }
    if (!success) {
        for (const auto& path : attemptedPaths) {
            DesktopShellService::SetPathSuppressedOnDesktop(path, true);
        }
        SyncLogicalDesktopPositions(fenceId);
    }
    return success;
}

FenceHitTestResult FenceManager::HitTest(POINT screenPt) const {
    if (!allVisible) return {};
    for (int i = (int)fences.size() - 1; i >= 0; --i) {
        const auto& f = fences[i];
        if (!f.monitorOnline) continue;
        if (!PtInRect(&f.rect, screenPt))
            continue;

        RECT titleRect = f.rect;
        titleRect.bottom = titleRect.top + FENCE_TITLE_BAR_HEIGHT;

        FenceHitTestResult res;
        res.hit = true;
        res.index = i;
        res.fenceId = f.id;
        res.area = PtInRect(&titleRect, screenPt)
                   ? FenceHitArea::TitleBar
                   : FenceHitArea::Content;
        return res;
    }
    return {};
}

void FenceManager::BringToFront(const std::wstring& fenceId) {
    auto it = std::find_if(fences.begin(), fences.end(),
        [&](const Fence& f) { return f.id == fenceId; });
    if (it == fences.end()) return;

    // Move the selected fence to the end without deep-copying every path and
    // DesktopItem at the exact moment a drag begins.
    std::rotate(it, std::next(it), fences.end());

    for (int i = 0; i < (int)fences.size(); ++i)
        fences[i].zOrder = i;
}

std::wstring FenceManager::CreateFenceId() const {
    unsigned long long value = GetTickCount64();
    std::wstring id;
    do {
        id = L"fence_" + std::to_wstring(value++);
    } while (std::any_of(fences.begin(), fences.end(), [&](const Fence& fence) {
        return fence.id == id;
    }));
    return id;
}

Fence* FenceManager::CreateFenceAt(POINT point, const std::wstring& title,
                                   const std::wstring& folderPath, bool mappedFolder) {
    Fence fence;
    fence.id = CreateFenceId();
    fence.title = title.empty() ? L"新建格子" : title;
    fence.rect = { point.x - 180, point.y - 18, point.x + 180, point.y + 222 };
    fence.expandedHeight = fence.rect.bottom - fence.rect.top;
    fence.color = RGB(75, 78, 84);
    fence.opacity = 0.56f;
    fence.zOrder = static_cast<int>(fences.size());
    fence.isMappedFolder = mappedFolder;
    fence.folderPath = folderPath;
    if (mappedFolder && !folderPath.empty()) fence.title = folderPath;
    if (fence.folderPath.empty()) {
        fence.folderPath = FenceStorageService::GetDefaultFenceFolderPath(fence.id);
        FenceStorageService::EnsureFenceFolder(fence.id);
    }
    MonitorService::ApplyFenceBounds(fence.rect);
    fences.push_back(std::move(fence));
    CommitFencePlacement(fences.back().id);
    RefreshFenceItems(fences.back().id);
    SaveConfig();
    return &fences.back();
}

Fence* FenceManager::CreateGameFenceAt(POINT point, const std::wstring& title) {
    Fence* fence = CreateFenceAt(
        point, title.empty() ? L"\u6e38\u620f" : title);
    if (!fence) return nullptr;
    fence->kind = FenceKind::Game;
    fence->color = RGB(75, 78, 84);
    fence->opacity = 0.68f;
    fence->viewMode = FenceViewMode::Icon;
    fence->searchActive = false;
    fence->searchQuery.clear();
    fence->collapsed = false;
    fence->rect = {
        point.x - GAME_FENCE_WIDTH / 2,
        point.y - GAME_FENCE_HEIGHT / 2,
        point.x - GAME_FENCE_WIDTH / 2 + GAME_FENCE_WIDTH,
        point.y - GAME_FENCE_HEIGHT / 2 + GAME_FENCE_HEIGHT
    };
    MonitorService::ApplyFenceBounds(fence->rect);
    fence->expandedHeight = GAME_FENCE_HEIGHT;
    LayoutFenceItems(fence->id);
    CommitFencePlacement(fence->id);
    SaveConfig();
    return fence;
}

Fence* FenceManager::CreateClockFenceAt(POINT point, const std::wstring& title) {
    Fence* fence = CreateFenceAt(
        point, title.empty() ? L"\u65f6\u95f4" : title);
    if (!fence) return nullptr;
    fence->kind = FenceKind::Clock;
    fence->color = RGB(238, 232, 234);
    fence->opacity = 0.82f;
    fence->viewMode = FenceViewMode::Icon;
    fence->searchActive = false;
    fence->searchQuery.clear();
    fence->collapsed = false;
    fence->rect = {
        point.x - CLOCK_FENCE_WIDTH / 2,
        point.y - CLOCK_FENCE_HEIGHT / 2,
        point.x - CLOCK_FENCE_WIDTH / 2 + CLOCK_FENCE_WIDTH,
        point.y - CLOCK_FENCE_HEIGHT / 2 + CLOCK_FENCE_HEIGHT
    };
    MonitorService::ApplyFenceBounds(fence->rect);
    fence->expandedHeight = CLOCK_FENCE_HEIGHT;
    LayoutFenceItems(fence->id);
    CommitFencePlacement(fence->id);
    SaveConfig();
    return fence;
}

bool FenceManager::ToggleFenceCollapsed(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return false;
    if (fence->collapsed) {
        fence->collapsed = false;
        fence->rect.bottom = fence->rect.top + (std::max)(160, fence->expandedHeight);
        LayoutFenceItems(fenceId);
    } else {
        fence->expandedHeight = (std::max)(160L, fence->rect.bottom - fence->rect.top);
        fence->collapsed = true;
        fence->rect.bottom = fence->rect.top + FENCE_TITLE_BAR_HEIGHT;
    }
    CommitFencePlacement(fenceId);
    SaveConfig();
    return true;
}

bool FenceManager::ToggleFenceLocked(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return false;
    fence->locked = !fence->locked;
    SaveConfig();
    return true;
}

void FenceManager::TriggerLockAnimation(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return;
    fence->lockAnimActive = true;
    fence->lockAnimProgress = 0.0f;
    fence->lockAnimStartTime = GetTickCount64();
    fence->lockAnimFromLocked = !fence->locked; // 动画起始状态 = 切换前状态
}

bool FenceManager::SortFenceItems(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->kind == FenceKind::Game) return false;
    fence->sortByExtension = !fence->sortByExtension;
    std::stable_sort(fence->items.begin(), fence->items.end(), [&](const DesktopItem& a, const DesktopItem& b) {
        const std::wstring& firstA = fence->sortByExtension ? a.ext : a.name;
        const std::wstring& firstB = fence->sortByExtension ? b.ext : b.name;
        const int first = _wcsicmp(firstA.c_str(), firstB.c_str());
        return first == 0 ? _wcsicmp(a.name.c_str(), b.name.c_str()) < 0 : first < 0;
    });
    ArrangeFenceItems(fenceId);
    SaveConfig();
    return true;
}

void FenceManager::ArrangeFenceItems(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->collapsed) return;
    RECT contentRect = GetContentRect(*fence);
    // 强制清除自由坐标标记，确保按网格排列
    for (auto& item : fence->items) item.hasPersistedOffset = false;
    ClampScrollOffset(*fence);
    if (fence->kind == FenceKind::Game) {
        GridLayoutService::LayoutGameLauncher(contentRect, fence->items);
    } else {
        GridLayoutService::LayoutByMode(contentRect, fence->items, fence->viewMode,
                                        fence->scrollOffset);
    }
    SyncItemOrder(*fence);
    SyncLogicalDesktopPositions(fenceId);
}

void FenceManager::ArrangeAllItems() {
    for (auto& fence : fences) ArrangeFenceItems(fence.id);
    SaveConfig();
}

void FenceManager::ToggleAllLocks() {
    const bool lockAll = std::any_of(fences.begin(), fences.end(), [](const Fence& fence) {
        return !fence.locked;
    });
    for (auto& fence : fences) {
        if (fence.locked != lockAll) {
            fence.lockAnimActive = true;
            fence.lockAnimProgress = 0.0f;
            fence.lockAnimStartTime = GetTickCount64();
            fence.lockAnimFromLocked = !lockAll;
        }
        fence.locked = lockAll;
    }
    SaveConfig();
}

bool FenceManager::RemoveFence(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (fence && !fence->isMappedFolder && !RestoreLogicalDesktopItems(fenceId)) {
        return false;
    }
    const auto oldSize = fences.size();
    fences.erase(std::remove_if(fences.begin(), fences.end(),
        [&](const Fence& fence) { return fence.id == fenceId; }), fences.end());
    if (fences.size() == oldSize) return false;
    for (int i = 0; i < static_cast<int>(fences.size()); ++i) fences[i].zOrder = i;
    SaveConfig();
    return true;
}

bool FenceManager::BackupLayout(const std::wstring& backupPath) {
    if (!configService || backupPath.empty()) return false;
    for (auto& fence : fences) SyncItemOrder(fence);

    std::vector<DesktopPositionRecord> desktopSnapshot;
    const std::wstring desktop = GetUserDesktopPath();
    for (const auto& item : FileService::ScanFolder(desktop)) {
        POINT position{};
        bool hasLogicalOrigin = false;
        for (const auto& fence : fences) {
            if (!IsLogicalItem(fence.id, item.fullPath)) continue;
            hasLogicalOrigin = GetItemDesktopOrigin(fence.id, item.fullPath, position);
            break;
        }
        if (hasLogicalOrigin ||
            DesktopShellService::GetPathPositionOnDesktop(item.fullPath, position)) {
            desktopSnapshot.push_back({ item.fullPath, position });
        }
    }
    return configService->BackupLayout(fences, desktopSnapshot, backupPath);
}

bool FenceManager::RestoreLayout(const std::wstring& backupPath) {
    if (!configService || backupPath.empty()) return false;
    std::vector<Fence> restoredFences;
    std::vector<DesktopPositionRecord> desktopSnapshot;
    if (!configService->RestoreLayout(backupPath, restoredFences, desktopSnapshot)) return false;

    std::vector<std::wstring> searchDirectories{ GetUserDesktopPath() };
    for (const auto& fence : fences) searchDirectories.push_back(fence.folderPath);
    for (const auto& fence : restoredFences) searchDirectories.push_back(fence.folderPath);

    auto findByName = [&](const std::wstring& name, const std::wstring& excludedPath) {
        for (const auto& directory : searchDirectories) {
            if (directory.empty()) continue;
            const std::wstring candidate = (std::filesystem::path(directory) / name).wstring();
            if (_wcsicmp(candidate.c_str(), excludedPath.c_str()) != 0 &&
                std::filesystem::exists(candidate)) return candidate;
        }
        return std::wstring{};
    };

    FileMoveService mover;
    for (const auto& record : desktopSnapshot) {
        if (std::filesystem::exists(record.path)) continue;
        const std::wstring name = std::filesystem::path(record.path).filename().wstring();
        const std::wstring source = findByName(name, record.path);
        if (source.empty()) continue;
        std::wstring movedPath;
        mover.MoveToDirectory(source, std::filesystem::path(record.path).parent_path().wstring(),
                              false, &movedPath);
    }

    for (const auto& fence : restoredFences) {
        if (fence.isMappedFolder || !fence.memberPaths.empty()) continue;
        FenceStorageService::EnsureFenceFolder(fence.id);
        for (const auto& itemName : fence.itemOrder) {
            const std::wstring target =
                (std::filesystem::path(fence.folderPath) / itemName).wstring();
            if (std::filesystem::exists(target)) continue;
            const std::wstring source = findByName(itemName, target);
            if (source.empty()) continue;
            std::wstring movedPath;
            mover.MoveToDirectory(source, fence.folderPath, false, &movedPath);
        }
    }

    for (const auto& fence : fences) {
        if (!fence.isMappedFolder && !RestoreLogicalDesktopItems(fence.id)) return false;
    }
    fences = std::move(restoredFences);
    for (auto& fence : fences) {
        if (fence.folderPath.empty()) {
            fence.folderPath = FenceStorageService::GetDefaultFenceFolderPath(fence.id);
            fence.isMappedFolder = false;
        }
        if (fence.isMappedFolder) fence.title = fence.folderPath;
    }
    std::sort(fences.begin(), fences.end(),
        [](const Fence& a, const Fence& b) { return a.zOrder < b.zOrder; });
    ApplyDisplayTopology(MonitorService::CaptureTopology());
    RefreshAllFenceItems();
    SaveConfig();
    for (const auto& record : desktopSnapshot) {
        if (std::filesystem::exists(record.path)) {
            DesktopShellService::RestorePathToDesktopPosition(
                record.path, record.position);
        }
    }
    DesktopShellService::RefreshDesktop();
    for (const auto& fence : fences) SyncLogicalDesktopPositions(fence.id);
    return true;
}

bool FenceManager::SetItemDesktopOrigin(const std::wstring& fenceId,
                                         const std::wstring& itemIdentity, POINT position) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return false;
    SyncItemOrder(*fence);
    const auto item = std::find_if(fence->items.begin(), fence->items.end(),
        [&](const DesktopItem& candidate) {
            return SameMemberPath(candidate.fullPath, itemIdentity) ||
                _wcsicmp(candidate.name.c_str(), itemIdentity.c_str()) == 0;
        });
    if (item == fence->items.end()) return false;
    for (size_t index = 0; index < fence->itemOrder.size(); ++index) {
        if (SavedOrderMatchesItem(fence->itemOrder[index], *item)) {
            if (fence->itemDesktopOrigins.size() < fence->itemOrder.size()) {
                fence->itemDesktopOrigins.resize(
                    fence->itemOrder.size(), POINT{ LONG_MIN, LONG_MIN });
            }
            fence->itemDesktopOrigins[index] = position;
            if (fence->itemDesktopOriginPlacements.size() <
                fence->itemOrder.size()) {
                fence->itemDesktopOriginPlacements.resize(
                    fence->itemOrder.size());
            }
            CaptureItemDesktopOriginPlacement(*fence, index, position);
            return true;
        }
    }
    return false;
}

bool FenceManager::GetItemDesktopOrigin(const std::wstring& fenceId,
                                         const std::wstring& itemIdentity, POINT& position) const {
    return ResolveItemDesktopOrigin(
        fenceId, itemIdentity, MonitorService::CaptureTopology(),
        position) == DesktopOriginResolution::Online;
}

DesktopOriginResolution FenceManager::ResolveItemDesktopOrigin(
    const std::wstring& fenceId,
    const std::wstring& itemIdentity,
    const MonitorService::DisplayTopologySnapshot& topology,
    POINT& position) const {
    position = {};
    for (const auto& fence : fences) {
        if (fence.id != fenceId) continue;
        const auto item = std::find_if(fence.items.begin(), fence.items.end(),
            [&](const DesktopItem& candidate) {
                return SameMemberPath(candidate.fullPath, itemIdentity) ||
                    _wcsicmp(candidate.name.c_str(), itemIdentity.c_str()) == 0;
            });
        if (item == fence.items.end()) break;
        for (size_t index = 0; index < fence.itemOrder.size(); ++index) {
            if (!SavedOrderMatchesItem(fence.itemOrder[index], *item)) {
                continue;
            }
            if (index < fence.itemDesktopOriginPlacements.size()) {
                const auto& placement =
                    fence.itemDesktopOriginPlacements[index];
                if (placement.valid) {
                    const auto* monitor = MonitorService::FindMonitorById(
                        topology, placement.monitorId);
                    if (!monitor) return DesktopOriginResolution::Offline;
                    position = MonitorService::MaterializePoint96(
                        placement, *monitor);
                    return DesktopOriginResolution::Online;
                }
            }
            if (index < fence.itemDesktopOrigins.size() &&
                fence.itemDesktopOrigins[index].x != LONG_MIN &&
                fence.itemDesktopOrigins[index].y != LONG_MIN) {
                position = fence.itemDesktopOrigins[index];
                return MonitorService::FindMonitorForPoint(
                           topology, position)
                    ? DesktopOriginResolution::Online
                    : DesktopOriginResolution::Offline;
            }
            return DesktopOriginResolution::Missing;
        }
        break;
    }
    return DesktopOriginResolution::Missing;
}

bool FenceManager::RenameFence(const std::wstring& fenceId, const std::wstring& newTitle) {
    // 空名称不允许保存
    std::wstring trimmed = newTitle;
    // 去除首尾空白
    while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t')) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t')) trimmed.pop_back();
    if (trimmed.empty()) return false;

    Fence* fence = GetFenceById(fenceId);
    if (!fence) return false;
    if (fence->isMappedFolder) return false;
    fence->title = trimmed;
    SaveConfig();
    return true;
}

bool FenceManager::ToggleViewMode(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->kind == FenceKind::Game) return false;
    fence->viewMode = (fence->viewMode == FenceViewMode::Icon) ? FenceViewMode::List : FenceViewMode::Icon;
    fence->scrollOffset = 0;
    LayoutFenceItems(fenceId);
    SaveConfig();
    return true;
}

// 搜索过滤：模糊匹配（子串，不区分大小写）
void FenceManager::ApplySearchFilter(Fence& fence) {
    if (fence.kind == FenceKind::Game) {
        for (auto& item : fence.items) item.filtered = false;
        return;
    }
    if (!fence.searchActive || fence.searchQuery.empty()) {
        for (auto& item : fence.items) item.filtered = false;
        return;
    }
    std::wstring query = fence.searchQuery;
    for (auto& c : query) c = towlower(c);
    for (auto& item : fence.items) {
        std::wstring name = item.name;
        for (auto& c : name) c = towlower(c);
        item.filtered = name.find(query) == std::wstring::npos;
    }
}

void FenceManager::SetFenceSearch(const std::wstring& fenceId, const std::wstring& query) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->kind == FenceKind::Game) return;
    fence->searchActive = true;
    fence->searchQuery = query;
    fence->scrollOffset = 0;
    ApplySearchFilter(*fence);
    LayoutFenceItems(fenceId);
}

void FenceManager::ClearFenceSearch(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return;
    fence->searchActive = false;
    fence->searchQuery.clear();
    fence->scrollOffset = 0;
    for (auto& item : fence->items) item.filtered = false;
    LayoutFenceItems(fenceId);
}

RECT FenceManager::GetContentRect(const Fence& fence) const {
    RECT contentRect = fence.rect;
    contentRect.top += FENCE_TITLE_BAR_HEIGHT;
    if (fence.searchActive) contentRect.top += 36;
    if (contentRect.top > contentRect.bottom) contentRect.top = contentRect.bottom;
    return contentRect;
}

void FenceManager::ClampScrollOffset(Fence& fence) {
    const RECT contentRect = GetContentRect(fence);
    const int viewportHeight = (std::max)(0L, contentRect.bottom - contentRect.top);
    const int contentHeight = GridLayoutService::GetContentHeight(
        contentRect, fence.items, fence.viewMode);
    const int maxOffset = (std::max)(0, contentHeight - viewportHeight);
    fence.scrollOffset = (std::clamp)(fence.scrollOffset, 0, maxOffset);
}

bool FenceManager::ScrollFence(const std::wstring& fenceId, int deltaPixels) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->kind == FenceKind::Game ||
        fence->collapsed || deltaPixels == 0) return false;
    const int previous = fence->scrollOffset;
    fence->scrollOffset += deltaPixels;
    ClampScrollOffset(*fence);
    if (fence->scrollOffset == previous) return false;
    const RECT contentRect = GetContentRect(*fence);
    GridLayoutService::LayoutByMode(contentRect, fence->items, fence->viewMode,
                                    fence->scrollOffset);
    return true;
}

// 重排序：只改变顺序，不保存坐标
bool FenceManager::ReorderItemAtPoint(const std::wstring& fenceId, const std::wstring& itemIdentity, POINT point) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence || fence->kind == FenceKind::Game) return false;

    RECT contentRect = GetContentRect(*fence);
    POINT layoutPoint = point;
    layoutPoint.y += fence->scrollOffset;
    int targetIndex = GridLayoutService::GetNearestVisibleItemIndex(
        contentRect, layoutPoint, fence->items, fence->viewMode);

    const auto source = std::find_if(fence->items.begin(), fence->items.end(),
        [&](const DesktopItem& item) {
            return SameMemberPath(item.fullPath, itemIdentity) ||
                _wcsicmp(item.name.c_str(), itemIdentity.c_str()) == 0;
        });
    if (source != fence->items.end()) {
        DesktopItem moving = std::move(*source);
        const int sourceIndex = static_cast<int>(std::distance(fence->items.begin(), source));
        fence->items.erase(source);
        if (sourceIndex < targetIndex) --targetIndex;
        targetIndex = (std::clamp)(targetIndex, 0, static_cast<int>(fence->items.size()));
        fence->items.insert(fence->items.begin() + targetIndex, std::move(moving));
        GridLayoutService::LayoutByMode(contentRect, fence->items, fence->viewMode,
                                        fence->scrollOffset);
        SyncItemOrder(*fence);
        return true;
    }
    return false;
}

bool FenceManager::ResizeFence(
    const std::wstring& fenceId,
    const RECT& newRect,
    const MonitorService::DisplayTopologySnapshot* topology) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return false;
    if (fence->locked || fence->kind == FenceKind::Game) return false;

    RECT rect = newRect;
    // 应用最小尺寸
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w < GridLayoutService::MIN_FENCE_WIDTH) rect.right = rect.left + GridLayoutService::MIN_FENCE_WIDTH;
    if (h < GridLayoutService::MIN_FENCE_HEIGHT) rect.bottom = rect.top + GridLayoutService::MIN_FENCE_HEIGHT;

    // 不超出桌面可视区域
    if (topology && !topology->signature.empty()) {
        MonitorService::ApplyFenceBounds(rect, *topology);
    } else {
        MonitorService::ApplyFenceBounds(rect);
    }
    // 确保不超出虚拟屏幕
    RECT vs = topology && !topology->signature.empty()
        ? topology->virtualScreen
        : MonitorService::GetVirtualScreen();
    if (rect.left < vs.left) { rect.right += (vs.left - rect.left); rect.left = vs.left; }
    if (rect.top < vs.top) { rect.bottom += (vs.top - rect.top); rect.top = vs.top; }
    if (rect.right > vs.right) rect.right = vs.right;
    if (rect.bottom > vs.bottom) rect.bottom = vs.bottom;

    fence->rect = rect;
    if (!fence->collapsed) {
        fence->expandedHeight = rect.bottom - rect.top;
    }
    LayoutFenceItems(fenceId);
    return true;
}

// 一键整理桌面：扫描桌面文件按类型分类到格子
int FenceManager::AutoOrganizeDesktop() {
    std::wstring desktopPath = GetUserDesktopPath();
    if (desktopPath.empty()) return 0;

    auto scanned = FileService::ScanFolder(desktopPath);
    if (scanned.empty()) return 0;

    // 分类规则
    auto classify = [](const DesktopItem& item) -> std::wstring {
        if (item.isDirectory) return L"文件夹";
        const std::wstring& ext = item.ext;
        if (ext == L".lnk") return L"快捷方式";
        if (ext == L".zip" || ext == L".rar" || ext == L".7z" || ext == L".tar" ||
            ext == L".gz" || ext == L".bz2" || ext == L".xz") return L"压缩包";
        if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" ||
            ext == L".bmp" || ext == L".svg" || ext == L".webp" || ext == L".ico" ||
            ext == L".tiff") return L"图片";
        if (ext == L".txt" || ext == L".doc" || ext == L".docx" || ext == L".pdf" ||
            ext == L".md" || ext == L".xls" || ext == L".xlsx" || ext == L".ppt" ||
            ext == L".pptx" || ext == L".rtf" || ext == L".csv") return L"文档";
        return L"其他文件";
    };

    // 按类别分组
    std::vector<std::wstring> categories = {
        L"文件夹", L"文档", L"图片", L"压缩包", L"快捷方式", L"其他文件"
    };
    std::unordered_map<std::wstring, std::vector<DesktopItem>> grouped;
    for (auto& item : scanned) {
        if (ShellIntegrationService::IsProtectedDesktopItem(item.fullPath)) continue;
        grouped[classify(item)].push_back(item);
    }

    int movedCount = 0;
    for (const auto& cat : categories) {
        auto it = grouped.find(cat);
        if (it == grouped.end() || it->second.empty()) continue;

        // 创建该类别的格子
        RECT workArea = MonitorService::GetPrimaryWorkArea();
        POINT pt{ workArea.left + 100 + (int)(movedCount % 3) * 360,
                  workArea.top + 100 + (int)(movedCount / 3) * 280 };
        Fence* fence = CreateFenceAt(pt, cat);
        if (!fence) continue;

        // 普通格子只记录路径成员关系；一键整理也不能改变桌面源路径。
        for (auto& item : it->second) {
            POINT originalPosition{};
            const bool hasOriginalPosition = DesktopShellService::GetPathPositionOnDesktop(
                item.fullPath, originalPosition);
            if (AddLogicalItem(fence->id, item.fullPath,
                    hasOriginalPosition ? originalPosition
                                        : POINT{ LONG_MIN, LONG_MIN })) {
                ++movedCount;
            }
        }
        RefreshFenceItems(fence->id);
    }
    SaveConfig();
    return movedCount;
}

void FenceManager::SetAllVisible(bool visible) {
    allVisible = visible;
}

void FenceManager::ToggleAllVisibility() {
    allVisible = !allVisible;
}

void FenceManager::RefreshFenceItems(
    const std::wstring& fenceId, bool syncDesktop) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return;

    if (fence->kind == FenceKind::Clock) {
        fence->items.clear();
        return;
    }

    if (fence->folderPath.empty() && fence->memberPaths.empty()) {
        fence->items.clear();
        return;
    }

    std::vector<DesktopItem> scanned;
    // A mapped fence is an explicit directory view. A normal fence is a
    // logical group and must never scan or adopt files from its private
    // storage directory; memberPaths is its only persistent source of truth.
    if (fence->isMappedFolder && !fence->folderPath.empty() &&
        fence->kind != FenceKind::Game) {
        if (!FileService::TryScanFolder(fence->folderPath, scanned)) {
            // A mapped network/removable folder can be briefly unavailable.
            // Preserve the last successful view and, crucially, its ordering.
            return;
        }
    }
    if (!fence->isMappedFolder) {
        for (const auto& memberPath : fence->memberPaths) {
            DesktopItem item;
            if (!FileService::DescribePath(memberPath, item, true)) continue;
            const bool duplicate = std::any_of(scanned.begin(), scanned.end(),
                [&](const DesktopItem& existing) {
                    return SameMemberPath(existing.fullPath, item.fullPath);
                });
            if (!duplicate) scanned.push_back(std::move(item));
        }
    }
    std::vector<DesktopItem> ordered;
    ordered.reserve(scanned.size());

    // 按 itemOrder 排序，但不应用自由坐标
    for (size_t savedIndex = 0; savedIndex < fence->itemOrder.size(); ++savedIndex) {
        const auto& savedName = fence->itemOrder[savedIndex];
        auto it = std::find_if(scanned.begin(), scanned.end(), [&](const DesktopItem& item) {
            return SavedOrderMatchesItem(savedName, item);
        });
        if (it == scanned.end()) continue;
        ordered.push_back(std::move(*it));
        scanned.erase(it);
    }
    for (auto& item : scanned) ordered.push_back(std::move(item));
    fence->items = std::move(ordered);

    RECT contentRect = GetContentRect(*fence);
    ApplySearchFilter(*fence);
    ClampScrollOffset(*fence);
    if (fence->kind == FenceKind::Game) {
        GridLayoutService::LayoutGameLauncher(contentRect, fence->items);
    } else {
        GridLayoutService::LayoutByMode(contentRect, fence->items, fence->viewMode,
                                        fence->scrollOffset);
    }
    SyncItemOrder(*fence);
    if (syncDesktop) {
        SyncLogicalDesktopPositions(fenceId);
    }
}

void FenceManager::LayoutFenceItems(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return;
    RECT contentRect = GetContentRect(*fence);
    ApplySearchFilter(*fence);
    ClampScrollOffset(*fence);
    if (fence->kind == FenceKind::Game) {
        GridLayoutService::LayoutGameLauncher(contentRect, fence->items);
    } else {
        GridLayoutService::LayoutByMode(contentRect, fence->items, fence->viewMode,
                                        fence->scrollOffset);
    }
}

void FenceManager::RefreshAllFenceItems() {
    for (auto& f : fences) {
        RefreshFenceItems(f.id);
    }
}

int FenceManager::HitTestItem(POINT screenPt, std::wstring& outFenceId) const {
    if (!allVisible) { outFenceId = L""; return -1; }
    for (int i = (int)fences.size() - 1; i >= 0; --i) {
        const auto& f = fences[i];
        if (!f.monitorOnline) continue;
        if (!PtInRect(&f.rect, screenPt)) continue;

        RECT titleRect = f.rect;
        titleRect.bottom = titleRect.top + FENCE_TITLE_BAR_HEIGHT;
        if (PtInRect(&titleRect, screenPt)) continue;

        int idx = GridLayoutService::HitTestItem(screenPt, f.items);
        if (idx >= 0) {
            outFenceId = f.id;
            return idx;
        }
    }
    outFenceId = L"";
    return -1;
}

void FenceManager::ClearItemHover(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return;
    for (auto& item : fence->items)
        item.hovered = false;
}

void FenceManager::ClearItemSelection(const std::wstring& fenceId) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return;
    for (auto& item : fence->items)
        item.selected = false;
}

void FenceManager::SetItemHover(POINT screenPt) {
    for (auto& f : fences) {
        for (auto& item : f.items)
            item.hovered = false;
    }

    std::wstring fenceId;
    int idx = HitTestItem(screenPt, fenceId);
    if (idx >= 0) {
        Fence* fence = GetFenceById(fenceId);
        if (fence && idx < (int)fence->items.size()) {
            fence->items[idx].hovered = true;
        }
    }
}

void FenceManager::ClearDropTargetState() {
    for (auto& f : fences)
        f.dropTarget = false;
}

DesktopItem* FenceManager::GetItem(const std::wstring& fenceId, int index) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return nullptr;
    if (index < 0 || index >= (int)fence->items.size()) return nullptr;
    return &fence->items[index];
}

bool FenceManager::CaptureItemDesktopOriginPlacement(
    Fence& fence, size_t index, POINT position) {
    if (fence.itemDesktopOriginPlacements.size() <
        fence.itemOrder.size()) {
        fence.itemDesktopOriginPlacements.resize(
            fence.itemOrder.size());
    }
    if (index >= fence.itemDesktopOriginPlacements.size()) return false;

    const auto topology = MonitorService::CaptureTopology();
    const auto* monitor = MonitorService::FindMonitorForPoint(
        topology, position);
    if (!monitor) {
        fence.itemDesktopOriginPlacements[index] = {};
        return false;
    }
    fence.itemDesktopOriginPlacements[index] =
        MonitorService::CapturePoint96(position, *monitor);
    return fence.itemDesktopOriginPlacements[index].valid;
}

bool FenceManager::MigrateItemDesktopOriginPlacements(
    Fence& fence,
    const MonitorService::DisplayTopologySnapshot& topology) {
    if (fence.itemDesktopOriginPlacements.size() <
        fence.itemOrder.size()) {
        fence.itemDesktopOriginPlacements.resize(
            fence.itemOrder.size());
    }
    bool changed = false;
    for (size_t index = 0; index < fence.itemOrder.size(); ++index) {
        auto& placement = fence.itemDesktopOriginPlacements[index];
        if (placement.valid ||
            index >= fence.itemDesktopOrigins.size()) {
            continue;
        }
        const POINT legacy = fence.itemDesktopOrigins[index];
        if (legacy.x == LONG_MIN || legacy.y == LONG_MIN) continue;
        const auto* monitor = MonitorService::FindMonitorForPoint(
            topology, legacy);
        if (!monitor) continue;
        placement = MonitorService::CapturePoint96(legacy, *monitor);
        changed = changed || placement.valid;
    }
    return changed;
}

void FenceManager::SyncItemOrder(Fence& fence) {
    const std::vector<std::wstring> previousOrder = fence.itemOrder;
    const std::vector<POINT> previousOrigins = fence.itemDesktopOrigins;
    const auto previousOriginPlacements =
        fence.itemDesktopOriginPlacements;
    fence.itemOrder.clear();
    fence.itemOffsets.clear();
    fence.itemDesktopOrigins.clear();
    fence.itemDesktopOriginPlacements.clear();
    fence.itemOrder.reserve(fence.items.size());
    fence.itemDesktopOrigins.reserve(fence.items.size());
    fence.itemDesktopOriginPlacements.reserve(fence.items.size());
    for (const auto& item : fence.items) {
        const std::wstring currentKey = ItemOrderKey(fence, item);
        fence.itemOrder.push_back(currentKey);
        POINT origin{ LONG_MIN, LONG_MIN };
        MonitorService::MonitorRelativePoint originPlacement;
        for (size_t index = 0; index < previousOrder.size(); ++index) {
            if (SavedOrderMatchesItem(previousOrder[index], item)) {
                if (index < previousOrigins.size()) origin = previousOrigins[index];
                if (index < previousOriginPlacements.size()) {
                    originPlacement = previousOriginPlacements[index];
                }
                break;
            }
        }
        fence.itemDesktopOrigins.push_back(origin);
        fence.itemDesktopOriginPlacements.push_back(
            std::move(originPlacement));
    }
}

void FenceManager::SaveConfig() {
    if (configService && configWritesEnabled) {
        configService->SaveFences(fences);
    }
}

void FenceManager::GenerateDefaultFences() {
    RECT workArea = MonitorService::GetPrimaryWorkArea();

    Fence f1;
    f1.id = L"fence_0";
    f1.title = L"项目文件";
    f1.rect = { workArea.left + 100, workArea.top + 100,
                workArea.left + 460, workArea.top + 340 };
    f1.color = 0x00606060;
    f1.opacity = 0.65f;
    f1.locked = false;
    f1.zOrder = 0;
    f1.folderPath = FenceStorageService::GetDefaultFenceFolderPath(f1.id);
    f1.isMappedFolder = false;
    fences.push_back(f1);

    Fence f2;
    f2.id = L"fence_1";
    f2.title = L"常用工具";
    f2.rect = { workArea.left + 500, workArea.top + 100,
                workArea.left + 820, workArea.top + 300 };
    f2.color = 0x00606060;
    f2.opacity = 0.65f;
    f2.locked = false;
    f2.zOrder = 1;
    f2.folderPath = FenceStorageService::GetDefaultFenceFolderPath(f2.id);
    f2.isMappedFolder = false;
    fences.push_back(f2);
}

bool FenceManager::CommitFencePlacement(
    const std::wstring& fenceId,
    const MonitorService::DisplayTopologySnapshot* topology) {
    Fence* fence = GetFenceById(fenceId);
    if (!fence) return false;
    const auto liveTopology = topology && !topology->signature.empty()
        ? *topology
        : MonitorService::CaptureTopology();
    const auto* monitor = FindBestMonitorForRect(liveTopology, fence->rect);
    if (!monitor) return false;
    fence->monitorPlacement = MonitorService::CapturePlacement96(
        fence->rect, fence->expandedHeight, *monitor);
    fence->monitorOnline = true;
    return fence->monitorPlacement.valid;
}

bool FenceManager::ApplyDisplayTopology(
    const MonitorService::DisplayTopologySnapshot& topology) {
    bool changed = false;
    for (auto& fence : fences) {
        if (MigrateItemDesktopOriginPlacements(fence, topology)) {
            changed = true;
        }
        const bool wasOnline = fence.monitorOnline;
        const RECT previousRect = fence.rect;
        const int previousExpandedHeight = fence.expandedHeight;

        if (!fence.monitorPlacement.valid) {
            const auto* legacyMonitor = FindBestMonitorForRect(topology, fence.rect);
            if (!legacyMonitor) {
                fence.monitorOnline = false;
            } else {
                fence.monitorPlacement = MonitorService::CapturePlacement96(
                    fence.rect, fence.expandedHeight, *legacyMonitor);
                NormalizeGameFenceGeometry(fence, *legacyMonitor);
                fence.monitorOnline = true;
            }
        } else {
            const auto* monitor = MonitorService::FindMonitorById(
                topology, fence.monitorPlacement.monitorId);
            if (!monitor) {
                // A USB-C reconnect can replace the CCD target path while
                // keeping the same physical work area. Recover by strong
                // geometric overlap; if there is no matching area, preserve
                // the old anchor and keep the fence offline until the
                // monitor returns.
                const auto* recovered = FindBestMonitorForRect(
                    topology, fence.rect);
                if (!IsSafeReconnectMonitorMatch(
                        topology, fence.rect, recovered)) {
                    fence.monitorOnline = false;
                } else {
                    int expandedHeight = fence.expandedHeight;
                    fence.monitorPlacement =
                        MonitorService::CapturePlacement96(
                            fence.rect, fence.expandedHeight, *recovered);
                    fence.rect = MonitorService::MaterializePlacement96(
                        fence.monitorPlacement, *recovered, &expandedHeight);
                    ClampRuntimeRectToWorkArea(
                        fence.rect, recovered->workArea);
                    fence.expandedHeight =
                        (std::max)(FENCE_TITLE_BAR_HEIGHT, expandedHeight);
                    NormalizeGameFenceGeometry(fence, *recovered);
                    fence.monitorOnline = true;
                    // The monitor's CCD identity changed while it was
                    // disconnected. Force the topology transaction to be
                    // persisted even if the recovered rectangle is equal to
                    // the old one.
                    changed = true;
                }
            } else {
                int expandedHeight = fence.expandedHeight;
                fence.rect = MonitorService::MaterializePlacement96(
                    fence.monitorPlacement, *monitor, &expandedHeight);
                ClampRuntimeRectToWorkArea(fence.rect, monitor->workArea);
                fence.expandedHeight =
                    (std::max)(FENCE_TITLE_BAR_HEIGHT, expandedHeight);
                NormalizeGameFenceGeometry(fence, *monitor);
                fence.monitorOnline = true;
            }
        }

        const bool rectChanged =
            !EqualRect(&previousRect, &fence.rect) ||
            previousExpandedHeight != fence.expandedHeight;
        if (rectChanged && fence.monitorOnline) LayoutFenceItems(fence.id);
        if (rectChanged || wasOnline != fence.monitorOnline) changed = true;
    }
    return changed;
}

bool FenceManager::RepairFencePositions() {
    // Compatibility entry point for older callers. Topology reconciliation
    // never rewrites a missing monitor's persisted placement.
    return ApplyDisplayTopology(MonitorService::CaptureTopology());
}
