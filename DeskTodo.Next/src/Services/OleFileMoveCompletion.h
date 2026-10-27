#pragma once
#include <Windows.h>
#include <shlobj.h>
#include <shldisp.h>
#include <memory>

// UI-apartment-only completion state. Workers use copied filesystem paths;
// they never access the source data object or its COM interfaces.
struct OleFileMoveCompletion {
    IDataObject* dataObject = nullptr;
    IDataObjectAsyncCapability* async = nullptr;
    size_t remaining = 0;
    bool failed = false;
    bool finished = false;
    bool succeeded = false;

    static void SetPerformedEffect(IDataObject* object) {
        if (!object) return;
        STGMEDIUM medium{};
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = GlobalAlloc(GHND, sizeof(DWORD));
        if (!medium.hGlobal) return;
        auto* value = static_cast<DWORD*>(GlobalLock(medium.hGlobal));
        if (!value) {
            GlobalFree(medium.hGlobal);
            return;
        }
        // The target performed the entire move. Even on partial failure the
        // source must not delete any originals as a second cleanup operation.
        *value = DROPEFFECT_NONE;
        GlobalUnlock(medium.hGlobal);
        FORMATETC format{static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT)),
            nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        if (FAILED(object->SetData(&format, &medium, TRUE))) ReleaseStgMedium(&medium);
    }

    static std::shared_ptr<OleFileMoveCompletion> Begin(
        IDataObject* object, size_t count) {
        if (!object || count == 0) return {};
        auto completion = std::make_shared<OleFileMoveCompletion>();
        if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&completion->async)))) {
            BOOL asyncMode = FALSE;
            if (FAILED(completion->async->GetAsyncMode(&asyncMode)) || !asyncMode ||
                FAILED(completion->async->StartOperation(nullptr))) {
                completion->async->Release();
                completion->async = nullptr;
            }
        }
        completion->dataObject = object;
        object->AddRef();
        completion->remaining = count;
        return completion;
    }

    void Finish(bool success) {
        if (finished) return;
        finished = true;
        succeeded = success;
        SetPerformedEffect(dataObject);
        if (async) async->EndOperation(success ? S_OK : E_ABORT,
                                      nullptr, DROPEFFECT_NONE);
    }

    void CompleteOne(bool success) {
        if (finished || remaining == 0) return;
        failed = failed || !success;
        if (--remaining == 0) Finish(!failed);
    }

    bool WaitWithMessagePump() {
        // A source without asynchronous capability cannot be released until
        // Drop completes. Keep this target responsive while its worker runs.
        bool sawQuit = false;
        int quitCode = 0;
        while (!finished) {
            const DWORD wait = MsgWaitForMultipleObjectsEx(
                0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_FAILED) {
                Finish(false);
                break;
            }
            MSG message{};
            // Bound each dispatch slice so completion is checked even if a
            // producer continuously posts messages to the UI queue.
            for (unsigned dispatched = 0; dispatched < 64 && !finished &&
                 PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++dispatched) {
                if (message.message == WM_QUIT) {
                    sawQuit = true;
                    quitCode = static_cast<int>(message.wParam);
                    Finish(false);
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        // Never swallow shutdown or repeatedly spin on an unremoved WM_QUIT.
        if (sawQuit) PostQuitMessage(quitCode);
        return succeeded;
    }

    ~OleFileMoveCompletion() {
        if (dataObject && !finished) Finish(false);
        if (async) async->Release();
        if (dataObject) dataObject->Release();
    }
};
