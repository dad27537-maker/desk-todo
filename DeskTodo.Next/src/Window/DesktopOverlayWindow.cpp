#include "DesktopOverlayWindow.h"
#include "App/AppCommand.h"
#include "Managers/FenceManager.h"
#include "Services/MonitorService.h"
#include "Services/TooltipService.h"
#include "Services/FileService.h"
#include "Services/FileMoveService.h"
#include "Services/GridLayoutService.h"
#include "Services/DesktopPathService.h"
#include "Services/DesktopShellService.h"
#include "Services/DesktopIconLayoutService.h"
#include "Services/IconCache.h"
#include "Services/LayoutBackupDialog.h"
#include "Services/ShellIntegrationService.h"
#include "Window/SettingsWindow.h"
#include "Window/AppWindowMessages.h"
#include "Window/PomodoroWidgetWindow.h"
#include "Window/TodoWidgetWindow.h"
#include "Window/LanTransferWindow.h"
#include "Components/BlackHole/BlackHoleBackground.h"
#include "Window/BlackHoleSettingsWindow.h"
#include "Window/ScreenshotTool.h"
#include <windowsx.h>
#include <dbt.h>
#include <filesystem>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <algorithm>
#include <cstring>
#include <new>

static const wchar_t WINDOW_CLASS[] = L"DeskTodoNext_Overlay";
static constexpr int DRAG_START_THRESHOLD = 4;
static constexpr int ITEM_DRAG_START_THRESHOLD = 4;
static constexpr UINT_PTR LOGICAL_ICON_SYNC_TIMER_ID = 1006;
static constexpr UINT_PTR CAPTURE_START_TIMER_ID = 1008;
static constexpr UINT_PTR DISPLAY_TOPOLOGY_TIMER_ID = 1009;
static constexpr UINT WM_DESKTODO_DESKTOP_VIEW_EVENT = WM_APP + 70;
static constexpr UINT WM_DESKTODO_ASYNC_SHOW = WM_APP + 71;
static constexpr UINT WM_DESKTODO_ASYNC_SETTINGS = WM_APP + 72;
static HWND desktopViewEventOverlay = nullptr;
static HWND desktopViewEventListView = nullptr;

static void CALLBACK DesktopViewWinEventProc(
    HWINEVENTHOOK, DWORD event, HWND eventWindow,
    LONG objectId, LONG, DWORD, DWORD) {
    if ((event != EVENT_OBJECT_CREATE &&
         event != EVENT_OBJECT_SHOW &&
         event != EVENT_OBJECT_REORDER) ||
        !desktopViewEventOverlay ||
        !IsWindow(desktopViewEventOverlay) ||
        (objectId != OBJID_CLIENT && objectId != OBJID_WINDOW)) {
        return;
    }
    if (eventWindow != desktopViewEventListView) {
        wchar_t className[64]{};
        if (!eventWindow ||
            !GetClassNameW(
                eventWindow, className,
                static_cast<int>(_countof(className))) ||
            _wcsicmp(className, WC_LISTVIEWW) != 0) {
            return;
        }
        desktopViewEventListView = eventWindow;
    }
    PostMessageW(
        desktopViewEventOverlay,
        WM_DESKTODO_DESKTOP_VIEW_EVENT, 0, 0);
}

class DesktopOverlayWindow::DropTarget final : public IDropTarget {
public:
    explicit DropTarget(DesktopOverlayWindow* owner) : owner_(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) ||
            IsEqualIID(iid, IID_IDropTarget)) {
            *object = static_cast<IDropTarget*>(this);
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

    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject* dataObject, DWORD, POINTL point,
        DWORD* effect) override {
        acceptsFiles_ = HasFileDrop(dataObject);
        if (effect) {
            *effect = acceptsFiles_
                ? ResolveEffect(*effect, ToPoint(point))
                : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(
        DWORD, POINTL point, DWORD* effect) override {
        if (effect) {
            *effect = acceptsFiles_
                ? ResolveEffect(*effect, ToPoint(point))
                : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        acceptsFiles_ = false;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject* dataObject, DWORD, POINTL point,
        DWORD* effect) override {
        const POINT screenPoint = ToPoint(point);
        const DWORD requested = effect ? *effect : DROPEFFECT_COPY;
        std::vector<std::wstring> paths =
            ExtractDroppedPaths(dataObject);

        const bool handled = owner_ && !paths.empty() &&
            owner_->HandleDroppedPaths(paths, screenPoint);
        if (effect) {
            *effect = handled
                ? ResolveEffect(requested, screenPoint)
                : DROPEFFECT_NONE;
        }
        acceptsFiles_ = false;
        return handled ? S_OK : S_FALSE;
    }

private:
    static POINT ToPoint(POINTL point) {
        return {
            static_cast<LONG>(point.x),
            static_cast<LONG>(point.y)
        };
    }

    static bool HasFileDrop(IDataObject* dataObject) {
        if (!dataObject) return false;
        FORMATETC fileDrop{
            static_cast<CLIPFORMAT>(CF_HDROP), nullptr,
            DVASPECT_CONTENT, -1, TYMED_HGLOBAL
        };
        if (SUCCEEDED(dataObject->QueryGetData(&fileDrop))) {
            return true;
        }
        FORMATETC shellItems{
            static_cast<CLIPFORMAT>(
                RegisterClipboardFormatW(CFSTR_SHELLIDLIST)),
            nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL
        };
        return SUCCEEDED(
            dataObject->QueryGetData(&shellItems));
    }

    static std::vector<std::wstring> ExtractDroppedPaths(
        IDataObject* dataObject) {
        std::vector<std::wstring> paths;
        if (!dataObject) return paths;

        FORMATETC fileDrop{
            static_cast<CLIPFORMAT>(CF_HDROP), nullptr,
            DVASPECT_CONTENT, -1, TYMED_HGLOBAL
        };
        STGMEDIUM medium{};
        if (SUCCEEDED(
                dataObject->GetData(&fileDrop, &medium))) {
            HDROP drop =
                reinterpret_cast<HDROP>(medium.hGlobal);
            const UINT count =
                DragQueryFileW(
                    drop, 0xFFFFFFFF, nullptr, 0);
            paths.reserve(count);
            for (UINT index = 0; index < count; ++index) {
                const UINT length =
                    DragQueryFileW(
                        drop, index, nullptr, 0);
                if (!length) continue;
                std::vector<wchar_t> buffer(length + 1);
                if (DragQueryFileW(
                        drop, index, buffer.data(),
                        static_cast<UINT>(buffer.size()))) {
                    paths.emplace_back(buffer.data());
                }
            }
            ReleaseStgMedium(&medium);
            if (!paths.empty()) return paths;
        }

        FORMATETC shellItems{
            static_cast<CLIPFORMAT>(
                RegisterClipboardFormatW(CFSTR_SHELLIDLIST)),
            nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL
        };
        medium = {};
        if (FAILED(
                dataObject->GetData(&shellItems, &medium))) {
            return paths;
        }

        const SIZE_T byteCount =
            GlobalSize(medium.hGlobal);
        const auto* ida = static_cast<const CIDA*>(
            GlobalLock(medium.hGlobal));
        if (ida && byteCount >= sizeof(CIDA) &&
            ida->cidl <= 65535) {
            const SIZE_T offsetCount =
                static_cast<SIZE_T>(ida->cidl) + 1;
            const SIZE_T headerSize =
                sizeof(UINT) + offsetCount * sizeof(UINT);
            if (headerSize <= byteCount) {
                const auto* bytes =
                    reinterpret_cast<const BYTE*>(ida);
                const UINT parentOffset = ida->aoffset[0];
                if (parentOffset < byteCount) {
                    const auto* parent =
                        reinterpret_cast<PCIDLIST_ABSOLUTE>(
                            bytes + parentOffset);
                    paths.reserve(ida->cidl);
                    for (UINT index = 0;
                         index < ida->cidl; ++index) {
                        const UINT childOffset =
                            ida->aoffset[index + 1];
                        if (childOffset >= byteCount) continue;
                        const auto* child =
                            reinterpret_cast<PCUIDLIST_RELATIVE>(
                                bytes + childOffset);
                        PIDLIST_ABSOLUTE absolute =
                            ILCombine(parent, child);
                        if (!absolute) continue;
                        IShellItem* item = nullptr;
                        PWSTR filesystemPath = nullptr;
                        if (SUCCEEDED(
                                SHCreateItemFromIDList(
                                    absolute,
                                    IID_PPV_ARGS(&item))) &&
                            item &&
                            SUCCEEDED(item->GetDisplayName(
                                SIGDN_FILESYSPATH,
                                &filesystemPath)) &&
                            filesystemPath) {
                            paths.emplace_back(filesystemPath);
                        }
                        if (filesystemPath) {
                            CoTaskMemFree(filesystemPath);
                        }
                        if (item) item->Release();
                        CoTaskMemFree(absolute);
                    }
                }
            }
        }
        if (ida) GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return paths;
    }

    DWORD ResolveEffect(DWORD allowed, POINT screenPoint) const {
        if (!owner_ || owner_->displayTopologyDirty) return DROPEFFECT_NONE;
        const auto hit = owner_->fenceManager->HitTest(screenPoint);
        if (!hit.hit || hit.area != FenceHitArea::Content) {
            return DROPEFFECT_NONE;
        }
        const Fence* fence =
            owner_->fenceManager->GetFenceById(hit.fenceId);
        if (!fence || fence->collapsed) return DROPEFFECT_NONE;
        if (fence->kind == FenceKind::Clock) return DROPEFFECT_NONE;

        const DWORD preferred =
            fence->isMappedFolder ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        if (allowed & preferred) return preferred;
        if (!fence->isMappedFolder && (allowed & DROPEFFECT_LINK)) {
            return DROPEFFECT_LINK;
        }
        if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
        if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
        if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
        return DROPEFFECT_NONE;
    }

    ~DropTarget() = default;
    LONG references_ = 1;
    DesktopOverlayWindow* owner_ = nullptr;
    bool acceptsFiles_ = false;
};

static bool TryGetOverlayPointer(HWND, POINT& point) {
    return GetPhysicalCursorPos(&point) == TRUE;
}

static const wchar_t POPUP_MENU_HOST_CLASS[] =
    L"DeskTodoNext_PopupMenuHost";

static LRESULT CALLBACK PopupMenuHostWndProc(
    HWND window, UINT message, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(window, message, wp, lp);
}

static HWND CreatePopupMenuHost(HWND owner) {
    static ATOM registered = 0;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PopupMenuHostWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = POPUP_MENU_HOST_CLASS;
        registered = RegisterClassExW(&wc);
        if (!registered && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }
    }

    // This window is intentionally a 1x1 tool window outside the visible
    // desktop. It is used only as the foreground owner required by the
    // Win32 popup-menu loop; the full-screen desktop overlay never becomes
    // the active/foreground window.
    return CreateWindowExW(
        WS_EX_TOOLWINDOW,
        POPUP_MENU_HOST_CLASS, L"",
        WS_POPUP,
        -32000, -32000, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), owner);
}

static UINT TrackForegroundPopupMenu(HWND owner, HMENU menu, UINT flags,
                                     POINT point) {
    if (!owner || !menu) return 0;
    const HWND previousForeground = GetForegroundWindow();
    HWND menuOwner = CreatePopupMenuHost(owner);
    const bool hasMenuHost = menuOwner && IsWindow(menuOwner);
    if (!hasMenuHost) menuOwner = owner;

    if (hasMenuHost) {
        SetWindowPos(
            menuOwner, HWND_TOP, -32000, -32000, 1, 1,
            SWP_SHOWWINDOW);
        SetForegroundWindow(menuOwner);
    } else {
        // Last-resort fallback for a failed host creation. This path is only
        // used during unusual shell/resource failures.
        const HWND foregroundRoot = previousForeground &&
            previousForeground != owner && IsWindow(previousForeground)
            ? GetAncestor(previousForeground, GA_ROOT)
            : owner;
        if (foregroundRoot && IsWindow(foregroundRoot)) {
            menuOwner = foregroundRoot;
            SetForegroundWindow(menuOwner);
        }
    }

    const UINT selected = TrackPopupMenuEx(
        menu, flags | TPM_RETURNCMD, point.x, point.y, menuOwner, nullptr);

    // Required for popup menus: without the benign message, Windows can keep
    // the menu loop attached to the temporary owner after the menu closes.
    PostMessageW(menuOwner, WM_NULL, 0, 0);

    if (hasMenuHost) {
        ShowWindow(menuOwner, SW_HIDE);
    }
    if (previousForeground && previousForeground != menuOwner &&
        IsWindow(previousForeground)) {
        const DWORD menuThread = GetWindowThreadProcessId(menuOwner, nullptr);
        const DWORD restoreThread = GetWindowThreadProcessId(previousForeground, nullptr);
        const bool restoreAttached = restoreThread && restoreThread != menuThread &&
            AttachThreadInput(menuThread, restoreThread, TRUE) == TRUE;
        SetForegroundWindow(previousForeground);
        if (restoreAttached) AttachThreadInput(menuThread, restoreThread, FALSE);
    }
    if (hasMenuHost) {
        DestroyWindow(menuOwner);
    }
    return selected;
}

DesktopOverlayWindow::DesktopOverlayWindow(FenceManager* mgr)
    : fenceManager(mgr) {}

DesktopOverlayWindow::~DesktopOverlayWindow() { Destroy(); }

bool DesktopOverlayWindow::Create(HINSTANCE hInst) {
    const auto initialTopology = MonitorService::CaptureTopology();
    RECT virtualScreen = initialTopology.virtualScreen;
    displayTopologySignature = initialTopology.signature;
    displayTopologySnapshot = initialTopology;
    topologyGeneration = 1;
    topologyReady = false;
    int w = virtualScreen.right - virtualScreen.left;
    int h = virtualScreen.bottom - virtualScreen.top;

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS;
        wc.style = CS_DBLCLKS;
        if (!RegisterClassExW(&wc)) return false;
        classRegistered = true;
    }

    if (!FenceRenderer::EnsureFactories()) return false;

    hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WINDOW_CLASS, L"DeskTodo.Next", WS_POPUP,
        virtualScreen.left, virtualScreen.top, w, h,
        nullptr, nullptr, hInst, this);
    if (!hwnd) { FenceRenderer::ReleaseFactories(); return false; }

    AttachToDesktopHost(virtualScreen);
    width = w;
    height = h;

    if (!EnsureBackbuffer(w, h)) { Destroy(); return false; }

    tooltipService = std::make_unique<TooltipService>();
    tooltipService->Create(hwnd);

    fileMoveService = std::make_unique<FileMoveService>();
    desktopIconLayoutService =
        std::make_unique<DesktopIconLayoutService>();
    desktopIconLayoutService->Load();
    pomodoroWidget = std::make_unique<PomodoroWidgetWindow>(hInst);
    todoWidget = std::make_unique<TodoWidgetWindow>(hInst);
    lanTransferWidget = std::make_unique<LanTransferWindow>(hInst);
    blackHoleBackground = std::make_unique<BlackHoleBackground>();
    screenshotTool = std::make_unique<ScreenshotTool>(hInst);

    renderer.SetIconCache(fenceManager->GetIconCache());

    DragAcceptFiles(hwnd, TRUE);
    dropTarget = new (std::nothrow) DropTarget(this);
    if (dropTarget) {
        dropTargetRegistered =
            SUCCEEDED(RegisterDragDrop(hwnd, dropTarget));
        if (!dropTargetRegistered) {
            dropTarget->Release();
            dropTarget = nullptr;
        }
    }

    Render();
    topologyReady = backbufferHasCompleteFrame &&
        !forceFullRedraw &&
        completeFrameGeneration == backbufferGeneration;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    DesktopShellService::ReconcileSuppressedDesktopPaths();
    EnsureDesktopViewEventHook();

    RegisterHotKey(hwnd, 1001, MOD_CONTROL | MOD_SHIFT, 'Q');
    RegisterHotKey(hwnd, 1002, MOD_CONTROL | MOD_SHIFT, VK_F5);
    RegisterHotKey(hwnd, 1003, MOD_CONTROL | MOD_SHIFT, 'N');
    RegisterHotKey(hwnd, 1004, MOD_CONTROL | MOD_SHIFT, 'L');
    RegisterScreenshotHotkey();
    CreateTrayIcon(hInst);
    pomodoroWidget->RestoreIfVisible();
    todoWidget->RestoreIfVisible();
    if (desktopIconLayoutService->Anchors().empty()) {
        // Establish the first last-known-good snapshot immediately. Waiting
        // for the periodic timer would leave a short first-run window in which
    // a display disconnect has no ordinary-icon anchors to restore.
        desktopIconLayoutService->CaptureStable(
            initialTopology, CollectLogicalMemberPaths());
    } else {
        RestoreDesktopIconsAfterTopology(initialTopology);
    }
    // Explorer can rebuild its desktop view after F5 or option changes. Batch
    // reconciliation is cheap enough to run promptly, but is suspended during
    // direct manipulation so Shell COM never stalls a fence drag.
    SetTimer(hwnd, LOGICAL_ICON_SYNC_TIMER_ID, 750, nullptr);
    SetTimer(hwnd, CLOCK_TIMER_ID, 1000, nullptr);
    const std::wstring showEventName = AppCommand::ShowEventName();
    showRunningInstanceEvent = OpenEventW(
        SYNCHRONIZE, FALSE, showEventName.c_str());
    if (showRunningInstanceEvent) {
        SetTimer(hwnd, SHOW_INSTANCE_TIMER_ID, 100, nullptr);
    }

    return true;
}

void DesktopOverlayWindow::AttachToDesktopHost(
    const RECT& virtualScreen) {
    // Keep the overlay as an independent layered top-level window. Making it
    // a child of Progman/SHELLDLL_DefView is tempting, but on current
    // Explorer builds a full-size layered child can cover the native ListView
    // (desktop files disappear) or be composited behind the wallpaper (the
    // fences disappear). Instead, keep the overlay immediately above the
    // native desktop host in Z-order. This leaves ordinary application
    // windows above the overlay while keeping the fences visible on desktop.
    desktopHostWindow = DesktopShellService::GetNativeDesktopHostWindow();
    PositionForVirtualScreen(virtualScreen);
}

bool DesktopOverlayWindow::PlaceInDesktopLayer() {
    if (!hwnd || !IsWindow(hwnd)) return false;

    if (!desktopHostWindow || !IsWindow(desktopHostWindow) ||
        desktopHostWindow == hwnd) {
        desktopHostWindow = DesktopShellService::GetNativeDesktopHostWindow();
    }

    if (!desktopHostWindow || !IsWindow(desktopHostWindow) ||
        desktopHostWindow == hwnd) {
        // Explorer may not have exposed its desktop host during startup.
        // Keep the current position until the shell becomes available rather
        // than promoting the overlay above all normal application windows.
        return false;
    }

    // Put the overlay immediately above Explorer's desktop host in the
    // Z-order, leaving ordinary application windows above the overlay.
    return SetWindowPos(
        hwnd, desktopHostWindow, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
        SWP_NOOWNERZORDER) != FALSE;
}

bool DesktopOverlayWindow::PositionForVirtualScreen(
    const RECT& virtualScreen) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    POINT origin{ virtualScreen.left, virtualScreen.top };
    const int targetWidth =
        virtualScreen.right - virtualScreen.left;
    const int targetHeight =
        virtualScreen.bottom - virtualScreen.top;

    // Display reconnects generate a burst of topology messages.  Avoid
    // repositioning an already correctly-sized overlay: SetWindowPos can
    // force a layered top-level window through a non-client recalculation,
    // which is visible as a one-frame flash while a fence is being moved.
    RECT current{};
    if (GetWindowRect(hwnd, &current) &&
        current.left == origin.x && current.top == origin.y &&
        current.right - current.left == targetWidth &&
        current.bottom - current.top == targetHeight) {
        PlaceInDesktopLayer();
        return true;
    }

    // A changed virtual-screen origin is just as significant as a changed
    // size: all screen-space fence coordinates map to new local DIB pixels.
    forceFullRedraw = true;
    backbufferHasCompleteFrame = false;
    completeFrameGeneration = 0;
    previousInteractiveDirtyValid = false;

    const bool positioned = SetWindowPos(
        hwnd,
        nullptr,
        origin.x, origin.y, targetWidth, targetHeight,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER |
        SWP_SHOWWINDOW) != FALSE;
    PlaceInDesktopLayer();
    return positioned;
}

void DesktopOverlayWindow::Destroy() {
    CancelTooltipTimer();
    KillTimer(hwnd, TOOLTIP_GUARD_TIMER_ID);
    CancelItemDrag();
    KillTimer(hwnd, CARET_TIMER_ID);
    KillTimer(hwnd, ANIM_TIMER_ID);
    KillTimer(hwnd, LOGICAL_ICON_SYNC_TIMER_ID);
    KillTimer(hwnd, CLOCK_TIMER_ID);
    KillTimer(hwnd, CAPTURE_START_TIMER_ID);
    KillTimer(hwnd, DISPLAY_TOPOLOGY_TIMER_ID);
    KillTimer(hwnd, INTERACTIVE_REDRAW_TIMER_ID);
    KillTimer(hwnd, DESKTOP_RECONCILE_TIMER_ID);
    KillTimer(hwnd, SHOW_INSTANCE_TIMER_ID);
    if (showRunningInstanceEvent) {
        CloseHandle(showRunningInstanceEvent);
        showRunningInstanceEvent = nullptr;
    }
    RemoveDesktopViewEventHook();
    interactiveRedrawTimerArmed = false;
    interactiveRedrawPending = false;
    if (dropTargetRegistered && hwnd) {
        RevokeDragDrop(hwnd);
        dropTargetRegistered = false;
    }
    if (dropTarget) {
        dropTarget->Release();
        dropTarget = nullptr;
    }
    DragAcceptFiles(hwnd, FALSE);
    DestroyTrayIcon();
    if (hwnd) {
        UnregisterHotKey(hwnd, 1001);
        UnregisterHotKey(hwnd, 1002);
        UnregisterHotKey(hwnd, 1003);
        UnregisterHotKey(hwnd, 1004);
        UnregisterScreenshotHotkey();
    }
    if (tooltipService) tooltipService->Destroy();
    if (pomodoroWidget) {
        pomodoroWidget->Destroy();
        pomodoroWidget.reset();
    }
    if (todoWidget) {
        todoWidget->Destroy();
        todoWidget.reset();
    }
    if (lanTransferWidget) {
        lanTransferWidget->Destroy();
        lanTransferWidget.reset();
    }
    if (blackHoleBackground) {
        blackHoleBackground->Stop();
        blackHoleBackground.reset();
    }
    if (screenshotTool) {
        screenshotTool->Destroy();
        screenshotTool.reset();
    }
    desktopIconLayoutService.reset();
    ResetRenderResources();
    if (memBitmap) { DeleteObject(memBitmap); memBitmap = nullptr; }
    if (memDC) { DeleteDC(memDC); memDC = nullptr; }
    if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
    FenceRenderer::ReleaseFactories();
}

void DesktopOverlayWindow::EnsureDesktopViewEventHook() {
    const HWND listView =
        DesktopShellService::GetNativeDesktopIconWindow();
    if (desktopViewEventHook &&
        listView == watchedDesktopIconWindow &&
        listView && IsWindow(listView)) {
        return;
    }

    RemoveDesktopViewEventHook();
    if (!listView || !IsWindow(listView) || !hwnd) return;

    DWORD processId = 0;
    const DWORD threadId =
        GetWindowThreadProcessId(listView, &processId);
    if (!threadId || !processId) return;

    desktopViewEventOverlay = hwnd;
    desktopViewEventListView = listView;
    desktopViewEventHook = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER,
        nullptr, DesktopViewWinEventProc,
        processId, threadId,
        WINEVENT_OUTOFCONTEXT);
    if (desktopViewEventHook) {
        watchedDesktopIconWindow = listView;
    } else {
        desktopViewEventOverlay = nullptr;
        desktopViewEventListView = nullptr;
    }
}

void DesktopOverlayWindow::RemoveDesktopViewEventHook() {
    if (desktopViewEventOverlay == hwnd) {
        desktopViewEventOverlay = nullptr;
        desktopViewEventListView = nullptr;
    }
    if (desktopViewEventHook) {
        UnhookWinEvent(desktopViewEventHook);
        desktopViewEventHook = nullptr;
    }
    watchedDesktopIconWindow = nullptr;
}

void DesktopOverlayWindow::ScheduleDesktopReconcile() {
    if (!hwnd) return;
    KillTimer(hwnd, DESKTOP_RECONCILE_TIMER_ID);
    SetTimer(hwnd, DESKTOP_RECONCILE_TIMER_ID, 25, nullptr);
}

void DesktopOverlayWindow::RequestRedraw() {
    if (!hwnd) return;
    const bool directManipulation =
        interactionState == FenceInteractionState::MovingFence ||
        interactionState == FenceInteractionState::ResizingFence ||
        interactionState == FenceInteractionState::DraggingItem;
    if (directManipulation && lastRenderTick != 0) {
        const ULONGLONG elapsed = GetTickCount64() - lastRenderTick;
        if (elapsed < INTERACTIVE_FRAME_MS) {
            interactiveRedrawPending = true;
            if (!interactiveRedrawTimerArmed) {
                const UINT wait = static_cast<UINT>(
                    (std::max)(
                        1ULL,
                        static_cast<ULONGLONG>(
                            INTERACTIVE_FRAME_MS) - elapsed));
                SetTimer(
                    hwnd, INTERACTIVE_REDRAW_TIMER_ID, wait, nullptr);
                interactiveRedrawTimerArmed = true;
            }
            return;
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void DesktopOverlayWindow::ExecuteCommand(const std::wstring& command) {
    if (command == AppCommand::Refresh) {
        fenceManager->RefreshAllFenceItems();
        RequestRedraw();
        return;
    }
    if (command == AppCommand::Show) {
        // Keep the desktop interactive after launch. A modal Settings window
        // disables the overlay and makes Explorer-to-fence drag/drop appear
        // broken until that dialog is closed.
        fenceManager->SetAllVisible(true);
        DesktopShellService::ReconcileSuppressedDesktopPaths();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        PlaceInDesktopLayer();
        RequestRedraw();
        nid.uFlags = NIF_INFO;
        wcscpy_s(
            nid.szInfoTitle,
            L"DeskTodo");
        wcscpy_s(
            nid.szInfo,
            L"DeskTodo \u5df2\u8fd0\u884c\uff0c"
            L"\u683c\u5b50\u5df2\u6062\u590d\u3002");
        nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        return;
    }
    POINT pt{};
    GetPhysicalCursorPos(&pt);
    if (command == AppCommand::NewFence) {
        Fence* fence = fenceManager->CreateFenceAt(pt, L"新建格子");
        if (fence) activeFenceId = fence->id;
        RequestRedraw();
        return;
    }
    if (command == AppCommand::NewGameFence) {
        Fence* fence = fenceManager->CreateGameFenceAt(pt);
        if (fence) activeFenceId = fence->id;
        RequestRedraw();
        return;
    }
    if (command == AppCommand::NewClockFence) {
        Fence* fence = fenceManager->CreateClockFenceAt(pt);
        if (fence) activeFenceId = fence->id;
        RequestRedraw();
        return;
    }
    if (command == AppCommand::MapFolder || command.rfind(L"map-folder=", 0) == 0) {
        const std::wstring folder = command.rfind(L"map-folder=", 0) == 0
            ? command.substr(std::wstring(L"map-folder=").size())
            : SelectFolder();
        if (!folder.empty()) {
            const std::wstring title = std::filesystem::path(folder).filename().wstring();
            Fence* fence = fenceManager->CreateFenceAt(pt, title, folder, true);
            if (fence) activeFenceId = fence->id;
            RequestRedraw();
        }
        return;
    }
    if (command == AppCommand::Arrange) {
        fenceManager->ArrangeAllItems();
        RequestRedraw();
        return;
    }
    if (command == AppCommand::AutoOrganize) {
        int moved = fenceManager->AutoOrganizeDesktop();
        std::wstring msg = L"\u5df2\u6574\u7406 " + std::to_wstring(moved) + L" \u4e2a\u6587\u4ef6\u3002";
        MessageBoxW(hwnd, msg.c_str(), L"DeskTodo \u4e00\u952e\u6574\u7406",
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        RequestRedraw();
        return;
    }
    if (command == AppCommand::ToggleVisibility) {
        fenceManager->ToggleAllVisibility();
        RequestRedraw();
        return;
    }
    if (command == AppCommand::ToggleLock) {
        fenceManager->ToggleAllLocks();
        RequestRedraw();
        return;
    }
    if (command == AppCommand::Settings) {
        ShowSettingsDialog();
        return;
    }
    if (command == AppCommand::Screenshot) {
        StartScreenshotCaptureSoon();
        return;
    }
    if (command == AppCommand::LanTransfer) {
        if (lanTransferWidget) lanTransferWidget->ShowOrCreate();
        return;
    }
    if (command == AppCommand::ToggleBlackHoleBg) {
        ToggleBlackHoleBackground();
        return;
    }
    if (command == AppCommand::BackupLayout) {
        BackupLayout();
        return;
    }
    if (command.rfind(L"backup-layout=", 0) == 0) {
        const std::wstring path = command.substr(std::wstring(L"backup-layout=").size());
        if (!path.empty()) fenceManager->BackupLayout(path);
        return;
    }
    if (command == AppCommand::RestoreLayout) {
        RestoreLayout();
        return;
    }
    if (command.rfind(L"restore-layout=", 0) == 0) {
        const std::wstring path = command.substr(std::wstring(L"restore-layout=").size());
        if (!path.empty()) fenceManager->RestoreLayout(path);
        RequestRedraw();
        return;
    }
    if (command == AppCommand::OpenStorage) {
        FileService::ShellOpenFile(GetFenceRootPath());
        return;
    }
    if (command == AppCommand::Version) {
        MessageBoxW(nullptr, L"DeskTodo.Next 1.7\n\n桌面刷新持久化、多屏稳定布局、游戏容器与增强 Todo 已启用。",
                    L"DeskTodo 版本信息",
                    MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
        return;
    }
    if (command == AppCommand::Exit) {
        RequestExit();
    }
}

std::wstring DesktopOverlayWindow::SelectFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return L"";
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"选择要映射到格子的文件夹");
    std::wstring result;
    if (SUCCEEDED(dialog->Show(hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

// Title-bar button hit test.
FenceTitleControl DesktopOverlayWindow::HitTestTitleControl(const Fence& fence, POINT pt) const {
    if (pt.y < fence.rect.top || pt.y >= fence.rect.top + FENCE_TITLE_BAR_HEIGHT) return FenceTitleControl::None;
    const int x = pt.x - fence.rect.left;
    const int width = fence.rect.right - fence.rect.left;
    if (x >= 4 && x < 30) return FenceTitleControl::Collapse;
    if (fence.kind == FenceKind::Game || fence.kind == FenceKind::Clock) {
        if (x >= 32 && x < 64) return FenceTitleControl::Lock;
        if (x >= width - 36 && x < width) return FenceTitleControl::Menu;
        return FenceTitleControl::None;
    }
    if (width < 240) {
        if (x >= width - 70 && x < width - 36) return FenceTitleControl::Search;
        if (x >= width - 36 && x < width) return FenceTitleControl::Menu;
        return FenceTitleControl::None;
    }
    if (x >= 32 && x < 64) return FenceTitleControl::Lock;
    if (x >= width - 104 && x < width - 70) return FenceTitleControl::View;
    if (x >= width - 70 && x < width - 36) return FenceTitleControl::Search;
    if (x >= width - 36 && x < width) return FenceTitleControl::Menu;
    return FenceTitleControl::None;
}

// 拉伸边缘命中测试
ResizeEdge DesktopOverlayWindow::HitTestResizeEdge(const Fence& fence, POINT pt) const {
    // Game containers are fixed-size launch cards. They keep the normal
    // title-bar movement path, but never expose resize handles.
    if (fence.locked || fence.kind == FenceKind::Game) return ResizeEdge::None;
    if (!PtInRect(&fence.rect, pt)) return ResizeEdge::None;

    const RECT& r = fence.rect;
    const int edgeHot = GridLayoutService::RESIZE_EDGE_HOT;
    const int cornerHot = GridLayoutService::RESIZE_CORNER_HOT;

    ResizeEdge edge = ResizeEdge::None;

    // Prefer larger corner hit zones.
    bool nearLeft = (pt.x >= r.left && pt.x < r.left + cornerHot);
    bool nearRight = (pt.x < r.right && pt.x >= r.right - cornerHot);
    bool nearTop = (pt.y >= r.top && pt.y < r.top + cornerHot);
    bool nearBottom = (pt.y < r.bottom && pt.y >= r.bottom - cornerHot);

    if (nearLeft && nearTop) return ResizeEdge::Left | ResizeEdge::Top;
    if (nearRight && nearTop) return ResizeEdge::Right | ResizeEdge::Top;
    if (nearLeft && nearBottom) return ResizeEdge::Left | ResizeEdge::Bottom;
    if (nearRight && nearBottom) return ResizeEdge::Right | ResizeEdge::Bottom;

    // Edge hit test.
    if (pt.x >= r.left && pt.x < r.left + edgeHot) edge = edge | ResizeEdge::Left;
    if (pt.x < r.right && pt.x >= r.right - edgeHot) edge = edge | ResizeEdge::Right;
    if (pt.y >= r.top && pt.y < r.top + edgeHot) edge = edge | ResizeEdge::Top;
    if (pt.y < r.bottom && pt.y >= r.bottom - edgeHot) edge = edge | ResizeEdge::Bottom;

    return edge;
}

void DesktopOverlayWindow::ShowFenceMenu(const std::wstring& fenceId, POINT pt) {
    Fence* fence = fenceManager->GetFenceById(fenceId);
    if (!fence) return;
    const bool mappedFolder = fence->isMappedFolder;
    const bool gameFence = fence->kind == FenceKind::Game;
    const std::wstring folderPath = fence->folderPath;
    const std::vector<std::wstring> logicalMembers = fence->memberPaths;
    const bool hasLogicalMembers = !mappedFolder && !logicalMembers.empty();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (mappedFolder ? MF_GRAYED : 0), 2101, L"\u91cd\u547d\u540d\u683c\u5b50");
    AppendMenuW(menu, MF_STRING, 2102, fence->collapsed ? L"展开格子" : L"折叠格子");
    AppendMenuW(menu, MF_STRING, 2103, fence->locked ? L"解锁格子" : L"锁定格子");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!gameFence) {
        AppendMenuW(menu, MF_STRING, 2104, fence->viewMode == FenceViewMode::List ? L"\u5207\u6362\u4e3a\u56fe\u6807\u663e\u793a" : L"\u5207\u6362\u4e3a\u5217\u8868\u663e\u793a");
        AppendMenuW(menu, MF_STRING, 2105, L"自动排列（整齐网格）");
        AppendMenuW(menu, MF_STRING, 2106, fence->sortByExtension ? L"\u6309\u540d\u79f0\u6392\u5e8f" : L"\u6309\u7c7b\u578b\u6392\u5e8f");
    }
    AppendMenuW(menu, MF_STRING, 2107, L"刷新格子");
    AppendMenuW(menu, MF_STRING, 2108, gameFence
        ? L"\u542f\u52a8\u6e38\u620f"
        : (hasLogicalMembers
            ? L"\u6253\u5f00\u6210\u5458\u539f\u8def\u5f84"
            : L"\u6253\u5f00\u683c\u5b50\u6587\u4ef6\u5939"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2109, L"新建格子");
    AppendMenuW(menu, MF_STRING, 2110, L"\u65b0\u5efa\u6587\u4ef6\u5939\u6620\u5c04");
    AppendMenuW(menu, MF_STRING, 2114, L"\u65b0\u5efa\u6e38\u620f\u5bb9\u5668");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2112, L"备份桌面布局");
    AppendMenuW(menu, MF_STRING, 2113, L"还原桌面布局");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 2111, mappedFolder ? L"\u89e3\u9664\u6587\u4ef6\u5939\u6620\u5c04" : L"\u89e3\u6563\u683c\u5b50");
    const UINT selected = TrackForegroundPopupMenu(
        hwnd, menu, TPM_RIGHTBUTTON, pt);
    DestroyMenu(menu);
    PlaceInDesktopLayer();
    // A display event can arrive while TrackPopupMenu runs its nested message
    // loop. Do not execute a stale placement-changing command after that event.
    if (displayTopologyDirty) return;
    switch (selected) {
    case 2101: if (!mappedFolder) EnterTextInputMode(TextInputMode::Rename, fenceId); break;
    case 2102: fenceManager->ToggleFenceCollapsed(fenceId); break;
    case 2103:
        fenceManager->ToggleFenceLocked(fenceId);
        fenceManager->TriggerLockAnimation(fenceId);
        break;
    case 2104:
        fenceManager->ToggleViewMode(fenceId);
        fenceManager->SyncLogicalDesktopPositions(fenceId);
        break;
    case 2105:
        fenceManager->ArrangeFenceItems(fenceId);
        fenceManager->SaveConfig();
        fenceManager->SyncLogicalDesktopPositions(fenceId);
        break;
    case 2106:
        fenceManager->SortFenceItems(fenceId);
        fenceManager->SyncLogicalDesktopPositions(fenceId);
        break;
    case 2107:
        fenceManager->RefreshFenceItems(fenceId);
        fenceManager->SaveConfig();
        fenceManager->SyncLogicalDesktopPositions(fenceId);
        break;
    case 2108:
        if (gameFence && !logicalMembers.empty()) {
            FileService::ShellOpenFile(logicalMembers.front());
        } else if (hasLogicalMembers) {
            const std::wstring parent =
                std::filesystem::path(logicalMembers.front()).parent_path().wstring();
            FileService::ShellOpenFile(parent.empty() ? logicalMembers.front() : parent);
        } else {
            FileService::ShellOpenFile(folderPath);
        }
        break;
    case 2109: ExecuteCommand(AppCommand::NewFence); break;
    case 2110: ExecuteCommand(AppCommand::MapFolder); break;
    case 2111: DissolveFence(fenceId); break;
    case 2112: BackupLayout(); break;
    case 2113: RestoreLayout(); break;
    case 2114: ExecuteCommand(AppCommand::NewGameFence); break;
    default: break;
    }
    RequestRedraw();
}

void DesktopOverlayWindow::BackupLayout() {
    const std::wstring path = LayoutBackupDialog::SelectSavePath(hwnd);
    if (path.empty()) return;
    const bool ok = fenceManager && fenceManager->BackupLayout(path);
    MessageBoxW(hwnd, ok ? L"\u5f53\u524d\u684c\u9762\u5e03\u5c40\u5df2\u5907\u4efd\u3002" : L"\u5e03\u5c40\u5907\u4efd\u5931\u8d25\u3002",
                L"DeskTodo", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR) | MB_TOPMOST);
}

void DesktopOverlayWindow::RestoreLayout() {
    const std::wstring path = LayoutBackupDialog::SelectOpenPath(hwnd);
    if (path.empty()) return;
    const int answer = MessageBoxW(hwnd, L"确定使用最近一次备份覆盖当前布局吗？",
        L"还原桌面布局", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
    if (answer != IDYES) return;
    const bool ok = fenceManager && fenceManager->RestoreLayout(path);
    MessageBoxW(hwnd, ok ? L"\u684c\u9762\u5e03\u5c40\u5df2\u8fd8\u539f\u3002" : L"\u6ca1\u6709\u53ef\u7528\u5907\u4efd\uff0c\u6216\u5907\u4efd\u6587\u4ef6\u65e0\u6548\u3002",
                L"DeskTodo", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR) | MB_TOPMOST);
    RequestRedraw();
}

bool DesktopOverlayWindow::RequestExit() {
    if (exitInProgress) return true;
    exitInProgress = true;
    if (!ReleaseAllManagedFilesToDesktop()) {
        exitInProgress = false;
        MessageBoxW(hwnd,
                    L"\u9000\u51fa\u5df2\u53d6\u6d88\uff1a\u6709\u683c\u5b50\u6587\u4ef6\u672a\u80fd\u79fb\u56de\u684c\u9762\u3002",
                    L"DeskTodo",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
        RequestRedraw();
        return false;
    }
    DestroyWindow(hwnd);
    hwnd = nullptr;
    return true;
}

void DesktopOverlayWindow::StartScreenshotCaptureSoon() {
    if (!hwnd || !screenshotTool) return;
    // Starting on the next composition frame prevents the destroyed settings
    // button from being copied into the screen bitmap. An arrow is forced now
    // so the user never sees the previous button's busy cursor while waiting.
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    KillTimer(hwnd, CAPTURE_START_TIMER_ID);
    SetTimer(hwnd, CAPTURE_START_TIMER_ID, 80, nullptr);
}

bool DesktopOverlayWindow::RegisterScreenshotHotkey() {
    if (!hwnd) return false;
    const ScreenshotHotkeyConfig config = LoadScreenshotHotkeyConfig();
    screenshotHotkeyModifiers = ScreenshotHotkeyModifiers(config);
    screenshotHotkeyVirtualKey = config.virtualKey;
    screenshotHotkeyRegistered = screenshotHotkeyVirtualKey != 0 &&
        RegisterHotKey(hwnd, 1005, screenshotHotkeyModifiers, screenshotHotkeyVirtualKey) == TRUE;
    return screenshotHotkeyRegistered;
}

void DesktopOverlayWindow::UnregisterScreenshotHotkey() {
    if (hwnd && screenshotHotkeyRegistered) UnregisterHotKey(hwnd, 1005);
    screenshotHotkeyRegistered = false;
}

bool DesktopOverlayWindow::ReloadScreenshotHotkey() {
    UnregisterScreenshotHotkey();
    return RegisterScreenshotHotkey();
}

bool DesktopOverlayWindow::ReleaseAllManagedFilesToDesktop() {
    if (!fenceManager) return true;
    if (displayTopologyDirty) return false;
    const std::wstring desktop = GetUserDesktopPath();
    if (desktop.empty()) return false;

    // Refresh every fence before taking the operation snapshot. Resolve all
    // saved desktop origins against one stable topology before moving even the
    // first file, so an offline secondary monitor cannot cause a partial exit.
    const auto fenceIdsSnapshot = fenceManager->GetAllFences();
    for (const auto& fence : fenceIdsSnapshot) {
        if (!fence.isMappedFolder) {
            fenceManager->RefreshFenceItems(fence.id);
        }
    }
    const auto fencesSnapshot = fenceManager->GetAllFences();
    const auto topology = MonitorService::CaptureTopology();
    const auto fenceMonitorIsOnline = [&](const Fence& fence) {
        return fence.monitorPlacement.valid
            ? MonitorService::FindMonitorById(
                  topology, fence.monitorPlacement.monitorId) != nullptr
            : fence.monitorOnline;
    };
    for (const auto& fence : fencesSnapshot) {
        if (fence.isMappedFolder) continue;
        for (const auto& item : fence.items) {
            std::error_code existsError;
            if (item.fullPath.empty() ||
                !std::filesystem::exists(item.fullPath, existsError) ||
                existsError) {
                continue;
            }
            POINT targetPoint{};
            const auto originResolution =
                fenceManager->ResolveItemDesktopOrigin(
                    fence.id, item.fullPath, topology, targetPoint);
            const bool logicalItem =
                fenceManager->IsLogicalItem(fence.id, item.fullPath);
            if ((!logicalItem &&
                 originResolution == DesktopOriginResolution::Offline) ||
                (!logicalItem &&
                 originResolution == DesktopOriginResolution::Missing &&
                 !fenceMonitorIsOnline(fence))) {
                return false;
            }
        }
    }

    struct MovedItemRollback {
        std::wstring movedPath;
        std::wstring originalDirectory;
    };
    std::vector<MovedItemRollback> rollbackItems;
    auto preserveFences = [&]() -> bool {
        bool rollbackSucceeded = true;
        for (auto item = rollbackItems.rbegin(); item != rollbackItems.rend(); ++item) {
            if (item->movedPath.empty() || item->originalDirectory.empty()) continue;
            std::error_code existsError;
            if (!std::filesystem::exists(item->movedPath, existsError) ||
                existsError) {
                const std::wstring originalPath =
                    (std::filesystem::path(item->originalDirectory) /
                     std::filesystem::path(item->movedPath).filename()).wstring();
                std::error_code originalExistsError;
                if (!std::filesystem::exists(
                        originalPath, originalExistsError) ||
                    originalExistsError) {
                    rollbackSucceeded = false;
                }
                continue;
            }
            std::wstring rolledBackPath;
            if (!MovePathWithConflictPrompt(
                    item->movedPath, item->originalDirectory,
                    &rolledBackPath, false)) {
                rollbackSucceeded = false;
            }
        }
        for (const auto& fence : fenceManager->GetAllFences()) {
            if (fence.isMappedFolder) continue;
            fenceManager->RefreshFenceItems(fence.id);
            fenceManager->SyncLogicalDesktopPositions(fence.id);
        }
        DesktopShellService::RefreshDesktop();
        return rollbackSucceeded;
    };
    auto abortRelease = [&]() -> bool {
        if (!preserveFences()) {
            MessageBoxW(
                hwnd,
                L"退出已取消，但部分文件未能退回格子存储目录。"
                L"格子配置已保留，请检查桌面与格子中的文件后重试。",
                L"DeskTodo",
                MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }
        return false;
    };

    bool movedAny = false;
    for (const auto& fenceSnapshot : fencesSnapshot) {
        if (fenceSnapshot.isMappedFolder) continue;

        Fence* fence = fenceManager->GetFenceById(fenceSnapshot.id);
        if (!fence) continue;
        if (!fenceManager->RestoreLogicalDesktopItems(fence->id, true)) {
            return abortRelease();
        }

        const auto itemsSnapshot = fence->items;
        for (const auto& item : itemsSnapshot) {
            std::error_code existsError;
            if (item.fullPath.empty() ||
                !std::filesystem::exists(item.fullPath, existsError) ||
                existsError) {
                continue;
            }
            if (fenceManager->IsLogicalItem(fence->id, item.fullPath)) {
                movedAny = true;
                continue;
            }

            POINT targetPoint{};
            const auto originResolution = fenceManager->ResolveItemDesktopOrigin(
                fence->id, item.fullPath, topology, targetPoint);
            if (originResolution == DesktopOriginResolution::Offline ||
                (originResolution == DesktopOriginResolution::Missing &&
                 !fenceMonitorIsOnline(*fence))) {
                return abortRelease();
            }
            if (originResolution == DesktopOriginResolution::Missing) {
                targetPoint = DesktopShellService::SnapPointToDesktopGrid(
                    { item.iconRect.left, item.iconRect.top });
            }

            std::wstring movedPath;
            if (displayTopologyDirty) return abortRelease();
            if (!MovePathWithConflictPrompt(item.fullPath, desktop, &movedPath)) {
                return abortRelease();
            }
            if (movedPath.empty()) {
                return abortRelease();
            }

            rollbackItems.push_back({
                movedPath,
                std::filesystem::path(item.fullPath).parent_path().wstring()
            });
            if (displayTopologyDirty) return abortRelease();
            const bool positioned =
                originResolution == DesktopOriginResolution::Online
                ? DesktopShellService::RestorePathToDesktopPosition(
                    movedPath, targetPoint)
                : DesktopShellService::PositionPathOnDesktop(
                    movedPath, targetPoint);
            if (!positioned) {
                return abortRelease();
            }
            if (displayTopologyDirty) return abortRelease();
            movedAny = true;
        }
    }

    if (displayTopologyDirty) return abortRelease();
    if (movedAny) {
        DesktopShellService::RefreshDesktop();
    }
    return true;
}

void DesktopOverlayWindow::DissolveFence(const std::wstring& fenceId) {
    Fence* source = fenceManager ? fenceManager->GetFenceById(fenceId) : nullptr;
    if (!source) return;
    Fence snapshot = *source;
    const wchar_t* question = snapshot.isMappedFolder
        ? L"\u786e\u5b9a\u89e3\u9664\u8fd9\u4e2a\u6587\u4ef6\u5939\u6620\u5c04\u5417\uff1f\u539f\u6587\u4ef6\u5939\u53ca\u5185\u5bb9\u4e0d\u4f1a\u88ab\u79fb\u52a8\u3002"
        : L"\u786e\u5b9a\u89e3\u6563\u8fd9\u4e2a\u683c\u5b50\u5417\uff1f\u684c\u9762\u6765\u6e90\u9879\u76ee\u5c06\u6062\u590d\u663e\u793a\uff0c\u5176\u4ed6\u8def\u5f84\u6587\u4ef6\u4fdd\u6301\u539f\u4f4d\u3002";
    if (MessageBoxW(hwnd, question, L"解散格子",
                    MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES) return;

    if (!snapshot.isMappedFolder) {
        if (displayTopologyDirty) {
            MessageBoxW(hwnd,
                        L"显示器配置正在更新，暂时无法安全解散格子。请稍后再试。",
                        L"DeskTodo",
                        MB_OK | MB_ICONWARNING | MB_TOPMOST);
            return;
        }
        const std::wstring desktop = GetUserDesktopPath();
        if (desktop.empty()) return;

        fenceManager->RefreshFenceItems(fenceId);
        source = fenceManager->GetFenceById(fenceId);
        if (!source) return;
        snapshot = *source;

        // Resolve every member against the same topology up front. In
        // particular, Offline is not Missing: the saved target belongs to a
        // monitor that must return before this fence can be dissolved.
        const auto topology = MonitorService::CaptureTopology();
        const bool snapshotMonitorOnline =
            snapshot.monitorPlacement.valid
            ? MonitorService::FindMonitorById(
                  topology,
                  snapshot.monitorPlacement.monitorId) != nullptr
            : snapshot.monitorOnline;
        for (const auto& item : snapshot.items) {
            std::error_code existsError;
            if (item.fullPath.empty() ||
                !std::filesystem::exists(item.fullPath, existsError) ||
                existsError) {
                continue;
            }
            POINT targetPoint{};
            const auto originResolution =
                fenceManager->ResolveItemDesktopOrigin(
                    fenceId, item.fullPath, topology, targetPoint);
            const bool logicalItem =
                fenceManager->IsLogicalItem(fenceId, item.fullPath);
            if (originResolution == DesktopOriginResolution::Offline ||
                (!logicalItem &&
                 originResolution == DesktopOriginResolution::Missing &&
                 !snapshotMonitorOnline)) {
                MessageBoxW(
                    hwnd,
                    L"该格子中有文件的目标显示器当前离线。为避免桌面图标错位，"
                    L"已保留格子；请重新连接显示器后再试。",
                    L"DeskTodo",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
                return;
            }
        }

        struct MovedItemRollback {
            std::wstring movedPath;
            std::wstring originalDirectory;
        };
        std::vector<MovedItemRollback> rollbackItems;
        auto preserveFence = [&]() -> bool {
            bool rollbackSucceeded = true;
            for (auto item = rollbackItems.rbegin();
                 item != rollbackItems.rend(); ++item) {
                if (item->movedPath.empty() ||
                    item->originalDirectory.empty()) {
                    continue;
                }
                std::error_code existsError;
                if (!std::filesystem::exists(item->movedPath, existsError) ||
                    existsError) {
                    const std::wstring originalPath =
                        (std::filesystem::path(item->originalDirectory) /
                         std::filesystem::path(item->movedPath).filename()).wstring();
                    std::error_code originalExistsError;
                    if (!std::filesystem::exists(
                            originalPath, originalExistsError) ||
                        originalExistsError) {
                        rollbackSucceeded = false;
                    }
                    continue;
                }
                std::wstring rolledBackPath;
                if (!MovePathWithConflictPrompt(
                        item->movedPath, item->originalDirectory,
                        &rolledBackPath, false)) {
                    rollbackSucceeded = false;
                }
            }
            fenceManager->RefreshFenceItems(fenceId);
            fenceManager->SyncLogicalDesktopPositions(fenceId);
            DesktopShellService::RefreshDesktop();
            RequestRedraw();
            return rollbackSucceeded;
        };
        auto warnRollbackFailure = [&]() {
            MessageBoxW(
                hwnd,
                L"格子配置已保留，但部分文件未能退回格子存储目录。"
                L"请检查桌面与格子中的文件后重试。",
                L"DeskTodo",
                MB_OK | MB_ICONWARNING | MB_TOPMOST);
        };

        for (const auto& item : snapshot.items) {
            if (fenceManager->IsLogicalItem(fenceId, item.fullPath)) continue;
            std::error_code existsError;
            if (item.fullPath.empty() ||
                !std::filesystem::exists(item.fullPath, existsError) ||
                existsError) {
                continue;
            }
            POINT targetPoint{};
            const auto originResolution = fenceManager->ResolveItemDesktopOrigin(
                fenceId, item.fullPath, topology, targetPoint);
            if (originResolution == DesktopOriginResolution::Offline ||
                (originResolution == DesktopOriginResolution::Missing &&
                 !snapshotMonitorOnline)) {
                if (!preserveFence()) warnRollbackFailure();
                return;
            }
            if (originResolution == DesktopOriginResolution::Missing) {
                targetPoint = DesktopShellService::SnapPointToDesktopGrid(
                    { item.iconRect.left, item.iconRect.top });
            }

            std::wstring movedPath;
            if (displayTopologyDirty) {
                if (!preserveFence()) warnRollbackFailure();
                return;
            }
            if (!MovePathWithConflictPrompt(item.fullPath, desktop, &movedPath)) {
                if (!preserveFence()) warnRollbackFailure();
                return;
            }
            if (movedPath.empty()) {
                if (!preserveFence()) warnRollbackFailure();
                return;
            }

            rollbackItems.push_back({
                movedPath,
                std::filesystem::path(item.fullPath).parent_path().wstring()
            });
            if (displayTopologyDirty) {
                const bool rolledBack = preserveFence();
                MessageBoxW(
                    hwnd,
                    rolledBack
                        ? L"显示器配置已变化，解散已取消，文件已退回格子。"
                        : L"显示器配置已变化，解散已取消；部分文件未能退回"
                          L"格子存储目录，请检查桌面与格子中的文件。",
                    L"DeskTodo",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
                return;
            }
            const bool positioned =
                originResolution == DesktopOriginResolution::Online
                ? DesktopShellService::RestorePathToDesktopPosition(
                    movedPath, targetPoint)
                : DesktopShellService::PositionPathOnDesktop(
                    movedPath, targetPoint);
            if (!positioned) {
                const bool rolledBack = preserveFence();
                MessageBoxW(
                    hwnd,
                    rolledBack
                        ? L"无法将格子文件安全恢复到桌面位置，"
                          L"文件已退回格子，格子已保留。请稍后重试。"
                        : L"无法安全恢复桌面位置，且部分文件未能退回"
                          L"格子存储目录。格子配置已保留，请检查文件。",
                    L"DeskTodo",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
                return;
            }
            if (displayTopologyDirty) {
                const bool rolledBack = preserveFence();
                MessageBoxW(
                    hwnd,
                    rolledBack
                        ? L"显示器配置已变化，解散已取消，文件已退回格子。"
                        : L"显示器配置已变化，解散已取消；部分文件未能退回"
                          L"格子存储目录，请检查桌面与格子中的文件。",
                    L"DeskTodo",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
                return;
            }
        }

        if (displayTopologyDirty) {
            const bool rolledBack = preserveFence();
            MessageBoxW(
                hwnd,
                rolledBack
                    ? L"显示器配置已变化，解散已取消，文件已退回格子。"
                    : L"显示器配置已变化，解散已取消；部分文件未能退回"
                      L"格子存储目录，请检查桌面与格子中的文件。",
                L"DeskTodo",
                MB_OK | MB_ICONWARNING | MB_TOPMOST);
            return;
        }
        if (!fenceManager->RemoveFence(fenceId)) {
            const bool rolledBack = preserveFence();
            MessageBoxW(
                hwnd,
                rolledBack
                    ? L"无法将格子内容安全恢复到桌面，"
                      L"文件已退回格子，格子已保留。请稍后重试。"
                    : L"无法安全恢复格子内容，且部分文件未能退回"
                      L"格子存储目录。格子配置已保留，请检查文件。",
                L"DeskTodo",
                MB_OK | MB_ICONWARNING | MB_TOPMOST);
            return;
        }
        DesktopShellService::RefreshDesktop();
    } else if (!fenceManager->RemoveFence(fenceId)) {
        return;
    }
    activeFenceId.clear();
    hoverFenceId.clear();
    RequestRedraw();
}

bool DesktopOverlayWindow::EnsureBackbuffer(int w, int h) {
    if (memBitmap && width == w && height == h)
        return true;

    // The D2D DC target is bound to the old DIB/DC. Release all D2D
    // resources before replacing either GDI object.
    ResetRenderResources();
    if (memBitmap) { DeleteObject(memBitmap); memBitmap = nullptr; }
    bitmapBits = nullptr;
    if (memDC) { DeleteDC(memDC); memDC = nullptr; }

    // The old DIB and its Direct2D target no longer describe the same
    // virtual desktop after a resize/reconnect. Any previous dirty rectangle
    // is invalid until a complete frame has been uploaded.
    forceFullRedraw = true;
    backbufferHasCompleteFrame = false;
    completeFrameGeneration = 0;
    previousInteractiveDirtyValid = false;

    HDC screenDC = GetDC(nullptr);
    memDC = CreateCompatibleDC(screenDC);

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    memBitmap = CreateDIBSection(screenDC, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                 &bitmapBits, nullptr, 0);
    if (!memBitmap) {
        ReleaseDC(nullptr, screenDC);
        return false;
    }

    SelectObject(memDC, memBitmap);
    ReleaseDC(nullptr, screenDC);

    width = w;
    height = h;
    ++backbufferGeneration;
    return true;
}

void DesktopOverlayWindow::ResetRenderResources() {
    // Snap brushes are created from FenceRenderer's target and must be
    // released first. Otherwise a reconnect can leave a brush from the old
    // target cached and make EndDraw fail on every frame with snap lines.
    snapRenderer.DestroyResources();
    renderer.DestroyRenderTarget();
}

void DesktopOverlayWindow::ClearBackbufferRect(
    const RECT& requestedRect) {
    if (!bitmapBits || width <= 0 || height <= 0) return;
    RECT rect{
        (std::clamp)(requestedRect.left, 0L,
                     static_cast<LONG>(width)),
        (std::clamp)(requestedRect.top, 0L,
                     static_cast<LONG>(height)),
        (std::clamp)(requestedRect.right, 0L,
                     static_cast<LONG>(width)),
        (std::clamp)(requestedRect.bottom, 0L,
                     static_cast<LONG>(height))
    };
    if (rect.right <= rect.left ||
        rect.bottom <= rect.top) {
        return;
    }
    auto* pixels = static_cast<BYTE*>(bitmapBits);
    const size_t rowBytes =
        static_cast<size_t>(rect.right - rect.left) * 4;
    for (LONG y = rect.top; y < rect.bottom; ++y) {
        memset(
            pixels +
                (static_cast<size_t>(y) *
                 static_cast<size_t>(width) +
                 static_cast<size_t>(rect.left)) * 4,
            0, rowBytes);
    }
}

void DesktopOverlayWindow::Render() {
    if (!hwnd) return;

    if (!renderer.CreateRenderTarget(memDC, width, height)) {
        forceFullRedraw = true;
        backbufferHasCompleteFrame = false;
        previousInteractiveDirtyValid = false;
        return;
    }

    RECT wr;
    GetWindowRect(hwnd, &wr);
    POINT windowOrigin = { wr.left, wr.top };

    bool isMoving = (interactionState == FenceInteractionState::MovingFence);
    bool isDragging = (interactionState == FenceInteractionState::DraggingItem);
    bool hasLiftedItem = isDragging || interactionState == FenceInteractionState::PressedItem;

    // Rendering is read-only. Avoid a deep copy of every path, item and icon
    // rectangle on each mouse move.
    const auto& fences = fenceManager->GetAllFencesRef();

    const auto unionRect = [](RECT first, const RECT& second) {
        if (first.right <= first.left ||
            first.bottom <= first.top) {
            return second;
        }
        RECT result{};
        UnionRect(&result, &first, &second);
        return result;
    };
    RECT currentInteractiveRect{};
    bool interactiveFrame = false;
    if (isMoving) {
        if (const Fence* movingFence =
                fenceManager->GetFenceById(
                    moveContext.fenceId)) {
            currentInteractiveRect = movingFence->rect;
            interactiveFrame = true;
        }
        for (const auto& line : currentSnapLines) {
            if (!line.visible) continue;
            RECT lineRect{
                (std::min)(line.start.x, line.end.x),
                (std::min)(line.start.y, line.end.y),
                (std::max)(line.start.x, line.end.x) + 1,
                (std::max)(line.start.y, line.end.y) + 1
            };
            currentInteractiveRect =
                unionRect(currentInteractiveRect, lineRect);
        }
    } else if (isDragging) {
        interactiveFrame = true;
        RECT ghostRect{
            ghostIcon.position.x - 12,
            ghostIcon.position.y - 12,
            ghostIcon.position.x +
                (std::max)(ghostIcon.iconSize, 100) + 12,
            ghostIcon.position.y +
                ghostIcon.iconSize + 72
        };
        currentInteractiveRect = ghostRect;
        if (const Fence* source =
                fenceManager->GetFenceById(
                    dragContext.sourceFenceId)) {
            currentInteractiveRect =
                unionRect(
                    currentInteractiveRect, source->rect);
        }
        if (dragContext.currentTarget.type ==
            DropTargetType::Fence) {
            if (const Fence* target =
                    fenceManager->GetFenceById(
                        dragContext.currentTarget.fenceId)) {
                currentInteractiveRect =
                    unionRect(
                        currentInteractiveRect,
                        target->rect);
            }
        }
    }

    RECT dirtyScreenRect = currentInteractiveRect;
    const bool completeFrameReady =
        backbufferHasCompleteFrame &&
        !forceFullRedraw &&
        completeFrameGeneration == backbufferGeneration &&
        !displayTopologyDirty;
    bool useDirtyRender = completeFrameReady &&
        (interactiveFrame || previousInteractiveDirtyValid);
    if (previousInteractiveDirtyValid) {
        dirtyScreenRect = unionRect(
            dirtyScreenRect,
            previousInteractiveDirtyRect);
    } else if (isMoving) {
        dirtyScreenRect = unionRect(
            dirtyScreenRect, moveContext.startRect);
    }
    RECT dirtyLocalRect{};
    if (useDirtyRender) {
        InflateRect(&dirtyScreenRect, 16, 16);
        dirtyLocalRect = dirtyScreenRect;
        OffsetRect(
            &dirtyLocalRect,
            -windowOrigin.x, -windowOrigin.y);
        RECT backbufferBounds{
            0, 0,
            static_cast<LONG>(width),
            static_cast<LONG>(height)
        };
        RECT clipped{};
        if (!IntersectRect(
                &clipped, &dirtyLocalRect,
                &backbufferBounds)) {
            useDirtyRender = false;
        } else {
            dirtyLocalRect = clipped;
            ClearBackbufferRect(dirtyLocalRect);
        }
    }

    renderer.BeginFrame(
        useDirtyRender ? &dirtyLocalRect : nullptr);

    if (fenceManager->IsAllVisible()) {
        for (size_t i = 0; i < fences.size(); ++i) {
            if (!fences[i].monitorOnline) continue;
            bool isHovered = (fences[i].id == hoverFenceId);
            bool isActive = (fences[i].id == activeFenceId);
            bool thisMoving = (isMoving && fences[i].id == moveContext.fenceId);
            bool thisDragSource = (hasLiftedItem && dragContext.sourceType == DragSourceType::FenceItem
                                   && fences[i].id == dragContext.sourceFenceId);
            renderer.DrawFence(fences[i], isActive, isHovered || thisMoving, thisMoving,
                               thisDragSource, dragContext.sourceItemIndex, windowOrigin);
        }
    }

    if (isDragging && dragContext.sourceType == DragSourceType::FenceItem) {
        DesktopItem* item = fenceManager->GetItem(dragContext.sourceFenceId, dragContext.sourceItemIndex);
        if (item) {
            ID2D1Bitmap* iconBmp = fenceManager->GetIconCache()
                ? fenceManager->GetIconCache()->GetOrCreateBitmap(*item, renderer.GetRenderTarget())
                : nullptr;
            renderer.DrawGhostIcon(ghostIcon, iconBmp, windowOrigin);
        }
    }

    if (isMoving && !currentSnapLines.empty()) {
        snapRenderer.DrawSnapLines(renderer.GetRenderTarget(), currentSnapLines, windowOrigin);
    }

    // 绘制重命名输入框
    if (textInputMode == TextInputMode::Rename && !textInputFenceId.empty()) {
        Fence* fence = fenceManager->GetFenceById(textInputFenceId);
        if (fence) {
            TextInputState state;
            state.active = true;
            state.text = textInputText;
            state.caretPos = textInputCaret;
            state.caretBlinkOn = caretBlinkOn;
            state.rect = GetRenameInputRect(*fence);
            renderer.DrawRenameInput(state, windowOrigin);
        }
    }

    // Draw search input.
    if (textInputMode == TextInputMode::Search && !textInputFenceId.empty()) {
        Fence* fence = fenceManager->GetFenceById(textInputFenceId);
        if (fence) {
            TextInputState state;
            state.active = true;
            state.text = textInputText;
            state.caretPos = textInputCaret;
            state.caretBlinkOn = caretBlinkOn;
            state.rect = GetSearchInputRect(*fence);
            renderer.DrawSearchInput(*fence, state, windowOrigin);
        }
    }

    const auto invalidateIncompleteFrame = [&]() {
        forceFullRedraw = true;
        backbufferHasCompleteFrame = false;
        completeFrameGeneration = 0;
        previousInteractiveDirtyValid = false;
    };
    if (!renderer.EndFrame()) {
        // Direct2D can lose a DC target during a display mode switch. The
        // dirty region has already been cleared, so publishing this bitmap
        // would make unrelated fences disappear. Keep the old compositor
        // frame and retry with a complete redraw instead.
        ResetRenderResources();
        invalidateIncompleteFrame();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) {
        invalidateIncompleteFrame();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    POINT ptSrc = { 0, 0 };
    POINT winPos = { wr.left, wr.top };
    SIZE winSize = { width, height };
    bool uploaded = false;
    if (useDirtyRender) {
        UPDATELAYEREDWINDOWINFO update{
            sizeof(UPDATELAYEREDWINDOWINFO)
        };
        update.hdcDst = screenDC;
        update.pptDst = &winPos;
        update.psize = &winSize;
        update.hdcSrc = memDC;
        update.pptSrc = &ptSrc;
        update.pblend = &blend;
        update.dwFlags = ULW_ALPHA;
        update.prcDirty = &dirtyLocalRect;
        uploaded = UpdateLayeredWindowIndirect(hwnd, &update) != FALSE;
    } else {
        // A full frame is the only safe fallback after a buffer/target
        // invalidation. Never upload a partially rendered bitmap as a full
        // layered-window update.
        uploaded = UpdateLayeredWindow(
            hwnd, screenDC, &winPos, &winSize,
            memDC, &ptSrc, 0, &blend, ULW_ALPHA) != FALSE;
    }
    ReleaseDC(nullptr, screenDC);
    if (!uploaded) {
        invalidateIncompleteFrame();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (!useDirtyRender) {
        forceFullRedraw = false;
        backbufferHasCompleteFrame = true;
        completeFrameGeneration = backbufferGeneration;
        // A topology transaction is not interactive until its replacement
        // backbuffer has actually made it through the layered-window upload.
        // This prevents a user drag from starting in the small gap where the
        // fences are already repositioned but the compositor still contains
        // the old/incomplete frame.
        if (topologyFramePending) {
            topologyFramePending = false;
            topologyReady = true;
            if (topologyRefreshSerial == topologyFrameSerial) {
                displayTopologyRefreshPending = false;
            }
        } else if (!displayTopologyRefreshPending && !displayTopologyDirty) {
            topologyReady = true;
        }
    }
    if (interactiveFrame) {
        previousInteractiveDirtyRect =
            currentInteractiveRect;
        previousInteractiveDirtyValid = true;
    } else {
        previousInteractiveDirtyRect = {};
        previousInteractiveDirtyValid = false;
    }
    lastRenderTick = GetTickCount64();

    // 动画期间持续重绘
    if (HasActiveAnimation()) {
        SetTimer(hwnd, ANIM_TIMER_ID, ANIM_FRAME_MS, nullptr);
    }
}

bool DesktopOverlayWindow::CreateTrayIcon(HINSTANCE hInst) {
    nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_ICON;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"DeskTodo.Next");
    return Shell_NotifyIconW(NIM_ADD, &nid) == TRUE;
}

void DesktopOverlayWindow::DestroyTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void DesktopOverlayWindow::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 3001, L"\u5237\u65b0\u683c\u5b50");
    AppendMenuW(menu, MF_STRING, 3002, L"\u65b0\u5efa\u683c\u5b50");
    AppendMenuW(menu, MF_STRING, 3013, L"\u65b0\u5efa\u6e38\u620f\u5bb9\u5668");
    AppendMenuW(menu, MF_STRING, 3003, L"\u4e00\u952e\u6574\u7406\u684c\u9762");
    AppendMenuW(menu, MF_STRING, 3004, fenceManager->IsAllVisible() ? L"\u9690\u85cf\u6240\u6709\u683c\u5b50" : L"\u663e\u793a\u6240\u6709\u683c\u5b50");
    AppendMenuW(menu, MF_STRING, 3005, L"\u9501\u5b9a/\u89e3\u9501\u5168\u90e8\u683c\u5b50");
    AppendMenuW(menu, MF_STRING, 3010, L"\u622a\u56fe");
    AppendMenuW(menu, MF_STRING, 3012, L"\u6587\u4ef6\u4f20\u9001");
    AppendMenuW(menu, MF_STRING, 3008, L"\u5907\u4efd\u684c\u9762\u5e03\u5c40");
    AppendMenuW(menu, MF_STRING, 3009, L"\u8fd8\u539f\u684c\u9762\u5e03\u5c40");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const bool bhRunning = blackHoleBackground && blackHoleBackground->IsRunning();
    AppendMenuW(menu, MF_STRING, 3011,
        bhRunning ? L"\u5173\u95ed\u9ed1\u6d1e\u52a8\u6001\u80cc\u666f" : L"\u5f00\u542f\u9ed1\u6d1e\u52a8\u6001\u80cc\u666f");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3006, L"\u8bbe\u7f6e");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3007, L"\u9000\u51fa DeskTodo");

    POINT pt;
    GetPhysicalCursorPos(&pt);

    const UINT selected = TrackForegroundPopupMenu(
        hwnd, menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt);
    PlaceInDesktopLayer();
    if (selected != 0 && !displayTopologyDirty) {
        PostMessageW(hwnd, WM_COMMAND, selected, 0);
    }

    DestroyMenu(menu);
}

void DesktopOverlayWindow::UpdateMovingFence(POINT pt) {
    if (displayTopologyDirty ||
        interactionState != FenceInteractionState::MovingFence) return;
    if (interactionTopologyGeneration != topologyGeneration) {
        CancelMove();
        return;
    }

    Fence* fence = fenceManager->GetFenceById(moveContext.fenceId);
    if (!fence) return;

    int dx = pt.x - moveContext.startMouse.x;
    int dy = pt.y - moveContext.startMouse.y;

    RECT proposedRect = moveContext.startRect;
    OffsetRect(&proposedRect, dx, dy);

    const auto* topology = interactionTopology.signature.empty()
        ? nullptr : &interactionTopology;
    if (topology) {
        MonitorService::ApplyFenceBounds(proposedRect, *topology);
    } else {
        MonitorService::ApplyFenceBounds(proposedRect);
    }

    SnapResult snap = snapManager.ApplySnap(
        moveContext.fenceId,
        proposedRect,
        fenceManager->GetAllFencesRef(),
        topology ? *topology : displayTopologySnapshot
    );

    RECT finalRect = snap.snappedRect;
    if (topology) {
        MonitorService::ApplyFenceBounds(finalRect, *topology);
    } else {
        MonitorService::ApplyFenceBounds(finalRect);
    }

    const LONG offsetX = finalRect.left - fence->rect.left;
    const LONG offsetY = finalRect.top - fence->rect.top;
    fence->rect = finalRect;

    currentSnapLines = snap.lines;

    // A pure fence move does not change its content geometry. Translating the
    // cached item rectangles is substantially cheaper than recomputing the
    // complete grid for every high-frequency WM_MOUSEMOVE.
    if (offsetX != 0 || offsetY != 0) {
        for (auto& item : fence->items) {
            OffsetRect(&item.iconRect, offsetX, offsetY);
            OffsetRect(&item.textRect, offsetX, offsetY);
            OffsetRect(&item.hitRect, offsetX, offsetY);
        }
    }

    RequestRedraw();
}

void DesktopOverlayWindow::CancelMove() {
    if (interactionState == FenceInteractionState::MovingFence ||
        interactionState == FenceInteractionState::PressedTitleBar) {
        if (moveContext.active) {
            if (Fence* fence =
                    fenceManager->GetFenceById(moveContext.fenceId)) {
                fence->rect = moveContext.startRect;
                fenceManager->LayoutFenceItems(fence->id);
            }
        }
        currentSnapLines.clear();
        interactionState = FenceInteractionState::None;
        moveContext = {};
        interactionTopology = {};
        interactionTopologyGeneration = 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        RequestRedraw();
    }
}

void DesktopOverlayWindow::CancelItemDrag() {
    if (interactionState == FenceInteractionState::PressedItem ||
        interactionState == FenceInteractionState::DraggingItem) {
        RestoreDraggedItemVisualState();
        ClearDragContext();
        interactionState = FenceInteractionState::None;
        if (GetCapture() == hwnd) ReleaseCapture();
        HideTooltip();
        RequestRedraw();
    }
}

void DesktopOverlayWindow::ClearDragContext() {
    dragContext = DragContext{};
    ghostIcon = GhostIcon{};
    currentDropFenceId = L"";
    fenceManager->ClearDropTargetState();
}

void DesktopOverlayWindow::RestoreDraggedItemVisualState() {
    DesktopItem* item = fenceManager->GetItem(dragContext.sourceFenceId, dragContext.sourceItemIndex);
    if (item) {
        item->hovered = false;
        item->selected = true;
    }
    dragContext.hideOriginalIcon = true;
    dragContext.dimOriginalIcon = false;
}

void DesktopOverlayWindow::StartTooltipTimer(const std::wstring& fenceId, int itemIndex) {
    CancelTooltipTimer();
    tooltipFenceId = fenceId;
    tooltipItemIndex = itemIndex;
    tooltipTimerActive = true;
    SetTimer(hwnd, TOOLTIP_TIMER_ID, TOOLTIP_DELAY_MS, nullptr);
}

void DesktopOverlayWindow::CancelTooltipTimer() {
    if (tooltipTimerActive) {
        KillTimer(hwnd, TOOLTIP_TIMER_ID);
        tooltipTimerActive = false;
    }
    tooltipFenceId = L"";
    tooltipItemIndex = -1;
}

void DesktopOverlayWindow::ShowItemTooltip(const std::wstring& fenceId, int itemIndex) {
    Fence* fence = fenceManager->GetFenceById(fenceId);
    if (!fence) return;
    if (itemIndex < 0 || itemIndex >= (int)fence->items.size()) return;

    if (!tooltipService) return;

    POINT pt{};
    if (!GetPhysicalCursorPos(&pt)) return;
    std::wstring currentFenceId;
    const int currentItemIndex = fenceManager->HitTestItem(pt, currentFenceId);
    if (currentItemIndex != itemIndex || currentFenceId != fenceId) {
        HideTooltip();
        return;
    }

    const DesktopItem& item = fence->items[itemIndex];
    tooltipService->Show(hwnd, item.name, pt.x, pt.y);
    visibleTooltipFenceId = fenceId;
    visibleTooltipItemIndex = itemIndex;
    SetTimer(hwnd, TOOLTIP_GUARD_TIMER_ID, TOOLTIP_GUARD_INTERVAL_MS, nullptr);
}

void DesktopOverlayWindow::ShowControlTooltip(const std::wstring& text, POINT pt) {
    if (tooltipService) {
        tooltipService->Show(hwnd, text, pt.x, pt.y);
    }
}

void DesktopOverlayWindow::HideTooltip() {
    KillTimer(hwnd, TOOLTIP_GUARD_TIMER_ID);
    visibleTooltipFenceId.clear();
    visibleTooltipItemIndex = -1;
    if (tooltipService) tooltipService->Hide();
}

// ===== 拉伸相关 =====

void DesktopOverlayWindow::StartResize(const std::wstring& fenceId, ResizeEdge edge, POINT pt) {
    if (displayTopologyDirty) return;
    Fence* fence = fenceManager->GetFenceById(fenceId);
    if (!fence || fence->locked) return;

    resizeContext.active = true;
    resizeContext.fenceId = fenceId;
    resizeContext.startMouse = pt;
    resizeContext.startRect = fence->rect;
    resizeContext.startExpandedHeight = fence->expandedHeight;
    resizeContext.edge = edge;
    interactionTopology = displayTopologySnapshot;
    interactionTopologyGeneration = topologyGeneration;
    if (interactionTopology.signature.empty()) {
        interactionTopology = MonitorService::CaptureTopology();
    }

    interactionState = FenceInteractionState::ResizingFence;
    SetCapture(hwnd);
    RequestRedraw();
}

void DesktopOverlayWindow::UpdateResize(POINT pt) {
    if (displayTopologyDirty ||
        interactionState != FenceInteractionState::ResizingFence) return;
    if (interactionTopologyGeneration != topologyGeneration) {
        CancelResize();
        return;
    }
    Fence* fence = fenceManager->GetFenceById(resizeContext.fenceId);
    if (!fence) return;

    RECT newRect = resizeContext.startRect;
    int dx = pt.x - resizeContext.startMouse.x;
    int dy = pt.y - resizeContext.startMouse.y;

    if (HasResizeEdge(resizeContext.edge, ResizeEdge::Left))
        newRect.left = resizeContext.startRect.left + dx;
    if (HasResizeEdge(resizeContext.edge, ResizeEdge::Right))
        newRect.right = resizeContext.startRect.right + dx;
    if (HasResizeEdge(resizeContext.edge, ResizeEdge::Top))
        newRect.top = resizeContext.startRect.top + dy;
    if (HasResizeEdge(resizeContext.edge, ResizeEdge::Bottom))
        newRect.bottom = resizeContext.startRect.bottom + dy;

    const auto* topology = interactionTopology.signature.empty()
        ? nullptr : &interactionTopology;
    fenceManager->ResizeFence(resizeContext.fenceId, newRect, topology);
    RequestRedraw();
}

void DesktopOverlayWindow::EndResize() {
    if (interactionState != FenceInteractionState::ResizingFence) return;
    if (displayTopologyDirty) {
        CancelResize();
        return;
    }
    const std::wstring resizedFenceId = resizeContext.fenceId;
    const auto placementTopology = interactionTopology;
    const auto* topology = placementTopology.signature.empty()
        ? nullptr : &placementTopology;
    interactionState = FenceInteractionState::None;
    if (GetCapture() == hwnd) ReleaseCapture();
    fenceManager->CommitFencePlacement(resizedFenceId, topology);
    fenceManager->SaveConfig();
    DesktopShellService::ReconcileSuppressedDesktopPaths();
    resizeContext = {};
    interactionTopology = {};
    interactionTopologyGeneration = 0;
    RequestRedraw();
}

void DesktopOverlayWindow::CancelResize() {
    if (!resizeContext.active &&
        interactionState != FenceInteractionState::ResizingFence) return;

    if (resizeContext.active) {
        if (Fence* fence = fenceManager->GetFenceById(resizeContext.fenceId)) {
            fence->rect = resizeContext.startRect;
            fence->expandedHeight = resizeContext.startExpandedHeight;
            fenceManager->LayoutFenceItems(fence->id);
        }
    }

        resizeContext = {};
        interactionTopology = {};
        interactionTopologyGeneration = 0;
        if (interactionState == FenceInteractionState::ResizingFence) {
        interactionState = FenceInteractionState::None;
    }
    if (GetCapture() == hwnd) ReleaseCapture();
    RequestRedraw();
}

// ===== 文本输入相关 =====

RECT DesktopOverlayWindow::GetRenameInputRect(const Fence& fence) const {
    // 覆盖标题文字区域
    RECT r;
    const int width = fence.rect.right - fence.rect.left;
    r.left = fence.rect.left + (width < 240 ? 32 : 70);
    r.right = fence.rect.right - (width < 240 ? 70 : 110);
    r.top = fence.rect.top + 4;
    r.bottom = fence.rect.top + FENCE_TITLE_BAR_HEIGHT - 4;
    return r;
}

RECT DesktopOverlayWindow::GetSearchInputRect(const Fence& fence) const {
    // Search box in the content area.
    RECT r;
    r.left = fence.rect.left + 10;
    r.right = fence.rect.right - 10;
    r.top = fence.rect.top + FENCE_TITLE_BAR_HEIGHT + 6;
    r.bottom = r.top + 26;
    return r;
}

bool DesktopOverlayWindow::IsPointInSearchClearButton(POINT pt) const {
    if (textInputMode != TextInputMode::Search || textInputFenceId.empty()) return false;
    Fence* fence = fenceManager->GetFenceById(textInputFenceId);
    if (!fence) return false;
    RECT sr = GetSearchInputRect(*fence);
    // Clear button is the right-side hit area.
    RECT clearRect{ sr.right - 22, sr.top, sr.right - 2, sr.bottom };
    return PtInRect(&clearRect, pt) == TRUE;
}

bool DesktopOverlayWindow::IsPointInTextInput(POINT pt) const {
    if (textInputMode == TextInputMode::None || textInputFenceId.empty()) return false;
    Fence* fence = fenceManager->GetFenceById(textInputFenceId);
    if (!fence) return false;
    RECT ir = (textInputMode == TextInputMode::Rename) ? GetRenameInputRect(*fence) : GetSearchInputRect(*fence);
    return PtInRect(&ir, pt) == TRUE;
}

void DesktopOverlayWindow::EnterTextInputMode(TextInputMode mode, const std::wstring& fenceId) {
    Fence* fence = fenceManager->GetFenceById(fenceId);
    if (!fence) return;
    if (mode == TextInputMode::Rename && fence->isMappedFolder) return;

    // 退出之前的输入模式
    if (textInputMode != TextInputMode::None) {
        ExitTextInputMode(true);
    }

    textInputMode = mode;
    textInputFenceId = fenceId;
    textInputCaret = 0;
    caretBlinkOn = true;

    if (mode == TextInputMode::Rename) {
        textInputText = fence->title;
        textInputCaret = (int)fence->title.size();
    } else if (mode == TextInputMode::Search) {
        textInputText = fence->searchQuery;
        textInputCaret = (int)fence->searchQuery.size();
        fence->searchActive = true;
    }

    // Temporarily remove WS_EX_NOACTIVATE to receive keyboard input.
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    textInputHadNoActivate = (exStyle & WS_EX_NOACTIVATE) != 0;
    if (textInputHadNoActivate) {
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_NOACTIVATE);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground
        ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();
    const bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
                          AttachThreadInput(currentThread, foregroundThread, TRUE);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (attached) AttachThreadInput(currentThread, foregroundThread, FALSE);

    interactionState = FenceInteractionState::TextInput;
    SetTimer(hwnd, CARET_TIMER_ID, CARET_BLINK_MS, nullptr);
    RequestRedraw();
}

void DesktopOverlayWindow::ExitTextInputMode(bool commit) {
    if (textInputMode == TextInputMode::None) return;

    KillTimer(hwnd, CARET_TIMER_ID);

    std::wstring fenceId = textInputFenceId;
    TextInputMode mode = textInputMode;
    std::wstring text = textInputText;

    textInputMode = TextInputMode::None;
    textInputFenceId.clear();
    textInputText.clear();
    textInputCaret = 0;
    interactionState = FenceInteractionState::None;

    // 恢复 WS_EX_NOACTIVATE
    if (textInputHadNoActivate) {
        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }

    if (commit) {
        if (mode == TextInputMode::Rename) {
            // Empty names are not committed.
            Fence* fence = fenceManager->GetFenceById(fenceId);
            if (fence) {
                std::wstring trimmed = text;
                while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t')) trimmed.erase(trimmed.begin());
                while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t')) trimmed.pop_back();
                if (!trimmed.empty()) {
                    fenceManager->RenameFence(fenceId, text);
                }
            }
        } else if (mode == TextInputMode::Search) {
            if (text.empty()) fenceManager->ClearFenceSearch(fenceId);
            else fenceManager->SetFenceSearch(fenceId, text);
        }
    } else {
        // 取消：搜索则清空
        if (mode == TextInputMode::Search) {
            Fence* fence = fenceManager->GetFenceById(fenceId);
            if (fence && text.empty()) {
                fenceManager->ClearFenceSearch(fenceId);
            }
        }
    }

    RequestRedraw();
}

void DesktopOverlayWindow::CommitRename() {
    ExitTextInputMode(true);
}

void DesktopOverlayWindow::CommitSearchText(const std::wstring& text) {
    if (textInputFenceId.empty()) return;
    if (text.empty()) {
        fenceManager->ClearFenceSearch(textInputFenceId);
    } else {
        fenceManager->SetFenceSearch(textInputFenceId, text);
    }
}

void DesktopOverlayWindow::HandleCharInput(wchar_t ch) {
    if (textInputMode == TextInputMode::None) return;

    // Printable characters.
    if (ch >= 0x20 && ch != 0x7F) {
        textInputText.insert(textInputText.begin() + textInputCaret, ch);
        ++textInputCaret;
        caretBlinkOn = true;

        if (textInputMode == TextInputMode::Search) {
            CommitSearchText(textInputText);
        }
        RequestRedraw();
    }
}

void DesktopOverlayWindow::HandleKeyDownInput(WPARAM vk) {
    if (textInputMode == TextInputMode::None) return;

    switch (vk) {
    case VK_RETURN:
        ExitTextInputMode(true);
        break;
    case VK_ESCAPE:
        ExitTextInputMode(false);
        break;
    case VK_BACK:
        if (textInputCaret > 0) {
            textInputText.erase(textInputText.begin() + textInputCaret - 1);
            --textInputCaret;
            caretBlinkOn = true;
            if (textInputMode == TextInputMode::Search) {
                CommitSearchText(textInputText);
            }
            RequestRedraw();
        }
        break;
    case VK_DELETE:
        if (textInputCaret < (int)textInputText.size()) {
            textInputText.erase(textInputText.begin() + textInputCaret);
            caretBlinkOn = true;
            if (textInputMode == TextInputMode::Search) {
                CommitSearchText(textInputText);
            }
            RequestRedraw();
        }
        break;
    case VK_LEFT:
        if (textInputCaret > 0) --textInputCaret;
        caretBlinkOn = true;
        RequestRedraw();
        break;
    case VK_RIGHT:
        if (textInputCaret < (int)textInputText.size()) ++textInputCaret;
        caretBlinkOn = true;
        RequestRedraw();
        break;
    case VK_HOME:
        textInputCaret = 0;
        caretBlinkOn = true;
        RequestRedraw();
        break;
    case VK_END:
        textInputCaret = (int)textInputText.size();
        caretBlinkOn = true;
        RequestRedraw();
        break;
    default:
        break;
    }
}

void DesktopOverlayWindow::UpdateCaretBlink() {
    caretBlinkOn = !caretBlinkOn;
    RequestRedraw();
}

bool DesktopOverlayWindow::HasActiveAnimation() const {
    for (const auto& fence : fenceManager->GetAllFencesRef()) {
        if (fence.lockAnimActive) return true;
    }
    return false;
}

// ===== 项目拖拽相关 =====

void DesktopOverlayWindow::BeginPressedItem(const std::wstring& fenceId, int itemIndex, POINT pt) {
    Fence* fence = fenceManager->GetFenceById(fenceId);
    if (!fence) return;
    if (itemIndex < 0 || itemIndex >= (int)fence->items.size()) return;

    DesktopItem* item = &fence->items[itemIndex];

    interactionState = FenceInteractionState::PressedItem;

    dragContext = DragContext{};
    dragContext.active = true;
    dragContext.hasMoved = false;
    dragContext.sourceType = DragSourceType::FenceItem;
    dragContext.sourceFenceId = fenceId;
    dragContext.sourceItemIndex = itemIndex;
    dragContext.sourcePath = item->fullPath;
    dragContext.originalPath = item->fullPath;
    dragContext.startMouse = pt;
    dragContext.currentMouse = pt;
    dragContext.originalIconRect = item->iconRect;
    dragContext.originalTextRect = item->textRect;
    dragContext.originalHitRect = item->hitRect;
    dragContext.hideOriginalIcon = false;
    dragContext.dimOriginalIcon = false;

    fenceManager->ClearItemSelection(fenceId);
    item->selected = true;

    activeFenceId = fenceId;
    fenceManager->BringToFront(fenceId);

    HideTooltip();
    CancelTooltipTimer();

    SetCapture(hwnd);
    RequestRedraw();
}

void DesktopOverlayWindow::UpdatePressedItem(POINT pt) {
    if (interactionState != FenceInteractionState::PressedItem) return;
    if (!dragContext.active) return;

    int dx = pt.x - dragContext.startMouse.x;
    int dy = pt.y - dragContext.startMouse.y;
    if (std::abs(dx) < ITEM_DRAG_START_THRESHOLD &&
        std::abs(dy) < ITEM_DRAG_START_THRESHOLD) return;

    interactionState = FenceInteractionState::DraggingItem;
    dragContext.hasMoved = true;
    dragContext.hideOriginalIcon = true;
    dragContext.dimOriginalIcon = false;
    dragContext.currentMouse = pt;
    dragContext.currentTarget = DetectDropTarget(pt);
    UpdateItemDragVisual(pt);
    ghostIcon.opacity = 0.75f;

    DesktopItem* item = fenceManager->GetItem(dragContext.sourceFenceId, dragContext.sourceItemIndex);
    if (item) {
        ghostIcon.name = item->name;
    }

    HideTooltip();
    CancelTooltipTimer();

    RequestRedraw();
}

void DesktopOverlayWindow::EndPressedItemWithoutDrag(POINT pt) {
    fenceManager->ClearItemSelection(dragContext.sourceFenceId);
    DesktopItem* item = fenceManager->GetItem(dragContext.sourceFenceId, dragContext.sourceItemIndex);
    if (item) item->selected = true;
    ClearDragContext();
    interactionState = FenceInteractionState::None;
    if (GetCapture() == hwnd) ReleaseCapture();
    RequestRedraw();
}

void DesktopOverlayWindow::UpdateDraggingItem(POINT pt) {
    if (interactionState != FenceInteractionState::DraggingItem) return;

    dragContext.currentMouse = pt;
    dragContext.currentTarget = DetectDropTarget(pt);
    UpdateItemDragVisual(pt);

    fenceManager->ClearDropTargetState();
    if (dragContext.currentTarget.type == DropTargetType::Fence) {
        Fence* target = fenceManager->GetFenceById(dragContext.currentTarget.fenceId);
        if (target) {
            target->dropTarget = true;
            currentDropFenceId = target->id;
        }
    } else {
        currentDropFenceId = L"";
    }

    RequestRedraw();
}

void DesktopOverlayWindow::UpdateItemDragVisual(POINT pt) {
    const bool droppingToDesktop =
        dragContext.currentTarget.type == DropTargetType::Desktop;
    const bool droppingToFence =
        dragContext.currentTarget.type == DropTargetType::Fence;
    ghostIcon.visible = droppingToDesktop || droppingToFence;
    dragContext.hasSnappedPoint = false;

    if (!ghostIcon.visible) return;

    ghostIcon.iconSize = droppingToDesktop
        ? (std::max)(1, GridLayoutService::GetRawDesktopIconSize(pt))
        : (std::max)(1L,
            dragContext.originalIconRect.right - dragContext.originalIconRect.left);

    // Native desktop dragging follows the pointer continuously. Grid snapping
    // is intentionally deferred until mouse-up so mixed-DPI monitor crossings
    // do not turn smooth mouse movement into visible cell-by-cell jumps.
    ghostIcon.position = GridLayoutService::GetDraggedDesktopIconTopLeft(
        dragContext.startMouse, dragContext.originalIconRect, pt, ghostIcon.iconSize);
}

void DesktopOverlayWindow::EndDraggingItem(POINT pt) {
    DropTargetInfo target = DetectDropTarget(pt);

    interactionState = FenceInteractionState::None;
    if (GetCapture() == hwnd) ReleaseCapture();

    bool ok = false;

    if (target.type == DropTargetType::Fence) {
        ok = DropItemToFence(target.fenceId, pt);
    } else if (target.type == DropTargetType::Desktop) {
        ok = DropItemToDesktop(pt);
    }

    if (!ok) {
        RestoreDraggedItemVisualState();
    }

    ClearDragContext();
    RequestRedraw();
}

DropTargetInfo DesktopOverlayWindow::DetectDropTarget(POINT pt) const {
    auto fenceHit = fenceManager->HitTest(pt);

    if (fenceHit.hit) {
        if (fenceHit.area == FenceHitArea::Content) {
            const Fence* fence = fenceManager->GetFenceById(fenceHit.fenceId);
            if (fence && fence->kind == FenceKind::Clock) {
                return { DropTargetType::Invalid, L"" };
            }
            return { DropTargetType::Fence, fenceHit.fenceId };
        }
        return { DropTargetType::Invalid, L"" };
    }

    if (DesktopShellService::IsPointOnDesktop(pt)) {
        return { DropTargetType::Desktop, L"" };
    }

    return { DropTargetType::Invalid, L"" };
}

bool DesktopOverlayWindow::DropItemToFence(const std::wstring& targetFenceId, POINT pt) {
    Fence* targetFence = fenceManager->GetFenceById(targetFenceId);
    if (!targetFence) return false;

    DesktopItem* item = fenceManager->GetItem(dragContext.sourceFenceId, dragContext.sourceItemIndex);
    if (!item) return false;
    const std::wstring itemName = item->name;
    POINT desktopOrigin{};
    const bool hasDesktopOrigin = fenceManager->GetItemDesktopOrigin(
        dragContext.sourceFenceId, item->fullPath, desktopOrigin);

    if (targetFenceId == dragContext.sourceFenceId) {
        if (targetFence->kind == FenceKind::Game) return false;
        // Reorder inside the same fence without saving desktop coordinates.
        const bool reordered = fenceManager->ReorderItemAtPoint(targetFenceId, item->fullPath, pt);
        if (reordered) {
            fenceManager->SaveConfig();
            fenceManager->SyncLogicalDesktopPositions(targetFenceId);
        }
        return reordered;
    }

    if (targetFence->kind == FenceKind::Game) {
        const std::wstring path = item->fullPath;
        const bool added = fenceManager->AddLogicalItem(
            targetFenceId, path,
            hasDesktopOrigin ? desktopOrigin : POINT{ LONG_MIN, LONG_MIN });
        if (added) {
            fenceManager->RefreshAllFenceItems();
            fenceManager->SaveConfig();
        }
        return added;
    }

    if (fenceManager->IsLogicalItem(dragContext.sourceFenceId, item->fullPath) &&
        !targetFence->isMappedFolder) {
        const std::wstring path = item->fullPath;
        const bool moved = fenceManager->MoveLogicalItem(
            dragContext.sourceFenceId, targetFenceId, path);
        if (moved) {
            fenceManager->ReorderItemAtPoint(targetFenceId, path, pt);
            fenceManager->SaveConfig();
            fenceManager->SyncLogicalDesktopPositions(targetFenceId);
        }
        return moved;
    }

    if (!fileMoveService) return false;

    std::wstring movedPath;
    bool ok = MovePathWithConflictPrompt(item->fullPath, targetFence->folderPath, &movedPath);
    if (ok) {
        RefreshFenceAfterMove(dragContext.sourceFenceId, targetFenceId);
        const std::wstring movedName = std::filesystem::path(movedPath).filename().wstring();
        // 跨格子移动后重排序到目标位置
        fenceManager->ReorderItemAtPoint(targetFenceId, movedName, pt);
        if (hasDesktopOrigin) {
            fenceManager->SetItemDesktopOrigin(targetFenceId, movedName, desktopOrigin);
        }
        fenceManager->SaveConfig();
    }
    return ok;
}

bool DesktopOverlayWindow::DropItemToDesktop(POINT pt) {
    DesktopItem* item = fenceManager->GetItem(dragContext.sourceFenceId, dragContext.sourceItemIndex);
    if (!item) return false;

    const std::wstring itemPath = item->fullPath;
    std::wstring desktopPath = GetUserDesktopPath();
    if (desktopPath.empty()) return false;

    if (!fileMoveService) return false;
    struct NativeDesktopUpdateGuard {
        NativeDesktopUpdateGuard() {
            DesktopShellService::BeginNativeDesktopUpdate();
        }
        ~NativeDesktopUpdateGuard() {
            DesktopShellService::EndNativeDesktopUpdate();
        }
    } updateGuard;

    if (fenceManager->IsLogicalItem(dragContext.sourceFenceId, itemPath)) {
        const int iconSize = (std::max)(
            1, GridLayoutService::GetRawDesktopIconSize(pt));
        const POINT desiredTopLeft =
            GridLayoutService::GetDraggedDesktopIconTopLeft(
                dragContext.startMouse,
                dragContext.originalIconRect, pt, iconSize);
        const POINT snappedPoint =
            DesktopShellService::SnapPointToDesktopGrid(
                desiredTopLeft);
        const bool removed = fenceManager->RemoveLogicalItem(
            dragContext.sourceFenceId, itemPath,
            &snappedPoint);
        if (!removed) return false;
        return true;
    }

    std::wstring movedPath;
    bool ok = MovePathWithConflictPrompt(item->fullPath, desktopPath, &movedPath);
    if (ok) {
        if (!movedPath.empty()) {
            const int iconSize = (std::max)(1, GridLayoutService::GetRawDesktopIconSize(pt));
            const POINT desiredTopLeft = GridLayoutService::GetDraggedDesktopIconTopLeft(
                dragContext.startMouse, dragContext.originalIconRect, pt, iconSize);
            const POINT snappedPoint = DesktopShellService::SnapPointToDesktopGrid(desiredTopLeft);
            DesktopShellService::PositionPathOnDesktop(
                movedPath, snappedPoint);
        }
        RefreshFenceAfterMove(dragContext.sourceFenceId, L"");
        fenceManager->SaveConfig();
    }
    return ok;
}

bool DesktopOverlayWindow::MovePathWithConflictPrompt(const std::wstring& sourcePath,
                                                      const std::wstring& targetDirectory,
                                                      std::wstring* movedPath,
                                                      bool reserveDesktopName) {
    if (!fileMoveService) return false;

    FileMoveResult result = fileMoveService->MoveToDirectory(
        sourcePath, targetDirectory, false, movedPath, reserveDesktopName);
    if (result == FileMoveResult::Conflict) {
        const std::wstring targetPath =
            (std::filesystem::path(targetDirectory) /
             std::filesystem::path(sourcePath).filename()).wstring();
        const std::wstring message =
            L"目标位置已存在同名文件：\n\n" + targetPath +
            L"\n\n\u9009\u62e9\u201c\u662f\u201d\u8986\u76d6\uff0c\u9009\u62e9\u201c\u5426\u201d\u8df3\u8fc7\u3002";
        const int choice = MessageBoxW(hwnd, message.c_str(),
                                       L"DeskTodo - 文件冲突",
                                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (choice != IDYES) return false;
        // The modal conflict dialog pumps display-change messages. Never
        // perform the overwrite after the topology guard has been dirtied;
        // callers will retry against a fresh monitor snapshot.
        if (displayTopologyDirty) return false;
        result = fileMoveService->MoveToDirectory(
            sourcePath, targetDirectory, true, movedPath, reserveDesktopName);
    }

    return result == FileMoveResult::Success || result == FileMoveResult::SameLocation;
}

void DesktopOverlayWindow::RefreshFenceAfterMove(const std::wstring& sourceFenceId, const std::wstring& targetFenceId) {
    if (!sourceFenceId.empty()) {
        fenceManager->RefreshFenceItems(sourceFenceId);
    }
    if (!targetFenceId.empty() && targetFenceId != sourceFenceId) {
        fenceManager->RefreshFenceItems(targetFenceId);
    }
    RequestRedraw();
}

std::wstring DesktopOverlayWindow::QueryDropFilePath(HDROP hDrop, UINT index) {
    UINT length = DragQueryFileW(hDrop, index, nullptr, 0);
    if (length == 0) return L"";
    std::vector<wchar_t> buf(length + 1);
    DragQueryFileW(hDrop, index, buf.data(), (UINT)buf.size());
    return buf.data();
}

void DesktopOverlayWindow::OnDropFiles(HDROP hDrop) {
    POINT pt{};
    POINT dropClient{};
    if (DragQueryPoint(hDrop, &dropClient)) {
        pt = dropClient;
        ClientToScreen(hwnd, &pt);
        LogicalToPhysicalPointForPerMonitorDPI(
            hwnd, &pt);
    } else if (!TryGetOverlayPointer(hwnd, pt)) {
        DragFinish(hDrop);
        return;
    }

    const UINT count = DragQueryFileW(
        hDrop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        std::wstring path = QueryDropFilePath(hDrop, index);
        if (!path.empty()) paths.push_back(std::move(path));
    }
    DragFinish(hDrop);
    HandleDroppedPaths(paths, pt);
}

bool DesktopOverlayWindow::HandleDroppedPaths(
    const std::vector<std::wstring>& paths, POINT pt) {
    if (displayTopologyDirty || paths.empty()) return false;

    auto hit = fenceManager->HitTest(pt);
    if (!hit.hit || hit.area != FenceHitArea::Content) {
        return false;
    }

    Fence* targetFence = fenceManager->GetFenceById(hit.fenceId);
    if (!targetFence || targetFence->collapsed ||
        targetFence->kind == FenceKind::Clock) return false;
    const std::wstring targetFenceId = targetFence->id;
    const bool mappedFolder = targetFence->isMappedFolder;
    const FenceKind targetKind = targetFence->kind;
    const std::wstring mappedFolderPath = targetFence->folderPath;

    DesktopShellService::BeginNativeDesktopUpdate();
    std::vector<std::wstring> addedNames;
    int failedCount = 0;
    for (const auto& sourcePath : paths) {
        if (sourcePath.empty()) continue;
        POINT originalPosition{};
        const bool fromDesktop =
            IsDesktopFilesystemPath(sourcePath) &&
            DesktopShellService::GetPathPositionOnDesktop(
                sourcePath, originalPosition);
        bool added = false;
        std::wstring addedName = std::filesystem::path(sourcePath).filename().wstring();
        if (targetFence->isMappedFolder) {
            // A mapped fence is an actual directory view, so dropping into it
            // retains the established physical move semantics.
            std::wstring movedPath;
            added = MovePathWithConflictPrompt(
                sourcePath, mappedFolderPath, &movedPath, false);
            if (added) addedName = std::filesystem::path(movedPath).filename().wstring();
        } else {
            // A normal fence is logical grouping only. Keep files, shortcuts,
            // executables and whole project directories at their source path.
            added = fenceManager->AddLogicalItem(
                targetFenceId, sourcePath,
                fromDesktop ? originalPosition : POINT{ LONG_MIN, LONG_MIN }, true);
        }
        if (added) {
            addedNames.push_back(addedName);
        } else {
            ++failedCount;
        }
    }

    if (!addedNames.empty()) {
        DesktopShellService::ReconcileSuppressedDesktopPaths();
    }
    DesktopShellService::EndNativeDesktopUpdate();

    if (failedCount > 0) {
        const std::wstring message = targetKind == FenceKind::Game
            ? L"\u6e38\u620f\u5bb9\u5668\u53ea\u80fd\u6536\u7eb3\u4e00\u4e2a .lnk\u3001.url \u6216 .exe \u542f\u52a8\u9879\u3002"
            : (L"本次拖入成功 " + std::to_wstring(addedNames.size()) +
               L" 项，失败 " + std::to_wstring(failedCount) +
               (mappedFolder
                    ? L" 项。\n失败项可能被其他程序占用。"
                    : L" 项。\n失败项的源路径不存在，或桌面图标未能可靠收纳。"));
        MessageBoxW(hwnd, message.c_str(), L"DeskTodo",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST);
    }

    // Only the target changed. A full refresh can synchronously rescan every
    // mapped/network folder and make a successful drop appear frozen.
    fenceManager->RefreshFenceItems(
        targetFenceId, false);
    // 外部拖入的文件追加到末尾（顺序）
    fenceManager->SaveConfig();
    targetFence = fenceManager->GetFenceById(targetFenceId);
    if (fenceManager->GetIconCache()) {
        if (targetFence) {
            fenceManager->GetIconCache()->PreloadIconsForFence(*targetFence);
        }
    }
    RequestRedraw();
    return !addedNames.empty();
}

void DesktopOverlayWindow::ShowItemContextMenu(POINT pt) {
    std::wstring itemFenceId;
    int itemIdx = fenceManager->HitTestItem(pt, itemFenceId);
    if (itemIdx < 0) return;

    Fence* fence = fenceManager->GetFenceById(itemFenceId);
    if (!fence || itemIdx >= (int)fence->items.size()) return;

    const ItemContextMenuResult result = DesktopShellService::ShowItemContextMenu(
        hwnd, fence->items[itemIdx].fullPath, pt);
    if (result == ItemContextMenuResult::BackupLayout) {
        BackupLayout();
    } else if (result == ItemContextMenuResult::RestoreLayout) {
        RestoreLayout();
    } else if (result == ItemContextMenuResult::ShellCommand) {
        fenceManager->RefreshFenceItems(itemFenceId);
        RequestRedraw();
    }
}

// ===== 设置对话�?=====

static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wp));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void DesktopOverlayWindow::ShowSettingsDialog() {
    SettingsWindow::Show(hwnd, fenceManager);
    return;
}

// ===== 黑洞背景层 =====

void DesktopOverlayWindow::ToggleBlackHoleBackground() {
    if (!blackHoleBackground) return;
    if (blackHoleBackground->IsRunning()) {
        SetBlackHoleBackgroundEnabled(false);
    } else {
        SetBlackHoleBackgroundEnabled(true);
    }
}

void DesktopOverlayWindow::SetBlackHoleBackgroundEnabled(bool enabled) {
    if (!blackHoleBackground) return;
    if (enabled) {
        if (!blackHoleBackground->IsRunning()) {
            blackHoleBackground->Start(0, 0, 1.0f);
        }
    } else {
        blackHoleBackground->Stop();
    }
    RequestRedraw();
}

void DesktopOverlayWindow::UpdateBlackHoleBackgroundSize() {
    // The ghostty renderer owns a virtual-screen topmost window.
}

void DesktopOverlayWindow::CancelInteractionsForTopology() {
    const bool itemInteraction =
        interactionState == FenceInteractionState::PressedItem ||
        interactionState == FenceInteractionState::DraggingItem;
    const bool placementInteraction =
        moveContext.active || resizeContext.active ||
        interactionState == FenceInteractionState::PressedTitleBar ||
        interactionState == FenceInteractionState::MovingFence ||
        interactionState == FenceInteractionState::ResizingFence;
    if (!itemInteraction && !placementInteraction) return;

    // Restore the pre-interaction runtime geometry before clearing the
    // contexts. Persisted monitor-relative placement is deliberately untouched.
    if (moveContext.active) {
        if (Fence* fence = fenceManager->GetFenceById(moveContext.fenceId)) {
            fence->rect = moveContext.startRect;
            fenceManager->LayoutFenceItems(fence->id);
        }
    }
    if (resizeContext.active) {
        if (Fence* fence = fenceManager->GetFenceById(resizeContext.fenceId)) {
            fence->rect = resizeContext.startRect;
            fence->expandedHeight = resizeContext.startExpandedHeight;
            fenceManager->LayoutFenceItems(fence->id);
        }
    }
    if (itemInteraction) {
        RestoreDraggedItemVisualState();
        ClearDragContext();
        HideTooltip();
    }

    currentSnapLines.clear();
    moveContext = {};
    resizeContext = {};
    interactionTopology = {};
    interactionTopologyGeneration = 0;
    interactionState = FenceInteractionState::None;
    if (GetCapture() == hwnd) ReleaseCapture();
    RequestRedraw();
}

void DesktopOverlayWindow::ArmDisplayTopologyRefreshTimer() {
    if (!hwnd) return;
    KillTimer(hwnd, DISPLAY_TOPOLOGY_TIMER_ID);
    SetTimer(hwnd, DISPLAY_TOPOLOGY_TIMER_ID,
             DISPLAY_TOPOLOGY_SAMPLE_MS, nullptr);
}

std::vector<std::wstring>
DesktopOverlayWindow::CollectLogicalMemberPaths() const {
    std::vector<std::wstring> excludedPaths;
    if (!fenceManager) return excludedPaths;

    const auto& fences = fenceManager->GetAllFencesRef();
    size_t pathCount = 0;
    for (const auto& fence : fences) {
        pathCount += fence.memberPaths.size();
    }
    excludedPaths.reserve(pathCount);
    for (const auto& fence : fences) {
        excludedPaths.insert(
            excludedPaths.end(),
            fence.memberPaths.begin(),
            fence.memberPaths.end());
    }
    return excludedPaths;
}

void DesktopOverlayWindow::RestoreDesktopIconsAfterTopology(
    const MonitorService::DisplayTopologySnapshot& topology) {
    if (!desktopIconLayoutService || topology.signature.empty()) return;

    desktopIconLayoutService->BeginTopologyRecovery(
        topology, CollectLogicalMemberPaths());
}

void DesktopOverlayWindow::SyncStableDesktopIconLayout(
    const MonitorService::DisplayTopologySnapshot& topology) {
    if (!desktopIconLayoutService || displayTopologyDirty ||
        topology.signature.empty() ||
        topology.signature != displayTopologySignature) {
        return;
    }

    const auto excludedPaths = CollectLogicalMemberPaths();
    if (desktopIconLayoutService->HasPendingTopologyRecovery()) {
        desktopIconLayoutService->ProcessTopologyRecoveryBatch(6);
        return;
    }
    desktopIconLayoutService->CaptureStable(topology, excludedPaths);
}

void DesktopOverlayWindow::ScheduleDisplayTopologyRefresh(bool forceRecovery) {
    // A reconnect produces a burst of display/device/settings notifications.
    // Treat them as a hint to sample the topology later; do not cancel an
    // active drag here.  The old implementation reset the fence geometry on
    // the first notification, which caused the visible flash/jump after a
    // second monitor was turned back on.
    ++topologyRefreshSerial;
    displayTopologyRefreshPending = true;
    displayRecoveryPending = displayRecoveryPending || forceRecovery;
    ArmDisplayTopologyRefreshTimer();
}

void DesktopOverlayWindow::RefreshDisplayTopology() {
    if (!hwnd || !fenceManager) return;
    KillTimer(hwnd, DISPLAY_TOPOLOGY_TIMER_ID);

    // QueryDisplayConfig and monitor enumeration can block on the UI thread
    // while Windows is rebuilding a USB-C display path.  Never perform that
    // work in the middle of a pointer interaction; the active drag keeps its
    // immutable interactionTopology snapshot and the topology transaction is
    // committed after the button is released.
    const bool pointerInteraction =
        moveContext.active || resizeContext.active ||
        interactionState == FenceInteractionState::PressedTitleBar ||
        interactionState == FenceInteractionState::MovingFence ||
        interactionState == FenceInteractionState::PressedItem ||
        interactionState == FenceInteractionState::DraggingItem ||
        interactionState == FenceInteractionState::ResizingFence;
    if (pointerInteraction) {
        displayTopologyRefreshPending = true;
        ArmDisplayTopologyRefreshTimer();
        return;
    }

    const auto topology = MonitorService::CaptureTopology();
    if (topology.signature.empty()) {
        topologyCandidate = {};
        topologyCandidateSamples = 0;
        topologyReady = false;
        ArmDisplayTopologyRefreshTimer();
        return;
    }
    if (topology.signature == displayTopologySignature) {
        // Notifications such as WM_SETTINGCHANGE and power broadcasts are
        // often unrelated to the monitor layout.  A stable signature normally
        // means there is nothing to re-layout.  A power-cycle is different:
        // Windows can tear down and recreate the DWM/D2D path while returning
        // the exact same monitor signature, so force one compositor recovery
        // transaction for explicit display/device notifications.
        topologyCandidate = {};
        topologyCandidateSamples = 0;
        if (displayRecoveryPending) {
            const RECT virtualScreen = topology.virtualScreen;
            const int newWidth = virtualScreen.right - virtualScreen.left;
            const int newHeight = virtualScreen.bottom - virtualScreen.top;
            if (newWidth <= 0 || newHeight <= 0) {
                topologyReady = false;
                ArmDisplayTopologyRefreshTimer();
                return;
            }

            // A power-cycle can recreate the DWM/D2D path without changing
            // the virtual desktop rectangle. Always start the recovery with
            // a fresh target, and release it before EnsureBackbuffer can
            // replace the DIB/DC.
            ResetRenderResources();
            if (!EnsureBackbuffer(newWidth, newHeight)) {
                topologyReady = false;
                ArmDisplayTopologyRefreshTimer();
                return;
            }

            displayTopologyDirty = true;
            if (desktopHostWindow && !IsWindow(desktopHostWindow)) {
                AttachToDesktopHost(virtualScreen);
            }
            if (!PositionForVirtualScreen(virtualScreen)) {
                displayTopologyDirty = false;
                topologyReady = false;
                ArmDisplayTopologyRefreshTimer();
                return;
            }

            forceFullRedraw = true;
            backbufferHasCompleteFrame = false;
            completeFrameGeneration = 0;
            previousInteractiveDirtyValid = false;
            topologyReady = false;
            topologyFramePending = true;
            topologyFrameSerial = topologyRefreshSerial;
            const bool fenceTopologyChanged =
                fenceManager->ApplyDisplayTopology(topology);
            if (fenceTopologyChanged) fenceManager->SaveConfig();
            ++topologyGeneration;
            currentSnapLines.clear();
            displayTopologySnapshot = topology;
            displayRecoveryPending = false;
            displayTopologyDirty = false;
            displayTopologyRefreshPending = true;
            RestoreDesktopIconsAfterTopology(topology);
            UpdateBlackHoleBackgroundSize();
            RequestRedraw();
            return;
        }

        // No explicit display recovery is pending. This is an unrelated
        // settings/layout notification, so do not repaint or invalidate the
        // interactive frame.
        displayTopologyDirty = false;
        displayTopologyRefreshPending = false;
        topologyReady = backbufferHasCompleteFrame &&
            !forceFullRedraw &&
            completeFrameGeneration == backbufferGeneration;
        return;
    }

    // A Type-C reconnect commonly reports several intermediate layouts
    // (offline, fallback mode, final mode).  Do not commit the first changed
    // sample. Require the same candidate topology three times; the timer is
    // re-armed after every sample, so a new notification naturally postpones
    // the next sample and acts as a quiet-period debounce.
    if (topologyCandidate.signature != topology.signature) {
        topologyCandidate = topology;
        topologyCandidateSamples = 1;
    } else {
        ++topologyCandidateSamples;
    }
    if (topologyCandidateSamples < DISPLAY_TOPOLOGY_STABLE_SAMPLES) {
        ArmDisplayTopologyRefreshTimer();
        return;
    }

    // If a real topology change arrives while the user is moving/resizing a
    // fence or dragging an item, defer the layout transaction until the
    // pointer interaction has finished.  This preserves the in-progress
    // visual state and lets WM_CAPTURECHANGED handle a genuine capture loss
    // (for example, a display being physically removed).
    const RECT virtualScreen = topology.virtualScreen;
    const int newWidth = virtualScreen.right - virtualScreen.left;
    const int newHeight = virtualScreen.bottom - virtualScreen.top;
    if (newWidth <= 0 || newHeight <= 0) {
        ArmDisplayTopologyRefreshTimer();
        return;
    }

    // Mark the short, synchronous layout transaction as dirty only after the
    // topology is proven to differ.  This keeps ordinary duplicate broadcasts
    // from blocking input or cancelling a drag.
    displayTopologyDirty = true;
    // Force a fresh D2D target even when the virtual desktop keeps the same
    // size: the display path/DWM surface may have been recreated underneath.
    ResetRenderResources();
    if (!EnsureBackbuffer(newWidth, newHeight)) {
        displayTopologyDirty = false;
        ArmDisplayTopologyRefreshTimer();
        return;
    }
    if (desktopHostWindow && !IsWindow(desktopHostWindow)) {
        AttachToDesktopHost(virtualScreen);
    }
    if (!PositionForVirtualScreen(virtualScreen)) {
        displayTopologyDirty = false;
        ArmDisplayTopologyRefreshTimer();
        return;
    }
    // Even when the virtual rectangle keeps the same size, monitor work areas
    // or DPI may have changed. The existing local DIB is not a valid complete
    // frame for the new topology.
    forceFullRedraw = true;
    backbufferHasCompleteFrame = false;
    completeFrameGeneration = 0;
    previousInteractiveDirtyValid = false;
    topologyReady = false;
    // A reconnect can replace the CCD target path while keeping the same
    // physical work area.  FenceManager may recover those fences by geometry
    // and update their persisted monitor identity; save that migration once
    // the topology has been stable so the next reconnect starts with the new
    // identity instead of being treated as an offline fence again.
    const bool fenceTopologyChanged = fenceManager->ApplyDisplayTopology(topology);
    if (fenceTopologyChanged) fenceManager->SaveConfig();
    ++topologyGeneration;
    topologyFramePending = true;
    topologyFrameSerial = topologyRefreshSerial;
    currentSnapLines.clear();
    displayTopologySignature = topology.signature;
    displayTopologySnapshot = topology;
    topologyCandidate = {};
    topologyCandidateSamples = 0;
    displayTopologyDirty = false;
    RestoreDesktopIconsAfterTopology(topology);
    UpdateBlackHoleBackgroundSize();
    RequestRedraw();
}

// ===== 窗口过程 =====

LRESULT CALLBACK DesktopOverlayWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DesktopOverlayWindow* self = nullptr;
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCT*)lp;
        self = (DesktopOverlayWindow*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (DesktopOverlayWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }
    if (self) return self->WndProc(msg, wp, lp);
    return DefWindowProc(hwnd, msg, wp, lp);
}

LRESULT DesktopOverlayWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEACTIVATE:
        // A desktop overlay must never become the foreground application just
        // because a fence or one of its items was clicked.
        return MA_NOACTIVATE;

    case WM_NCHITTEST: {
        POINT pt{};
        if (!TryGetOverlayPointer(hwnd, pt)) return HTTRANSPARENT;
        auto hit = fenceManager->HitTest(pt);

        // The overlay is click-through outside fences, so it will not receive
        // WM_MOUSEMOVE after the pointer reaches the desktop. Cancel pending
        // and visible hints during hit testing before a stale timer can place
        // the previous item name at the desktop cursor position.
        if (!hit.hit) {
            CancelTooltipTimer();
            HideTooltip();
        }

        std::wstring newHover = hit.hit ? hit.fenceId : L"";
        if (newHover != hoverFenceId) {
            hoverFenceId = newHover;
            RequestRedraw();
        }

        if (hit.hit) return HTCLIENT;
        return HTTRANSPARENT;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{};
        if (!TryGetOverlayPointer(hwnd, pt)) return 0;
        // Do not start a new interaction while a display topology is being
        // debounced/applied or while the replacement frame is incomplete.
        // Existing drags continue on their captured snapshot and are
        // reconciled after release; new drags must never mix old and new
        // monitor coordinate spaces.
        if (displayTopologyDirty || displayTopologyRefreshPending ||
            topologyFramePending || !topologyReady ||
            !backbufferHasCompleteFrame ||
            completeFrameGeneration != backbufferGeneration) return 0;

        if (textInputMode != TextInputMode::None) {
            if (IsPointInSearchClearButton(pt)) {
                textInputText.clear();
                textInputCaret = 0;
                if (textInputMode == TextInputMode::Search) {
                    CommitSearchText(L"");
                }
                caretBlinkOn = true;
                RequestRedraw();
                return 0;
            }
            if (!IsPointInTextInput(pt)) {
                ExitTextInputMode(true);
            } else {
                return 0;
            }
        }

        CancelTooltipTimer();
        HideTooltip();

        auto fenceHit = fenceManager->HitTest(pt);
        if (fenceHit.hit && fenceHit.area == FenceHitArea::TitleBar) {
            Fence* fence = fenceManager->GetFenceById(fenceHit.fenceId);
            if (fence) {
                switch (HitTestTitleControl(*fence, pt)) {
                case FenceTitleControl::Collapse: fenceManager->ToggleFenceCollapsed(fence->id); RequestRedraw(); return 0;
                case FenceTitleControl::Lock:
                    fenceManager->ToggleFenceLocked(fence->id);
                    fenceManager->TriggerLockAnimation(fence->id);
                    RequestRedraw();
                    return 0;
                case FenceTitleControl::View: fenceManager->ToggleViewMode(fence->id); RequestRedraw(); return 0;
                case FenceTitleControl::Search:
                    if (fence->searchActive) {
                        fenceManager->ClearFenceSearch(fence->id);
                        RequestRedraw();
                    } else {
                        EnterTextInputMode(TextInputMode::Search, fence->id);
                    }
                    return 0;
                case FenceTitleControl::Menu: ShowFenceMenu(fence->id, pt); return 0;
                default: break;
                }
            }
        }

        if (fenceHit.hit) {
            Fence* fence = fenceManager->GetFenceById(fenceHit.fenceId);
            if (fence && !fence->locked) {
                if (!(fence->searchActive && PtInRect(&GetSearchInputRect(*fence), pt))) {
                    ResizeEdge edge = HitTestResizeEdge(*fence, pt);
                    if (edge != ResizeEdge::None &&
                        HitTestTitleControl(*fence, pt) == FenceTitleControl::None) {
                        StartResize(fenceHit.fenceId, edge, pt);
                        return 0;
                    }
                }
            }
        }

        std::wstring itemFenceId;
        int itemIdx = fenceManager->HitTestItem(pt, itemFenceId);
        if (itemIdx >= 0) {
            BeginPressedItem(itemFenceId, itemIdx, pt);
            return 0;
        }

        auto hit = fenceHit;
        if (!hit.hit) return 0;
        Fence* hitFence = fenceManager->GetFenceById(hit.fenceId);

        // A game card has no visible title bar. Allow dragging its empty
        // card body as a move gesture while keeping OLE file drops targeted
        // at the same content area.
        const bool gameCardMove =
            hit.area == FenceHitArea::Content &&
            hitFence && (hitFence->kind == FenceKind::Game ||
                         hitFence->kind == FenceKind::Clock);
        if (hit.area == FenceHitArea::TitleBar || gameCardMove) {
            Fence* fence = hitFence;
            if (fence && !fence->locked) {
                interactionState = FenceInteractionState::PressedTitleBar;
                moveContext.active = true;
                moveContext.hasMoved = false;
                moveContext.fenceId = hit.fenceId;
                moveContext.startMouse = pt;
                moveContext.startRect = fence->rect;
                interactionTopology = displayTopologySnapshot;
                interactionTopologyGeneration = topologyGeneration;
                if (interactionTopology.signature.empty()) {
                    interactionTopology = MonitorService::CaptureTopology();
                }
                activeFenceId = hit.fenceId;
                fenceManager->BringToFront(hit.fenceId);
                SetCapture(hwnd);
                RequestRedraw();
            }
        } else {
            activeFenceId = hit.fenceId;
            RequestRedraw();
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        POINT pt{};
        if (!TryGetOverlayPointer(hwnd, pt)) return 0;

        std::wstring itemFenceId;
        int itemIdx = fenceManager->HitTestItem(pt, itemFenceId);
        if (itemIdx >= 0) {
            Fence* fence = fenceManager->GetFenceById(itemFenceId);
            if (fence && itemIdx < (int)fence->items.size()) {
                FileService::ShellOpenFile(fence->items[itemIdx].fullPath);
            }
            PlaceInDesktopLayer();
            return 0;
        }

        auto fenceHit = fenceManager->HitTest(pt);
        if (fenceHit.hit && fenceHit.area == FenceHitArea::TitleBar) {
            Fence* fence = fenceManager->GetFenceById(fenceHit.fenceId);
            if (fence && !fence->isMappedFolder &&
                HitTestTitleControl(*fence, pt) == FenceTitleControl::None) {
                EnterTextInputMode(TextInputMode::Rename, fenceHit.fenceId);
                return 0;
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        if (displayTopologyDirty) return 0;
        if (interactionState == FenceInteractionState::DraggingItem ||
            interactionState == FenceInteractionState::PressedItem ||
            interactionState == FenceInteractionState::MovingFence ||
            interactionState == FenceInteractionState::PressedTitleBar ||
            interactionState == FenceInteractionState::ResizingFence) {
            return 0;
        }

        POINT pt{};
        if (!TryGetOverlayPointer(hwnd, pt)) return 0;

        std::wstring itemFenceId;
        if (fenceManager->HitTestItem(pt, itemFenceId) >= 0) {
            ShowItemContextMenu(pt);
        } else {
            auto hit = fenceManager->HitTest(pt);
            if (hit.hit) ShowFenceMenu(hit.fenceId, pt);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt{};
        if (!TryGetOverlayPointer(hwnd, pt)) return 0;
        auto hit = fenceManager->HitTest(pt);
        if (!hit.hit || hit.area != FenceHitArea::Content) return 0;
        Fence* fence = fenceManager->GetFenceById(hit.fenceId);
        if (!fence || fence->collapsed) return 0;

        int wheelDelta = GET_WHEEL_DELTA_WPARAM(wp);
        int steps = wheelDelta / WHEEL_DELTA;
        if (steps == 0 && wheelDelta != 0) steps = wheelDelta > 0 ? 1 : -1;
        const int pageStep = fence->viewMode == FenceViewMode::List
            ? GridLayoutService::LIST_ROW_HEIGHT * 3
            : GridLayoutService::GetIconItemHeight(fence->rect) +
              GridLayoutService::ITEM_SPACING;
        if (fenceManager->ScrollFence(fence->id, -steps * pageStep)) {
            fenceManager->SetItemHover(pt);
            RequestRedraw();
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{};
        if (!TryGetOverlayPointer(hwnd, pt)) return 0;

        if (interactionState == FenceInteractionState::ResizingFence) {
            UpdateResize(pt);
            return 0;
        }

        if (interactionState == FenceInteractionState::PressedItem) {
            UpdatePressedItem(pt);
            return 0;
        }

        if (interactionState == FenceInteractionState::DraggingItem) {
            UpdateDraggingItem(pt);
            return 0;
        }

        if (interactionState == FenceInteractionState::PressedTitleBar) {
            int dx = pt.x - moveContext.startMouse.x;
            int dy = pt.y - moveContext.startMouse.y;
            if (std::abs(dx) >= DRAG_START_THRESHOLD ||
                std::abs(dy) >= DRAG_START_THRESHOLD) {
                interactionState = FenceInteractionState::MovingFence;
                moveContext.hasMoved = true;
            }
        }

        if (interactionState == FenceInteractionState::MovingFence) {
            UpdateMovingFence(pt);
            return 0;
        }

        auto hit = fenceManager->HitTest(pt);
        std::wstring newHover = hit.hit ? hit.fenceId : L"";
        if (newHover != hoverFenceId) {
            hoverFenceId = newHover;
            RequestRedraw();
        }

        std::wstring itemFenceId;
        int itemIdx = fenceManager->HitTestItem(pt, itemFenceId);
        if (itemIdx >= 0) {
            fenceManager->SetItemHover(pt);
            RequestRedraw();

            if (!tooltipTimerActive || tooltipFenceId != itemFenceId || tooltipItemIndex != itemIdx) {
                StartTooltipTimer(itemFenceId, itemIdx);
            }
        } else {
            bool hadHover = false;
            for (const auto& f : fenceManager->GetAllFencesRef()) {
                for (const auto& item : f.items) {
                    if (item.hovered) { hadHover = true; break; }
                }
                if (hadHover) break;
            }
            if (hadHover) {
                fenceManager->SetItemHover(pt);
                RequestRedraw();
            }
            CancelTooltipTimer();
            HideTooltip();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (displayTopologyDirty) {
            CancelInteractionsForTopology();
            return 0;
        }
        POINT pt{};
        if (!TryGetOverlayPointer(hwnd, pt)) {
            CancelMove();
            CancelItemDrag();
            if (interactionState == FenceInteractionState::ResizingFence) EndResize();
            return 0;
        }

        if (interactionState == FenceInteractionState::ResizingFence) {
            EndResize();
            return 0;
        }

        if (interactionState == FenceInteractionState::DraggingItem) {
            EndDraggingItem(pt);
            return 0;
        }

        if (interactionState == FenceInteractionState::PressedItem) {
            EndPressedItemWithoutDrag(pt);
            return 0;
        }

        if (interactionState == FenceInteractionState::MovingFence ||
            interactionState == FenceInteractionState::PressedTitleBar) {

            bool shouldSave = moveContext.hasMoved;
            const std::wstring movedFenceId = moveContext.fenceId;
            const auto placementTopology = interactionTopology;
            const auto* topology = placementTopology.signature.empty()
                ? nullptr : &placementTopology;

            currentSnapLines.clear();
            interactionState = FenceInteractionState::None;
            moveContext = {};
            interactionTopology = {};
            interactionTopologyGeneration = 0;

            if (GetCapture() == hwnd) ReleaseCapture();

            if (shouldSave) {
                fenceManager->LayoutFenceItems(movedFenceId);
                fenceManager->CommitFencePlacement(movedFenceId, topology);
                fenceManager->SaveConfig();
            }

            RequestRedraw();
        }
        return 0;
    }

    case WM_CHAR: {
        if (textInputMode != TextInputMode::None) {
            HandleCharInput((wchar_t)wp);
            return 0;
        }
        break;
    }

    case WM_KEYDOWN: {
        if (textInputMode != TextInputMode::None) {
            HandleKeyDownInput(wp);
            return 0;
        }
        break;
    }

    case WM_DROPFILES: {
        OnDropFiles(reinterpret_cast<HDROP>(wp));
        return 0;
    }

    case WM_COPYDATA: {
        auto* data = reinterpret_cast<COPYDATASTRUCT*>(lp);
        if (!data || data->dwData != 1 || !data->lpData || data->cbData < sizeof(wchar_t)) {
            return FALSE;
        }
        const auto* command = static_cast<const wchar_t*>(data->lpData);
        if (_wcsicmp(command, AppCommand::Show) == 0) {
            PostMessageW(hwnd, WM_DESKTODO_ASYNC_SHOW, 0, 0);
            return TRUE;
        }
        if (_wcsicmp(command, AppCommand::Settings) == 0) {
            PostMessageW(hwnd, WM_DESKTODO_ASYNC_SETTINGS, 0, 0);
            return TRUE;
        }
        ExecuteCommand(command);
        return TRUE;
    }

    case WM_DESKTODO_ASYNC_SHOW:
        ExecuteCommand(AppCommand::Show);
        return 0;

    case WM_DESKTODO_ASYNC_SETTINGS:
        ExecuteCommand(AppCommand::Settings);
        return 0;

    case WM_DESKTODO_DESKTOP_VIEW_EVENT:
        PlaceInDesktopLayer();
        if (interactionState ==
                FenceInteractionState::None &&
            !displayTopologyDirty) {
            DesktopShellService::
                ReconcileSuppressedDesktopPaths();
            EnsureDesktopViewEventHook();
        } else {
            ScheduleDesktopReconcile();
        }
        return 0;

    case WM_TIMER: {
        if (wp == TOOLTIP_TIMER_ID) {
            const std::wstring pendingFenceId = tooltipFenceId;
            const int pendingItemIndex = tooltipItemIndex;
            CancelTooltipTimer();
            if (pendingItemIndex >= 0 && !pendingFenceId.empty()) {
                ShowItemTooltip(pendingFenceId, pendingItemIndex);
            }
        } else if (wp == TOOLTIP_GUARD_TIMER_ID) {
            if (!tooltipService || !tooltipService->IsVisible()) {
                HideTooltip();
            } else {
                POINT pt{};
                std::wstring currentFenceId;
                const int currentItemIndex = GetPhysicalCursorPos(&pt)
                    ? fenceManager->HitTestItem(pt, currentFenceId)
                    : -1;
                if (currentItemIndex != visibleTooltipItemIndex ||
                    currentFenceId != visibleTooltipFenceId) {
                    HideTooltip();
                }
            }
        } else if (wp == CARET_TIMER_ID) {
            UpdateCaretBlink();
        } else if (wp == ANIM_TIMER_ID) {
            if (!HasActiveAnimation()) {
                KillTimer(hwnd, ANIM_TIMER_ID);
            }
            RequestRedraw();
        } else if (wp == INTERACTIVE_REDRAW_TIMER_ID) {
            KillTimer(hwnd, INTERACTIVE_REDRAW_TIMER_ID);
            interactiveRedrawTimerArmed = false;
            if (interactiveRedrawPending) {
                interactiveRedrawPending = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (wp == DESKTOP_RECONCILE_TIMER_ID) {
            KillTimer(hwnd, DESKTOP_RECONCILE_TIMER_ID);
            if (interactionState == FenceInteractionState::None &&
                !displayTopologyDirty) {
                DesktopShellService::ReconcileSuppressedDesktopPaths();
                EnsureDesktopViewEventHook();
            } else {
                SetTimer(
                    hwnd, DESKTOP_RECONCILE_TIMER_ID, 50, nullptr);
            }
        } else if (wp == LOGICAL_ICON_SYNC_TIMER_ID) {
            // Shell enumeration and icon snapshots can block briefly. Never
            // run them in the middle of a pointer-driven move/resize/drag.
            PlaceInDesktopLayer();
            if (interactionState == FenceInteractionState::None &&
                !displayTopologyDirty) {
                EnsureDesktopViewEventHook();
                DesktopShellService::ReconcileSuppressedDesktopPaths();
                const ULONGLONG now = GetTickCount64();
                if (now - lastDesktopLayoutSyncTick >=
                    DESKTOP_LAYOUT_SYNC_MS) {
                    lastDesktopLayoutSyncTick = now;
                    const auto topology = MonitorService::CaptureTopology();
                    if (topology.signature.empty() ||
                        topology.signature != displayTopologySignature) {
                        ScheduleDisplayTopologyRefresh();
                    } else {
                        SyncStableDesktopIconLayout(topology);
                    }
                }
            }
        } else if (wp == SHOW_INSTANCE_TIMER_ID) {
            if (showRunningInstanceEvent &&
                WaitForSingleObject(showRunningInstanceEvent, 0) ==
                WAIT_OBJECT_0) {
                ExecuteCommand(AppCommand::Show);
            }
        } else if (wp == CLOCK_TIMER_ID) {
            for (const auto& fence : fenceManager->GetAllFencesRef()) {
                if (fence.kind == FenceKind::Clock) {
                    RequestRedraw();
                    break;
                }
            }
        } else if (wp == DISPLAY_TOPOLOGY_TIMER_ID) {
            RefreshDisplayTopology();
        } else if (wp == CAPTURE_START_TIMER_ID) {
            KillTimer(hwnd, CAPTURE_START_TIMER_ID);
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            if (screenshotTool) screenshotTool->StartCapture();
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            if (!TryGetOverlayPointer(hwnd, pt)) return FALSE;
            auto hit = fenceManager->HitTest(pt);
            if (hit.hit) {
                Fence* f = fenceManager->GetFenceById(hit.fenceId);
                if (f) {
                    if (!f->locked && HitTestTitleControl(*f, pt) == FenceTitleControl::None) {
                        ResizeEdge edge = HitTestResizeEdge(*f, pt);
                        if (edge != ResizeEdge::None) {
                            bool hasH = HasResizeEdge(edge, ResizeEdge::Left) || HasResizeEdge(edge, ResizeEdge::Right);
                            bool hasV = HasResizeEdge(edge, ResizeEdge::Top) || HasResizeEdge(edge, ResizeEdge::Bottom);
                            if (hasH && hasV) {
                                if ((HasResizeEdge(edge, ResizeEdge::Left) && HasResizeEdge(edge, ResizeEdge::Top)) ||
                                    (HasResizeEdge(edge, ResizeEdge::Right) && HasResizeEdge(edge, ResizeEdge::Bottom))) {
                                    SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
                                } else {
                                    SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
                                }
                            } else if (hasH) {
                                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                            } else if (hasV) {
                                SetCursor(LoadCursor(nullptr, IDC_SIZENS));
                            }
                            return TRUE;
                        }
                    }
                    if ((hit.area == FenceHitArea::TitleBar ||
                         (hit.area == FenceHitArea::Content &&
                         (f->kind == FenceKind::Game ||
                          f->kind == FenceKind::Clock))) && !f->locked) {
                        if (HitTestTitleControl(*f, pt) == FenceTitleControl::None) {
                            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                            return TRUE;
                        }
                    }
                }
            }
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    }
    case WM_CAPTURECHANGED:
        // A display reconnect can transiently revoke mouse capture.  If the
        // button is still down, keep the in-progress placement alive and
        // reacquire capture instead of snapping the fence back to its start
        // rectangle.  A real button release still follows the normal cancel
        // path below.
        if (displayRecoveryPending && displayTopologyRefreshPending &&
            (moveContext.active || resizeContext.active ||
             interactionState == FenceInteractionState::PressedTitleBar ||
             interactionState == FenceInteractionState::MovingFence ||
             interactionState == FenceInteractionState::ResizingFence) &&
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
            SetCapture(hwnd);
            return 0;
        }
        CancelMove();
        CancelItemDrag();
        // Capture loss is cancellation, never an affirmative placement commit.
        // In particular, display removal often causes this before the debounced
        // WM_DISPLAYCHANGE reconciliation has completed.
        if (interactionState == FenceInteractionState::ResizingFence) {
            CancelResize();
        }
        return 0;

    case WM_CANCELMODE:
        CancelMove();
        CancelItemDrag();
        // Capture loss is cancellation, never an affirmative placement commit.
        // In particular, display removal often causes this before the debounced
        // WM_DISPLAYCHANGE reconciliation has completed.
        if (interactionState == FenceInteractionState::ResizingFence) {
            CancelResize();
        }
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        ScheduleDisplayTopologyRefresh(true);
        return 0;

    case WM_DEVICECHANGE:
        switch (wp) {
        case DBT_DEVNODES_CHANGED:
        case DBT_CONFIGCHANGED:
        case DBT_DEVICEARRIVAL:
        case DBT_DEVICEREMOVECOMPLETE:
            ScheduleDisplayTopologyRefresh(true);
            break;
        default:
            break;
        }
        return TRUE;

    case WM_SETTINGCHANGE:
        // Keep this as a cheap topology hint.  RefreshDisplayTopology() will
        // discard it without painting when the monitor signature is unchanged.
        ScheduleDisplayTopologyRefresh();
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC ||
            wp == PBT_APMRESUMESUSPEND ||
            wp == PBT_APMRESUMECRITICAL) {
            ScheduleDisplayTopologyRefresh(true);
        }
        return TRUE;

    case WM_TRAY_ICON:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP ||
            lp == WM_CONTEXTMENU || lp == WM_LBUTTONDBLCLK) {
            ShowTrayMenu();
        }
        return 0;

    case WM_DESKTODO_SHOW_POMODORO:
        if (pomodoroWidget) pomodoroWidget->ShowOrCreate();
        return 0;

    case WM_DESKTODO_SHOW_TODO:
        if (todoWidget) todoWidget->ShowOrCreate();
        return 0;

    case WM_DESKTODO_NEW_GAME_FENCE:
        ExecuteCommand(AppCommand::NewGameFence);
        return 0;

    case WM_DESKTODO_NEW_CLOCK_FENCE:
        ExecuteCommand(AppCommand::NewClockFence);
        return 0;

    case WM_DESKTODO_TOGGLE_BLACK_HOLE_BG:
        ToggleBlackHoleBackground();
        return 0;
    case WM_DESKTODO_APPLY_BLACK_HOLE_SETTINGS:
        SetBlackHoleBackgroundEnabled(false);
        SetBlackHoleBackgroundEnabled(true);
        return 0;

    case WM_DESKTODO_START_CAPTURE:
        StartScreenshotCaptureSoon();
        return 0;

    case WM_DESKTODO_RELOAD_SCREENSHOT_HOTKEY:
        return ReloadScreenshotHotkey() ? 1 : 0;

    case WM_DESKTODO_SHOW_LAN_TRANSFER:
        if (lanTransferWidget) lanTransferWidget->ShowOrCreate();
        return 0;

    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case 1001: RequestExit(); break;
        case 3001: fenceManager->RefreshAllFenceItems(); RequestRedraw(); break;
        case 3002: ExecuteCommand(AppCommand::NewFence); break;
        case 3013: ExecuteCommand(AppCommand::NewGameFence); break;
        case 3003: ExecuteCommand(AppCommand::AutoOrganize); break;
        case 3004: ExecuteCommand(AppCommand::ToggleVisibility); break;
        case 3005: ExecuteCommand(AppCommand::ToggleLock); break;
        case 3006: ExecuteCommand(AppCommand::Settings); break;
        case 3010: ExecuteCommand(AppCommand::Screenshot); break;
        case 3012: ExecuteCommand(AppCommand::LanTransfer); break;
        case 3008: ExecuteCommand(AppCommand::BackupLayout); break;
        case 3009: ExecuteCommand(AppCommand::RestoreLayout); break;
        case 3011: ExecuteCommand(AppCommand::ToggleBlackHoleBg); break;
        case 3007: RequestExit(); break;
        }
        return 0;
    }

    case WM_HOTKEY:
        if (wp == 1001) RequestExit();
        if (wp == 1002) {
            fenceManager->RefreshAllFenceItems();
            RequestRedraw();
        }
        if (wp == 1003) ExecuteCommand(AppCommand::NewFence);
        if (wp == 1004) ExecuteCommand(AppCommand::ToggleLock);
        if (wp == 1005) StartScreenshotCaptureSoon();
        return 0;

    case WM_CLOSE:
        RequestExit();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
