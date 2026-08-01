#include "DesktopShellService.h"
#include "DesktopPathService.h"

#include <ocidl.h>
#include <exdisp.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <commctrl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <new>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::unordered_set<std::wstring> suppressedDesktopPaths;
std::mutex suppressedDesktopPathsMutex;

ComPtr<IFolderFilter> desktopPathFilter;
ComPtr<IFolderFilterSite> installedDesktopFilterSite;
ComPtr<IUnknown> installedDesktopFilterSiteIdentity;
HWND installedDesktopFilterWindow = nullptr;
int nativeDesktopUpdateDepth = 0;
constexpr wchar_t NativeDesktopFreezeProperty[] =
    L"DeskTodo.Next.NativeDesktopFreeze";

std::wstring PathKey(const std::wstring& path) {
    std::wstring key = NormalizePathIdentity(path);
    std::transform(key.begin(), key.end(), key.begin(), towlower);
    return key;
}

bool ContainsSuppressedPathKey(const std::wstring& key) {
    std::lock_guard<std::mutex> lock(suppressedDesktopPathsMutex);
    return suppressedDesktopPaths.find(key) != suppressedDesktopPaths.end();
}

bool ContainsSuppressedPath(const std::wstring& path) {
    return ContainsSuppressedPathKey(PathKey(path));
}

void AddSuppressedPathKey(const std::wstring& key) {
    std::lock_guard<std::mutex> lock(suppressedDesktopPathsMutex);
    suppressedDesktopPaths.insert(key);
}

void RemoveSuppressedPathKey(const std::wstring& key) {
    std::lock_guard<std::mutex> lock(suppressedDesktopPathsMutex);
    suppressedDesktopPaths.erase(key);
}

bool HasSuppressedPaths() {
    std::lock_guard<std::mutex> lock(suppressedDesktopPathsMutex);
    return !suppressedDesktopPaths.empty();
}

std::vector<std::wstring> SuppressedPathSnapshot() {
    std::lock_guard<std::mutex> lock(suppressedDesktopPathsMutex);
    return { suppressedDesktopPaths.begin(), suppressedDesktopPaths.end() };
}

void ClearSuppressedPaths() {
    std::lock_guard<std::mutex> lock(suppressedDesktopPathsMutex);
    suppressedDesktopPaths.clear();
}

class DesktopPathFilter final : public IFolderFilter {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IFolderFilter)) {
            *object = static_cast<IFolderFilter*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            static_cast<ULONG>(InterlockedDecrement(&references_));
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetEnumFlags(
        IShellFolder*, PCIDLIST_ABSOLUTE, HWND* window,
        DWORD* flags) override {
        if (!flags) return E_POINTER;
        if (window) *window = nullptr;

        // GetEnumFlags owns this out parameter; leaving it untouched makes a
        // refresh depend on uninitialized host memory. Mirror Explorer's
        // current hidden-item preferences, then let ShouldShow remove only
        // DeskTodo's logical members.
        *flags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;
        DWORD value = 0;
        DWORD valueBytes = sizeof(value);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"Hidden", RRF_RT_REG_DWORD, nullptr,
                &value, &valueBytes) == ERROR_SUCCESS &&
            value == 1) {
            *flags |= SHCONTF_INCLUDEHIDDEN;
        }
        value = 0;
        valueBytes = sizeof(value);
        if (RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"ShowSuperHidden", RRF_RT_REG_DWORD, nullptr,
                &value, &valueBytes) == ERROR_SUCCESS &&
            value != 0) {
            *flags |= SHCONTF_INCLUDESUPERHIDDEN;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ShouldShow(
        IShellFolder* shellFolder, PCIDLIST_ABSOLUTE folderPidl,
        PCUITEMID_CHILD itemPidl) override {
        if (!shellFolder || !itemPidl) return S_OK;

        ComPtr<IShellItem> shellItem;
        HRESULT result = SHCreateItemWithParent(
            folderPidl, shellFolder, itemPidl, IID_PPV_ARGS(&shellItem));
        if (FAILED(result) || !shellItem) {
            shellItem.Reset();
            result = SHCreateItemWithParent(
                nullptr, shellFolder, itemPidl, IID_PPV_ARGS(&shellItem));
        }
        if (FAILED(result) || !shellItem) return S_OK;

        PWSTR filesystemPath = nullptr;
        result = shellItem->GetDisplayName(SIGDN_FILESYSPATH, &filesystemPath);
        if (FAILED(result) || !filesystemPath) return S_OK;
        const bool suppressed = ContainsSuppressedPath(filesystemPath);
        CoTaskMemFree(filesystemPath);
        return suppressed ? S_FALSE : S_OK;
    }

private:
    ~DesktopPathFilter() = default;
    LONG references_ = 1;
};

struct DesktopViewDiscovery {
    HRESULT shellWindows = E_UNEXPECTED;
    HRESULT findDesktop = E_UNEXPECTED;
    HRESULT serviceProvider = E_UNEXPECTED;
    HRESULT shellBrowser = E_UNEXPECTED;
    HRESULT shellView = E_UNEXPECTED;
    HRESULT folderView = E_UNEXPECTED;
    HRESULT viewWindow = E_UNEXPECTED;
};

HWND FindDescendantListView(HWND root) {
    if (!root || !IsWindow(root)) return nullptr;
    wchar_t className[64]{};
    if (GetClassNameW(root, className, static_cast<int>(_countof(className))) &&
        _wcsicmp(className, WC_LISTVIEWW) == 0) {
        return root;
    }

    struct SearchState {
        HWND result = nullptr;
    } state;
    EnumChildWindows(
        root,
        [](HWND child, LPARAM parameter) -> BOOL {
            auto* search = reinterpret_cast<SearchState*>(parameter);
            wchar_t name[64]{};
            if (GetClassNameW(
                    child, name, static_cast<int>(_countof(name))) &&
                _wcsicmp(name, WC_LISTVIEWW) == 0) {
                search->result = child;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));
    return state.result;
}

bool TryGetDesktopFolderView(ComPtr<IFolderView>& folderView, HWND& listViewWindow,
                             DesktopViewDiscovery* discovery = nullptr,
                             ComPtr<IFolderFilterSite>* filterSiteOut = nullptr) {
    listViewWindow = nullptr;
    if (filterSiteOut) filterSiteOut->Reset();

    DesktopViewDiscovery local;
    DesktopViewDiscovery& status = discovery ? *discovery : local;

    ComPtr<IShellWindows> shellWindows;
    status.shellWindows = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                           IID_PPV_ARGS(&shellWindows));
    if (FAILED(status.shellWindows) || !shellWindows) {
        return false;
    }

    VARIANT location{};
    VariantInit(&location);
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    VARIANT empty{};
    VariantInit(&empty);
    long handle = 0;
    ComPtr<IDispatch> dispatch;
    status.findDesktop = shellWindows->FindWindowSW(
        &location, &empty, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH, &dispatch);
    if (FAILED(status.findDesktop) || !dispatch) {
        // Some Explorer generations require VT_EMPTY for SWC_DESKTOP even
        // though others accept CSIDL_DESKTOP as the location discriminator.
        dispatch.Reset();
        status.findDesktop = shellWindows->FindWindowSW(
            &empty, &empty, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH, &dispatch);
    }
    if (FAILED(status.findDesktop) || !dispatch) {
        return false;
    }

    ComPtr<IServiceProvider> serviceProvider;
    status.serviceProvider = dispatch.As(&serviceProvider);
    if (FAILED(status.serviceProvider) || !serviceProvider) {
        return false;
    }

    ComPtr<IShellBrowser> shellBrowser;
    status.shellBrowser = serviceProvider->QueryService(
        SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser));
    if (FAILED(status.shellBrowser) || !shellBrowser) {
        return false;
    }

    ComPtr<IShellView> shellView;
    status.shellView = shellBrowser->QueryActiveShellView(&shellView);
    if (FAILED(status.shellView) || !shellView) {
        return false;
    }

    status.folderView = shellView.As(&folderView);
    if (FAILED(status.folderView) || !folderView) {
        return false;
    }

    if (filterSiteOut) {
        ComPtr<IFolderFilterSite> filterSite;
        if (FAILED(folderView.As(&filterSite)) || !filterSite) {
            filterSite.Reset();
            shellView.As(&filterSite);
        }
        if (!filterSite) {
            shellBrowser.As(&filterSite);
        }
        if (!filterSite) {
            serviceProvider.As(&filterSite);
        }
        if (filterSite) *filterSiteOut = std::move(filterSite);
    }

    // Windows 11 no longer guarantees the historical
    // Progman/SHELLDLL_DefView/SysListView32 hierarchy. The COM view is the
    // authority for both its HWND and item-coordinate space.
    HWND viewWindow = nullptr;
    status.viewWindow = shellView->GetWindow(&viewWindow);
    if (viewWindow && IsWindow(viewWindow)) {
        // IFolderView item coordinates belong to the actual list view. Newer
        // Explorer builds may return a SHELLDLL_DefView container here, so
        // resolve its SysListView32 descendant before converting coordinates.
        listViewWindow = FindDescendantListView(viewWindow);
        if (!listViewWindow) listViewWindow = viewWindow;
        return true;
    }

    // Compatibility fallback for older Explorer builds whose shell view does
    // not expose a usable HWND.
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND shellViewWnd = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    HWND listView = shellViewWnd ? FindWindowExW(shellViewWnd, nullptr, L"SysListView32", L"FolderView") : nullptr;

    if (!listView) {
        HWND worker = nullptr;
        while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) != nullptr) {
            shellViewWnd = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
            if (!shellViewWnd) continue;

            listView = FindWindowExW(shellViewWnd, nullptr, L"SysListView32", L"FolderView");
            if (listView) break;
        }
    }

    listViewWindow = listView;
    return listViewWindow != nullptr;
}

void UninstallDesktopPathFilter() {
    if (installedDesktopFilterSite) {
        installedDesktopFilterSite->SetFilter(nullptr);
    }
    installedDesktopFilterSite.Reset();
    installedDesktopFilterSiteIdentity.Reset();
    installedDesktopFilterWindow = nullptr;
}

bool EnsureDesktopPathFilter(IFolderView* folderView, HWND viewWindow,
                             IFolderFilterSite* discoveredSite = nullptr,
                             bool forceRebind = false) {
    if (!HasSuppressedPaths()) {
        UninstallDesktopPathFilter();
        return false;
    }
    if (!folderView || !viewWindow || !IsWindow(viewWindow)) return false;

    ComPtr<IFolderFilterSite> filterSite;
    if (discoveredSite) {
        filterSite = discoveredSite;
    } else if (folderView) {
        folderView->QueryInterface(IID_PPV_ARGS(&filterSite));
        if (!filterSite) {
            ComPtr<IShellView> shellView;
            if (SUCCEEDED(folderView->QueryInterface(IID_PPV_ARGS(&shellView))) &&
                shellView) {
                shellView->QueryInterface(IID_PPV_ARGS(&filterSite));
            }
        }
    }
    if (!filterSite) return false;

    ComPtr<IUnknown> identity;
    filterSite->QueryInterface(IID_PPV_ARGS(&identity));
    const bool sameBinding = installedDesktopFilterSite &&
        installedDesktopFilterWindow == viewWindow &&
        viewWindow && IsWindow(viewWindow) &&
        ((!identity && !installedDesktopFilterSiteIdentity) ||
         identity.Get() == installedDesktopFilterSiteIdentity.Get());
    if (sameBinding) {
        if (!forceRebind) return true;
        // Explorer can clear a view filter during F5 while keeping the same
        // COM identity and HWND. Reapplying SetFilter is therefore required;
        // identity equality alone is not proof that the filter is still live.
        return desktopPathFilter &&
            SUCCEEDED(filterSite->SetFilter(desktopPathFilter.Get()));
    }

    UninstallDesktopPathFilter();
    if (!desktopPathFilter) {
        desktopPathFilter.Attach(new (std::nothrow) DesktopPathFilter());
    }
    if (!desktopPathFilter ||
        FAILED(filterSite->SetFilter(desktopPathFilter.Get()))) {
        return false;
    }

    installedDesktopFilterSite = filterSite;
    installedDesktopFilterSiteIdentity = identity;
    installedDesktopFilterWindow = viewWindow;
    return true;
}

bool SameFilesystemPath(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(NormalizePathIdentity(left).c_str(),
                    NormalizePathIdentity(right).c_str()) == 0;
}

bool TryFindDesktopChild(IFolderView* folderView, const std::wstring& path,
                         PITEMID_CHILD* childOut) {
    if (!folderView || path.empty() || !childOut) return false;
    *childOut = nullptr;

    ComPtr<IShellFolder> desktopFolder;
    if (FAILED(folderView->GetFolder(IID_PPV_ARGS(&desktopFolder))) ||
        !desktopFolder) return false;

    auto childMatchesPath = [&](PITEMID_CHILD candidate) {
        ComPtr<IShellItem> shellItem;
        PWSTR filesystemPath = nullptr;
        const bool matches = candidate &&
            SUCCEEDED(SHCreateItemWithParent(nullptr, desktopFolder.Get(), candidate,
                                             IID_PPV_ARGS(&shellItem))) && shellItem &&
            SUCCEEDED(shellItem->GetDisplayName(SIGDN_FILESYSPATH, &filesystemPath)) &&
            filesystemPath && SameFilesystemPath(filesystemPath, path);
        if (filesystemPath) CoTaskMemFree(filesystemPath);
        return matches;
    };

    // Fast path for the overwhelmingly common unambiguous leaf name.
    std::wstring fileName = std::filesystem::path(path).filename().wstring();
    ULONG eaten = 0;
    ULONG attributes = 0;
    PITEMID_CHILD parsed = nullptr;
    if (!fileName.empty() && SUCCEEDED(desktopFolder->ParseDisplayName(
            nullptr, nullptr, fileName.data(), &eaten, &parsed, &attributes)) && parsed) {
        if (childMatchesPath(parsed)) {
            *childOut = parsed;
            return true;
        }
        CoTaskMemFree(parsed);
    }

    // The desktop is a merged namespace (user + Public desktop). An absolute
    // filesystem PIDL's last component is not guaranteed to be the relative
    // child PIDL used by this view. Enumerate view children and compare their
    // filesystem paths instead.
    int itemCount = 0;
    if (SUCCEEDED(folderView->ItemCount(SVGIO_ALLVIEW, &itemCount))) {
        for (int index = 0; index < itemCount; ++index) {
            PITEMID_CHILD candidate = nullptr;
            if (FAILED(folderView->Item(index, &candidate)) || !candidate) continue;

            const bool matches = childMatchesPath(candidate);
            if (matches) {
                *childOut = ILClone(candidate);
                CoTaskMemFree(candidate);
                return *childOut != nullptr;
            }
            CoTaskMemFree(candidate);
        }
    }

    return false;
}

bool IsPathPresentInDesktopView(IFolderView* folderView,
                                const std::wstring& path) {
    PITEMID_CHILD child = nullptr;
    if (!TryFindDesktopChild(folderView, path, &child) || !child) return false;
    POINT position{};
    const bool present = SUCCEEDED(folderView->GetItemPosition(child, &position));
    CoTaskMemFree(child);
    return present;
}

bool IsDesktopViewPositionVisible(HWND listView, POINT position) {
    if (!listView || !IsWindow(listView)) return false;

    // Explorer uses two coordinate conventions across Windows 10/11 builds:
    // some desktop views return ListView-client coordinates, while a desktop
    // spanning monitors can return virtual-desktop coordinates (negative on a
    // monitor placed to the left). Accept either convention. A parked item is
    // deliberately far outside both spaces.
    RECT clientRect{};
    if (GetClientRect(listView, &clientRect) &&
        PtInRect(&clientRect, position) != FALSE) {
        return true;
    }

    const RECT virtualRect{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) +
            GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) +
            GetSystemMetrics(SM_CYVIRTUALSCREEN)
    };
    constexpr LONG kVirtualDesktopEdgeTolerance = 64;
    if (position.x >= virtualRect.left - kVirtualDesktopEdgeTolerance &&
        position.x < virtualRect.right + kVirtualDesktopEdgeTolerance &&
        position.y >= virtualRect.top - kVirtualDesktopEdgeTolerance &&
        position.y < virtualRect.bottom + kVirtualDesktopEdgeTolerance) {
        return true;
    }

    // Some per-monitor-DPI combinations return a scaled virtual coordinate
    // that falls just outside GetSystemMetrics' rectangle even though the icon
    // is visible. Do not classify such a coordinate as parked. We only regard
    // the deliberately extreme parking sentinels as hidden; this keeps every
    // ordinary negative secondary-monitor coordinate retryable.
    constexpr LONG kClearlyParkedCoordinate = 8192;
    return position.x > -kClearlyParkedCoordinate &&
           position.x < kClearlyParkedCoordinate &&
           position.y > -kClearlyParkedCoordinate &&
           position.y < kClearlyParkedCoordinate;
}

bool IsPathVisibleInDesktopView(IFolderView* folderView,
                                HWND listView,
                                const std::wstring& path) {
    if (!folderView || !listView || !IsWindow(listView)) return false;
    PITEMID_CHILD child = nullptr;
    if (!TryFindDesktopChild(folderView, path, &child) || !child) {
        // A filter may already have excluded this item from the view.
        return false;
    }

    POINT position{};
    const bool visible = SUCCEEDED(folderView->GetItemPosition(child, &position)) &&
                         IsDesktopViewPositionVisible(listView, position);
    CoTaskMemFree(child);
    return visible;
}

bool HidePathFromDesktopView(IFolderView* folderView,
                             HWND listView,
                             const std::wstring& path) {
    if (!folderView || !listView || !IsWindow(listView)) return false;

    PITEMID_CHILD child = nullptr;
    if (!TryFindDesktopChild(folderView, path, &child) || !child) {
        // A filter may already have excluded this item from the view.
        return true;
    }

    POINT currentPosition{};
    RECT clientRect{};
    const bool positioned = SUCCEEDED(
        folderView->GetItemPosition(child, &currentPosition));
    if (!positioned || !IsDesktopViewPositionVisible(listView, currentPosition)) {
        // Already outside both supported desktop coordinate spaces (for
        // example, from a previous reconciliation pass). Do not move it again.
        CoTaskMemFree(child);
        return true;
    }

    // Prefer parking the exact PIDL outside the desktop viewport. On current
    // Explorer builds RemoveObject can report success but the shell
    // immediately re-enumerates the item, which makes the icon flash back on
    // every reconciliation pass. Parking is idempotent and survives that
    // re-enumeration while leaving the real filesystem path untouched.
    PCUITEMID_CHILD items[1]{ child };
    POINT parkedPoints[] = {
        POINT{ -32768, -32768 },
        POINT{ clientRect.right + 4096, clientRect.bottom + 4096 },
        POINT{ clientRect.right + 4096, -32768 }
    };
    for (POINT parkedPoint : parkedPoints) {
        if (FAILED(folderView->SelectAndPositionItems(
                1, items, &parkedPoint, SVSI_POSITIONITEM))) {
            continue;
        }
        POINT actual{};
        if (SUCCEEDED(folderView->GetItemPosition(child, &actual)) &&
            !IsDesktopViewPositionVisible(listView, actual)) {
            // Tell the desktop view that this exact child was removed after
            // parking it.  This prevents Explorer's auto-arrange pass from
            // immediately materializing the child back at its old cell.
            SHChangeNotify(
                SHCNE_DELETE, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                path.c_str(), nullptr);
            CoTaskMemFree(child);
            return true;
        }
    }

    // Compatibility fallback for Explorer builds that reject positioning the
    // desktop child. Remove only this exact PIDL; never use a numeric
    // list-view index (LVM_DELETEITEM), and never refresh the whole desktop
    // from this retry path.
    ComPtr<IShellFolderView> shellFolderView;
    UINT removedIndex = 0;
    if (SUCCEEDED(folderView->QueryInterface(
            IID_PPV_ARGS(&shellFolderView))) && shellFolderView &&
        SUCCEEDED(shellFolderView->RemoveObject(child, &removedIndex))) {
        CoTaskMemFree(child);
        return true;
    }

    CoTaskMemFree(child);
    return false;
}

POINT PhysicalScreenToLogicalPoint(HWND hwnd, POINT point) {
    if (hwnd && IsWindow(hwnd)) {
        PhysicalToLogicalPointForPerMonitorDPI(hwnd, &point);
    }
    return point;
}

POINT ClampDesktopClientPoint(HWND listView, POINT clientPoint, int iconSize) {
    // clientPoint is the desired icon TOP-LEFT in listview client coords.
    RECT clientRect{};
    if (listView && GetClientRect(listView, &clientRect)) {
        clientPoint.x = (std::clamp)(clientPoint.x, 0L,
                                     (std::max)(0L, clientRect.right - iconSize));
        clientPoint.y = (std::clamp)(clientPoint.y, 0L,
                                     (std::max)(0L, clientRect.bottom - iconSize));
    } else {
        clientPoint.x = (std::max)(0L, clientPoint.x);
        clientPoint.y = (std::max)(0L, clientPoint.y);
    }
    return clientPoint;
}

// Read the desktop logical icon size from the registry (used for clamping).
int ReadDesktopLogicalIconSize() {
    DWORD iconSize = 40;
    DWORD bytes = sizeof(iconSize);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop",
        L"IconSize", RRF_RT_REG_DWORD, nullptr, &iconSize, &bytes) != ERROR_SUCCESS) {
        iconSize = 40;
    }
    return (std::max)(8, static_cast<int>(iconSize));
}

POINT FindNearestEmptyDesktopPoint(IFolderView* folderView,
                                   PCUITEMID_CHILD movingChild,
                                   HWND listView,
                                   POINT desiredPoint,
                                   int iconSize) {
    // desiredPoint is already pre-snapped by SnapPointToDesktopGrid (physical)
    // then converted to logical client coords. Do NOT re-snap here — re-snapping
    // with folderView->GetSpacing() can land on a different cell than the ghost
    // preview showed, causing visible "错位" (misalignment) on drop.
    POINT spacing{ 112, 112 };
    if (!folderView || FAILED(folderView->GetSpacing(&spacing))) {
        SystemParametersInfoW(SPI_ICONHORIZONTALSPACING, 0, &spacing.x, 0);
        SystemParametersInfoW(SPI_ICONVERTICALSPACING, 0, &spacing.y, 0);
    }
    spacing.x = (std::max)(1L, spacing.x);
    spacing.y = (std::max)(1L, spacing.y);

    POINT snapped = ClampDesktopClientPoint(listView, desiredPoint, iconSize);

    int itemCount = 0;
    std::unordered_set<unsigned long long> occupied;
    if (SUCCEEDED(folderView->ItemCount(SVGIO_ALLVIEW, &itemCount))) {
        occupied.reserve(static_cast<size_t>((std::max)(0, itemCount)));
        for (int i = 0; i < itemCount; ++i) {
            PITEMID_CHILD child = nullptr;
            if (FAILED(folderView->Item(i, &child)) || !child) continue;

            const bool isMovingItem = movingChild && ILIsEqual(child, movingChild);
            POINT existing{};
            if (!isMovingItem && SUCCEEDED(folderView->GetItemPosition(child, &existing))) {
                const auto key = (static_cast<unsigned long long>(static_cast<DWORD>(existing.x)) << 32) |
                                 static_cast<DWORD>(existing.y);
                occupied.insert(key);
            }
            CoTaskMemFree(child);
        }
    }

    auto keyOf = [](POINT point) {
        return (static_cast<unsigned long long>(static_cast<DWORD>(point.x)) << 32) |
               static_cast<DWORD>(point.y);
    };
    if (occupied.find(keyOf(snapped)) == occupied.end()) return snapped;

    for (int radius = 1; radius <= 64; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if ((std::max)(std::abs(dx), std::abs(dy)) != radius) continue;
                POINT candidate{ snapped.x + dx * spacing.x,
                                 snapped.y + dy * spacing.y };
                candidate = ClampDesktopClientPoint(listView, candidate, iconSize);
                if (occupied.find(keyOf(candidate)) == occupied.end()) return candidate;
            }
        }
    }
    return snapped;
}

// Snap a listview-client-coord point to the desktop icon grid using the SAME
// spacing source as the real desktop (IFolderView::GetSpacing).  This is the
// single source of truth for "where will the icon actually land" — both the
// ghost preview (via DesktopShellService::SnapPointToDesktopGrid) and the final
// placement (via PositionPathOnDesktop) call this, so they can never disagree.
POINT SnapClientPointToDesktopGrid(IFolderView* folderView, POINT clientPoint) {
    POINT spacing{ 112, 112 };
    if (!folderView || FAILED(folderView->GetSpacing(&spacing))) {
        SystemParametersInfoW(SPI_ICONHORIZONTALSPACING, 0, &spacing.x, 0);
        SystemParametersInfoW(SPI_ICONVERTICALSPACING, 0, &spacing.y, 0);
    }
    spacing.x = (std::max)(1L, spacing.x);
    spacing.y = (std::max)(1L, spacing.y);

    auto snapAxis = [](int value, int step) {
        const int rounded = value >= 0
            ? (value + step / 2) / step
            : (value - step / 2) / step;
        return rounded * step;
    };
    clientPoint.x = snapAxis(clientPoint.x, spacing.x);
    clientPoint.y = snapAxis(clientPoint.y, spacing.y);
    return clientPoint;
}

} // namespace

POINT DesktopShellService::SnapPointToDesktopGrid(POINT pt) {
    // Cache the desktop listview handle briefly — this runs on every mouse-move
    // during drag.  We snap in listview CLIENT coords using
    // IFolderView::GetSpacing(), which is the exact grid Windows uses for
    // native desktop icon placement.  This replaces the old approach of using
    // SystemParametersInfoW(SPI_ICONHORIZONTALSPACING) which can differ from
    // the real desktop grid (DPI virtualization, Windows version differences),
    // causing the ghost preview to land on a different cell than the icon.
    struct GridCache {
        ULONGLONG tick = 0;
        HWND listView = nullptr;
        bool valid = false;
    };
    static GridCache cache;

    const ULONGLONG now = GetTickCount64();
    if (now - cache.tick > 2000) {
        cache.valid = false;
        ComPtr<IFolderView> folderView;
        HWND listView = nullptr;
        if (TryGetDesktopFolderView(folderView, listView) && folderView && listView) {
            cache.listView = listView;
            cache.valid = true;
        }
        cache.tick = now;
    }

    // If we can't reach the real desktop listview, fall back to a
    // SystemParametersInfoW-based snap in physical coords so the UI still works.
    if (!cache.valid || !cache.listView || !IsWindow(cache.listView)) {
        int stepX = 112, stepY = 112;
        SystemParametersInfoW(SPI_ICONHORIZONTALSPACING, 0, &stepX, 0);
        SystemParametersInfoW(SPI_ICONVERTICALSPACING, 0, &stepY, 0);
        stepX = (std::max)(1, stepX);
        stepY = (std::max)(1, stepY);
        HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(info) };
        if (!monitor || !GetMonitorInfoW(monitor, &info)) return pt;
        auto snapAxis = [](int value, int origin, int step) {
            const int relative = value - origin;
            const int rounded = relative >= 0
                ? (relative + step / 2) / step
                : (relative - step / 2) / step;
            return origin + rounded * step;
        };
        pt.x = snapAxis(pt.x, info.rcWork.left, stepX);
        pt.y = snapAxis(pt.y, info.rcWork.top, stepY);
        return pt;
    }

    HWND listView = cache.listView;

    // physical screen → logical screen → listview client coords.
    // Snapping in client coords sidesteps all DPI scaling — GetSpacing()
    // returns logical client pixels and so does ScreenToClient.
    POINT logical = pt;
    PhysicalToLogicalPointForPerMonitorDPI(listView, &logical);
    POINT client = logical;
    ScreenToClient(listView, &client);

    // We need the IFolderView for GetSpacing.  Re-fetch (cheap, COM-cached);
    // if it fails the helper falls back to SystemParametersInfoW.
    ComPtr<IFolderView> folderView;
    HWND dummy = nullptr;
    if (TryGetDesktopFolderView(folderView, dummy) && folderView) {
        client = SnapClientPointToDesktopGrid(folderView.Get(), client);
    }

    // Clamp in client coords (same logic as FindNearestEmptyDesktopPoint).
    const int logicalIconSize = ReadDesktopLogicalIconSize();
    client = ClampDesktopClientPoint(listView, client, logicalIconSize);

    // client → logical screen → physical screen.
    POINT logicalScreen = client;
    ClientToScreen(listView, &logicalScreen);
    POINT physical = logicalScreen;
    LogicalToPhysicalPointForPerMonitorDPI(listView, &physical);
    return physical;
}

bool DesktopShellService::PositionPathOnDesktop(const std::wstring& path, POINT physicalScreenPoint) {
    // physicalScreenPoint is the desired icon TOP-LEFT in physical screen coords.
    if (path.empty()) {
        return false;
    }

    const std::wstring desktopDirectory = std::filesystem::path(path).parent_path().wstring();
    // Non-blocking: let the retry loop (6x25ms) handle shell refresh.
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW,
                   desktopDirectory.c_str(), nullptr);

    const int logicalIconSize = ReadDesktopLogicalIconSize();

    bool positioned = false;
    // Reduced from 12x50ms=600ms to 6x25ms=150ms max. The shell usually picks
    // up the new file within the first retry; the loop only blocks when the
    // folder view hasn't reflected the move yet.
    for (int attempt = 0; attempt < 6 && !positioned; ++attempt) {
        ComPtr<IFolderView> folderView;
        HWND listView = nullptr;
        if (TryGetDesktopFolderView(folderView, listView) && folderView) {
            PITEMID_CHILD child = nullptr;
            if (!TryFindDesktopChild(folderView.Get(), path, &child)) {
                if (attempt + 1 < 6) Sleep(25);
                continue;
            }
            POINT logicalScreenPoint = PhysicalScreenToLogicalPoint(listView, physicalScreenPoint);
            POINT clientPoint = logicalScreenPoint;
            if (listView && IsWindow(listView)) {
                ScreenToClient(listView, &clientPoint);
            }

            // Re-snap in client coords using IFolderView::GetSpacing() — the
            // SAME grid the ghost preview used (via SnapPointToDesktopGrid).
            // This guarantees the icon lands exactly on the cell the user saw,
            // even if physical→client rounding introduced a sub-pixel offset.
            clientPoint = SnapClientPointToDesktopGrid(folderView.Get(), clientPoint);

            PCUITEMID_CHILD items[1]{ child };
            POINT points[1]{ FindNearestEmptyDesktopPoint(
                folderView.Get(), items[0], listView, clientPoint, logicalIconSize) };
            const bool requested = SUCCEEDED(folderView->SelectAndPositionItems(
                1, items, points, SVSI_POSITIONITEM | SVSI_SELECT | SVSI_FOCUSED));
            POINT actual{};
            RECT clientRect{};
            positioned = requested &&
                SUCCEEDED(folderView->GetItemPosition(child, &actual)) &&
                GetClientRect(listView, &clientRect) && PtInRect(&clientRect, actual);
            CoTaskMemFree(child);
        }

        if (!positioned) {
            Sleep(25);
        }
    }

    if (positioned) {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW,
                       desktopDirectory.c_str(), nullptr);
    }
    return positioned;
}

bool DesktopShellService::RestorePathToDesktopPosition(
    const std::wstring& path, POINT physicalScreenPoint) {
    if (path.empty()) return false;

    // The saved point came from GetPathPositionOnDesktop. Convert it back to
    // the same desktop-view client coordinate space and submit it unchanged.
    // Do not snap again: the grid origin is not necessarily (0,0), and a second
    // snap breaks coordinates on negative-coordinate secondary monitors.
    for (int attempt = 0; attempt < 10; ++attempt) {
        ComPtr<IFolderView> folderView;
        HWND listView = nullptr;
        if (TryGetDesktopFolderView(folderView, listView) && folderView && listView) {
            PITEMID_CHILD child = nullptr;
            if (TryFindDesktopChild(folderView.Get(), path, &child) && child) {
                POINT logicalScreen = PhysicalScreenToLogicalPoint(
                    listView, physicalScreenPoint);
                POINT requestedClient = logicalScreen;
                ScreenToClient(listView, &requestedClient);

                PCUITEMID_CHILD items[1]{ child };
                POINT points[1]{ requestedClient };
                const bool requested = SUCCEEDED(folderView->SelectAndPositionItems(
                    1, items, points, SVSI_POSITIONITEM));
                POINT actualClient{};
                bool exact = requested &&
                    SUCCEEDED(folderView->GetItemPosition(child, &actualClient));
                CoTaskMemFree(child);

                if (exact) {
                    POINT actualScreen = actualClient;
                    ClientToScreen(listView, &actualScreen);
                    LogicalToPhysicalPointForPerMonitorDPI(listView, &actualScreen);
                    exact = std::abs(actualScreen.x - physicalScreenPoint.x) <= 1 &&
                            std::abs(actualScreen.y - physicalScreenPoint.y) <= 1;
                }
                if (exact) return true;
            }
        }
        Sleep(25);
    }
    return false;
}

bool DesktopShellService::GetPathPositionOnDesktop(const std::wstring& path,
                                                   POINT& physicalScreenPoint) {
    // Returns the icon TOP-LEFT in physical screen coords.
    physicalScreenPoint = {};
    if (path.empty()) return false;

    ComPtr<IFolderView> folderView;
    HWND listView = nullptr;
    if (!TryGetDesktopFolderView(folderView, listView) || !folderView || !listView) return false;

    PITEMID_CHILD child = nullptr;
    if (!TryFindDesktopChild(folderView.Get(), path, &child)) return false;
    POINT logicalClient{};
    const bool found = SUCCEEDED(folderView->GetItemPosition(child, &logicalClient));
    CoTaskMemFree(child);
    if (!found) return false;

    POINT logicalScreen = logicalClient;
    ClientToScreen(listView, &logicalScreen);
    LogicalToPhysicalPointForPerMonitorDPI(listView, &logicalScreen);
    physicalScreenPoint = logicalScreen;
    return true;
}

HWND DesktopShellService::GetNativeDesktopIconWindow() {
    ComPtr<IFolderView> folderView;
    HWND listView = nullptr;
    if (!TryGetDesktopFolderView(folderView, listView) ||
        !listView || !IsWindow(listView)) {
        return nullptr;
    }
    wchar_t className[64]{};
    if (!GetClassNameW(
            listView, className,
            static_cast<int>(_countof(className))) ||
        _wcsicmp(className, WC_LISTVIEWW) != 0) {
        return nullptr;
    }
    return listView;
}

HWND DesktopShellService::GetNativeDesktopHostWindow() {
    const HWND listView = GetNativeDesktopIconWindow();
    if (!listView || !IsWindow(listView)) return nullptr;
    const HWND root = GetAncestor(listView, GA_ROOT);
    return root && root != GetDesktopWindow() && IsWindow(root)
        ? root : nullptr;
}

bool DesktopShellService::BeginNativeDesktopUpdate() {
    ++nativeDesktopUpdateDepth;
    // Never pause or hide Explorer's complete desktop ListView. On newer
    // Explorer builds cross-process WM_SETREDRAW can remove WS_VISIBLE and
    // leave every desktop icon blank. Suppression is performed per item by
    // the folder filter and RemoveObject instead.
    return GetNativeDesktopIconWindow() != nullptr;
}

void DesktopShellService::EndNativeDesktopUpdate() {
    if (nativeDesktopUpdateDepth <= 0) return;
    --nativeDesktopUpdateDepth;
}

bool DesktopShellService::RestoreNativeDesktopView() {
    HWND desktopIconWindow = GetNativeDesktopIconWindow();
    if (!desktopIconWindow || !IsWindow(desktopIconWindow)) return false;

    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(
        desktopIconWindow, WM_SETREDRAW, TRUE, 0,
        SMTO_ABORTIFHUNG, 250, &ignored);
    const bool hiddenByLegacyFreeze =
        GetPropW(
            desktopIconWindow,
            NativeDesktopFreezeProperty) != nullptr;
    RemovePropW(
        desktopIconWindow, NativeDesktopFreezeProperty);
    if (hiddenByLegacyFreeze && !IsWindowVisible(desktopIconWindow)) {
        ShowWindow(desktopIconWindow, SW_SHOWNA);
    }
    RedrawWindow(
        desktopIconWindow, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE |
        RDW_ALLCHILDREN | RDW_UPDATENOW);
    return true;
}

bool DesktopShellService::SetPathSuppressedOnDesktop(
    const std::wstring& path, bool suppressed) {
    if (path.empty()) return false;
    struct NativeDesktopUpdateGuard {
        NativeDesktopUpdateGuard() {
            DesktopShellService::BeginNativeDesktopUpdate();
        }
        ~NativeDesktopUpdateGuard() {
            DesktopShellService::EndNativeDesktopUpdate();
        }
    } updateGuard;
    const std::wstring key = PathKey(path);

    if (!suppressed) {
        RemoveSuppressedPathKey(key);
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (!HasSuppressedPaths()) UninstallDesktopPathFilter();
            return true;
        }

        ComPtr<IFolderView> folderView;
        HWND viewWindow = nullptr;
        ComPtr<IFolderFilterSite> filterSite;
        if (!TryGetDesktopFolderView(
                folderView, viewWindow, nullptr, &filterSite) ||
            !folderView || !viewWindow) {
            if (!HasSuppressedPaths()) UninstallDesktopPathFilter();
            // The real path already exists and has been removed from the
            // suppression set. Explorer can finish rebuilding asynchronously;
            // do not reject a fence-to-desktop drop on transient discovery.
            return true;
        }
        // The installed filter reads suppressedDesktopPaths dynamically. Keep
        // it for remaining logical members, or remove it before restoring the
        // final member.
        EnsureDesktopPathFilter(folderView.Get(), viewWindow, filterSite.Get());

        bool restoredToDesktopView = false;
        if (IsPathPresentInDesktopView(folderView.Get(), path)) {
            restoredToDesktopView = true;
        } else {
            // Compatibility fast path. AddObject is scoped to this one desktop
            // view; unlike a fake SHCNE_CREATE notification it cannot perturb
            // an Explorer window that is showing the real Desktop directory.
            PITEMID_CHILD child = nullptr;
            ComPtr<IShellFolderView> shellFolderView;
            UINT itemIndex = 0;
            if (TryFindDesktopChild(folderView.Get(), path, &child) && child &&
                SUCCEEDED(folderView.As(&shellFolderView)) && shellFolderView) {
                restoredToDesktopView = SUCCEEDED(
                    shellFolderView->AddObject(child, &itemIndex));
            }
            if (child) CoTaskMemFree(child);
        }

        if (!restoredToDesktopView) {
            ComPtr<IShellView> shellView;
            if (SUCCEEDED(folderView.As(&shellView)) && shellView) {
                shellView->Refresh();
            }
        }
        ReconcileSuppressedDesktopPaths();
        // Visibility is finalized by the following positioning call or the
        // next Shell notification. Avoid a synchronous 200 ms polling loop on
        // the pointer-up path.
        return true;
    }

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ||
        !IsDesktopFilesystemPath(path)) {
        RemoveSuppressedPathKey(key);
        if (!HasSuppressedPaths()) UninstallDesktopPathFilter();
        return false;
    }
    AddSuppressedPathKey(key);

    ComPtr<IFolderView> folderView;
    HWND listView = nullptr;
    ComPtr<IFolderFilterSite> filterSite;
    if (!TryGetDesktopFolderView(
            folderView, listView, nullptr, &filterSite) ||
        !folderView || !listView) {
        // Explorer may be rebuilding its desktop view at the exact moment a
        // drop completes. Keep the logical membership queued and let the
        // batch reconciliation timer bind the replacement view; a transient
        // COM discovery failure must never reject the user's drop.
        return true;
    }
    EnsureDesktopPathFilter(
        folderView.Get(), listView, filterSite.Get());

    // Remove the current native entry immediately using this exact PIDL. On
    // Explorer builds that do not expose IShellFolderView::RemoveObject, the
    // fallback parks the same PIDL outside the list-view viewport. It never
    // sends a numeric LVM_DELETEITEM and never refreshes the whole desktop.
    HidePathFromDesktopView(folderView.Get(), listView, path);
    // The valid path is now durably queued even if Explorer is between views.
    // Reconciliation will retry without making the drag operation fail.
    return true;
}

bool DesktopShellService::IsPathSuppressedOnDesktop(const std::wstring& path) {
    const std::wstring key = PathKey(path);
    if (!ContainsSuppressedPathKey(key)) return false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        RemoveSuppressedPathKey(key);
        if (!HasSuppressedPaths()) UninstallDesktopPathFilter();
        return false;
    }
    // This query is used from layout and timer hot paths. Membership in the
    // suppression set is the source of truth; COM view verification is done
    // once per batch by ReconcileSuppressedDesktopPaths().
    return true;
}

bool DesktopShellService::ReconcileSuppressedDesktopPaths() {
    auto paths = SuppressedPathSnapshot();
    if (paths.empty()) {
        UninstallDesktopPathFilter();
        return true;
    }

    for (const auto& path : paths) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ||
            !IsDesktopFilesystemPath(path)) {
            RemoveSuppressedPathKey(PathKey(path));
        }
    }
    paths = SuppressedPathSnapshot();
    if (paths.empty()) {
        UninstallDesktopPathFilter();
        return true;
    }

    ComPtr<IFolderView> folderView;
    HWND listView = nullptr;
    ComPtr<IFolderFilterSite> filterSite;
    if (!TryGetDesktopFolderView(
            folderView, listView, nullptr, &filterSite) ||
        !folderView || !listView) {
        return false;
    }

    const bool filterInstalled = EnsureDesktopPathFilter(
        folderView.Get(), listView, filterSite.Get());

    struct NativeDesktopUpdateGuard {
        NativeDesktopUpdateGuard() {
            DesktopShellService::BeginNativeDesktopUpdate();
        }
        ~NativeDesktopUpdateGuard() {
            DesktopShellService::EndNativeDesktopUpdate();
        }
    } updateGuard;

    // Reconcile by exact PIDL and park only members that are currently visible.
    // This is intentionally idempotent: a parked member is already outside the
    // viewport on the next timer tick, so the desktop is not refreshed forever.
    for (const auto& path : paths) {
        if (IsPathVisibleInDesktopView(folderView.Get(), listView, path)) {
            HidePathFromDesktopView(folderView.Get(), listView, path);
        }
    }

    if (listView && IsWindow(listView)) {
        RedrawWindow(
            listView, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
            RDW_UPDATENOW | RDW_ERASENOW);
    }
    return filterInstalled;
}

void DesktopShellService::ForgetPathSuppression(const std::wstring& path) {
    RemoveSuppressedPathKey(PathKey(path));
    if (!HasSuppressedPaths()) UninstallDesktopPathFilter();
}

void DesktopShellService::ClearSuppressionState() {
    const bool hadSuppressedPaths = HasSuppressedPaths();
    ClearSuppressedPaths();
    UninstallDesktopPathFilter();

    if (!hadSuppressedPaths) return;
    ComPtr<IFolderView> folderView;
    HWND viewWindow = nullptr;
    if (TryGetDesktopFolderView(folderView, viewWindow) && folderView) {
        ComPtr<IShellView> shellView;
        if (SUCCEEDED(folderView.As(&shellView)) && shellView) {
            shellView->Refresh();
        }
        if (viewWindow && IsWindow(viewWindow)) {
            RedrawWindow(viewWindow, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
    }
}

std::wstring DesktopShellService::DiagnoseDesktopView(const std::wstring& path) {
    ComPtr<IFolderView> folderView;
    HWND listView = nullptr;
    DesktopViewDiscovery discovery;
    const bool viewFound = TryGetDesktopFolderView(folderView, listView, &discovery);

    ComPtr<IShellView> shellView;
    ComPtr<IFolderFilterSite> viewFilterSite;
    ComPtr<IFolderFilterSite> shellFilterSite;
    HRESULT viewFilterHr = folderView
        ? folderView.As(&viewFilterSite) : E_NOINTERFACE;
    HRESULT shellFilterHr = E_NOINTERFACE;
    if (folderView && SUCCEEDED(folderView.As(&shellView)) && shellView) {
        shellFilterHr = shellView.As(&shellFilterSite);
    }

    ComPtr<IShellFolder> desktopFolder;
    HRESULT folderHr = folderView
        ? folderView->GetFolder(IID_PPV_ARGS(&desktopFolder)) : E_NOINTERFACE;
    HRESULT parseNameHr = E_NOINTERFACE;
    PITEMID_CHILD child = nullptr;
    if (desktopFolder && !path.empty()) {
        std::wstring fileName = std::filesystem::path(path).filename().wstring();
        ULONG eaten = 0;
        ULONG attributes = 0;
        parseNameHr = desktopFolder->ParseDisplayName(
            nullptr, nullptr, fileName.data(), &eaten, &child, &attributes);
    }
    POINT itemPosition{};
    HRESULT positionHr = child && folderView
        ? folderView->GetItemPosition(child, &itemPosition) : E_NOINTERFACE;
    if (child) CoTaskMemFree(child);

    std::wostringstream output;
    output << L"shell-windows-hr=0x" << std::hex
           << static_cast<unsigned long>(discovery.shellWindows)
           << L" find-desktop-hr=0x" << static_cast<unsigned long>(discovery.findDesktop)
           << L" service-provider-hr=0x" << static_cast<unsigned long>(discovery.serviceProvider)
           << L" shell-browser-hr=0x" << static_cast<unsigned long>(discovery.shellBrowser)
           << L" shell-view-hr=0x" << static_cast<unsigned long>(discovery.shellView)
           << L" folder-view-hr=0x" << static_cast<unsigned long>(discovery.folderView)
           << L" get-window-hr=0x" << static_cast<unsigned long>(discovery.viewWindow)
           << std::dec << L" view=" << viewFound
           << L" listview=" << (listView && IsWindow(listView))
           << L" folder-hr=0x" << std::hex << static_cast<unsigned long>(folderHr)
           << L" parse-relative-hr=0x" << static_cast<unsigned long>(parseNameHr)
           << L" position-relative-hr=0x" << static_cast<unsigned long>(positionHr)
           << L" view-filter-site-hr=0x" << static_cast<unsigned long>(viewFilterHr)
           << L" shell-filter-site-hr=0x" << static_cast<unsigned long>(shellFilterHr);
    return output.str();
}

void DesktopShellService::RefreshDesktop() {
    struct NativeDesktopUpdateGuard {
        NativeDesktopUpdateGuard() {
            DesktopShellService::BeginNativeDesktopUpdate();
        }
        ~NativeDesktopUpdateGuard() {
            DesktopShellService::EndNativeDesktopUpdate();
        }
    } updateGuard;
    ComPtr<IFolderView> folderView;
    HWND listView = nullptr;
    ComPtr<IFolderFilterSite> filterSite;
    if (TryGetDesktopFolderView(
            folderView, listView, nullptr, &filterSite) && folderView) {
        EnsureDesktopPathFilter(
            folderView.Get(), listView, filterSite.Get(), true);
        ComPtr<IShellView> shellView;
        if (SUCCEEDED(folderView.As(&shellView)) && shellView) {
            shellView->Refresh();
        }
    }
    // Refresh may replace the view synchronously. Discover it again and remove
    // only the logical members; never suspend the complete desktop ListView.
    ReconcileSuppressedDesktopPaths();
}

bool DesktopShellService::IsPointOnDesktop(POINT physicalScreenPoint) {
    // The overlay owns mouse capture while an item is being dragged. Windows
    // below it must not change whether the item can leave its fence; otherwise
    // crossing any application window makes the target invalid and the item
    // visibly jumps back. A desktop drop is valid anywhere in a monitor's
    // usable work area, including negative-coordinate secondary monitors.
    HMONITOR monitor = MonitorFromPoint(physicalScreenPoint, MONITOR_DEFAULTTONULL);
    MONITORINFO info{ sizeof(info) };
    return monitor && GetMonitorInfoW(monitor, &info) &&
           PtInRect(&info.rcWork, physicalScreenPoint);
}

ItemContextMenuResult DesktopShellService::ShowItemContextMenu(
    HWND owner, const std::wstring& path, POINT screenPoint) {
    if (!owner || path.empty()) {
        return ItemContextMenuResult::Cancelled;
    }

    PIDLIST_ABSOLUTE absolutePidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &absolutePidl, 0, nullptr)) || !absolutePidl) {
        return ItemContextMenuResult::Cancelled;
    }

    ComPtr<IShellFolder> parentFolder;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(absolutePidl, IID_PPV_ARGS(&parentFolder), &child)) || !parentFolder || !child) {
        CoTaskMemFree(absolutePidl);
        return ItemContextMenuResult::Cancelled;
    }

    ComPtr<IContextMenu> contextMenu;
    PCUITEMID_CHILD itemArray[1]{ child };
    if (FAILED(parentFolder->GetUIObjectOf(owner, 1, itemArray, IID_IContextMenu,
                                           nullptr, reinterpret_cast<void**>(contextMenu.GetAddressOf()))) ||
        !contextMenu) {
        CoTaskMemFree(absolutePidl);
        return ItemContextMenuResult::Cancelled;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        CoTaskMemFree(absolutePidl);
        return ItemContextMenuResult::Cancelled;
    }

    ItemContextMenuResult result = ItemContextMenuResult::Cancelled;
    if (contextMenu->QueryContextMenu(menu, 0, 1, 0x7FFF, CMF_NORMAL | CMF_EXPLORE) >= 0) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 0x9000, L"备份桌面布局");
        AppendMenuW(menu, MF_STRING, 0x9001, L"还原桌面布局");
        UINT selected = TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                         screenPoint.x, screenPoint.y, owner, nullptr);
        if (selected == 0x9000) {
            result = ItemContextMenuResult::BackupLayout;
        } else if (selected == 0x9001) {
            result = ItemContextMenuResult::RestoreLayout;
        } else if (selected != 0) {
            result = ItemContextMenuResult::ShellCommand;
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invoke.hwnd = owner;
            invoke.lpVerb = MAKEINTRESOURCEA(selected - 1);
            invoke.lpVerbW = MAKEINTRESOURCEW(selected - 1);
            invoke.nShow = SW_SHOWNORMAL;
            invoke.ptInvoke = screenPoint;
            if (FAILED(contextMenu->InvokeCommand(
                    reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke)))) {
                result = ItemContextMenuResult::Cancelled;
            }
        } else {
            result = ItemContextMenuResult::Cancelled;
        }
    }

    DestroyMenu(menu);
    CoTaskMemFree(absolutePidl);
    return result;
}
