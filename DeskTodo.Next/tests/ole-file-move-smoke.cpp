#include "Services/OleFileMoveCompletion.h"
#include <iostream>

namespace {
int checks = 0;
int failures = 0;
void Check(bool passed, const char* message) {
    ++checks;
    if (!passed) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

class Source final : public IDataObject, public IDataObjectAsyncCapability {
public:
    bool supportsAsync = true;
    bool asyncMode = true;
    bool startFails = false;
    ULONG references = 1;
    int starts = 0;
    int ends = 0;
    int feedbacks = 0;
    DWORD feedback = DROPEFFECT_MOVE;
    DWORD finalEffect = DROPEFFECT_MOVE;
    HRESULT finalResult = E_PENDING;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDataObject))
            *out = static_cast<IDataObject*>(this);
        else if (supportsAsync && iid == __uuidof(IDataObjectAsyncCapability))
            *out = static_cast<IDataObjectAsyncCapability*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (format->cfFormat != RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT) ||
            medium->tymed != TYMED_HGLOBAL) return E_INVALIDARG;
        auto* value = static_cast<DWORD*>(GlobalLock(medium->hGlobal));
        if (!value) return E_FAIL;
        feedback = *value;
        ++feedbacks;
        GlobalUnlock(medium->hGlobal);
        if (release) ReleaseStgMedium(medium);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL value) override { asyncMode = value != FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* out) override { *out = asyncMode; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { ++starts; return startFails ? E_FAIL : S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* out) override { *out = starts > ends; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx*, DWORD effect) override {
        ++ends; finalResult = result; finalEffect = effect; return S_OK;
    }
};

OleFileMoveCompletion* waiting = nullptr;
int timerDispatches = 0;
void CALLBACK CompleteFromTimer(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    ++timerDispatches;
    if (waiting) waiting->CompleteOne(true);
}
}

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        Source source;
        auto batch = OleFileMoveCompletion::Begin(&source, 2);
        Check(source.starts == 1 && source.references == 3, "async batch starts and retains both interfaces");
        batch->CompleteOne(true);
        Check(!batch->finished && source.ends == 0 && source.feedbacks == 0,
              "first file never prematurely completes the whole batch");
        batch->CompleteOne(true);
        Check(batch->finished && batch->succeeded && source.ends == 1 && source.finalResult == S_OK,
              "all files complete async batch successfully");
        Check(source.feedback == DROPEFFECT_NONE && source.finalEffect == DROPEFFECT_NONE,
              "optimized move never requests source deletion");
        batch->CompleteOne(false);
        Check(source.ends == 1 && source.feedbacks == 1, "completion is idempotent");
        batch.reset();
        Check(source.references == 1, "batch releases retained source references");
    }
    {
        Source source;
        auto batch = OleFileMoveCompletion::Begin(&source, 3);
        batch->CompleteOne(true);
        batch->CompleteOne(false);
        Check(!batch->finished, "failed member still waits for remaining tasks");
        batch->CompleteOne(true);
        Check(!batch->succeeded && source.finalResult == E_ABORT && source.feedback == DROPEFFECT_NONE,
              "partial failure cannot delete any original source item");
    }
    {
        Source source;
        { auto batch = OleFileMoveCompletion::Begin(&source, 2); }
        Check(source.ends == 1 && source.finalResult == E_ABORT && source.references == 1,
              "destruction cancels unfinished batch and releases COM references");
    }
    {
        Source source;
        source.supportsAsync = false;
        auto batch = OleFileMoveCompletion::Begin(&source, 1);
        Check(!batch->async && source.references == 2, "legacy source retains only IDataObject");
        waiting = batch.get();
        const UINT_PTR timer = SetTimer(nullptr, 0, 30, CompleteFromTimer);
        Check(timer != 0, "completion timer created");
        if (timer) {
            Check(batch->WaitWithMessagePump() && timerDispatches == 1,
                  "legacy wait dispatches UI timer and waits for actual completion");
        } else batch->Finish(false);
        waiting = nullptr;
        Check(source.starts == 0 && source.ends == 0 && source.feedback == DROPEFFECT_NONE,
              "legacy source gets final feedback without async calls");
    }
    {
        Source source;
        source.asyncMode = false;
        auto batch = OleFileMoveCompletion::Begin(&source, 1);
        Check(!batch->async && source.starts == 0 && source.references == 2,
              "source disabling async uses synchronous completion lifetime");
        batch->CompleteOne(true);
    }
    {
        Source source;
        source.startFails = true;
        auto batch = OleFileMoveCompletion::Begin(&source, 1);
        Check(!batch->async && source.references == 2, "failed StartOperation falls back to responsive synchronous wait");
        batch->CompleteOne(true);
        Check(source.ends == 0 && batch->succeeded, "failed async negotiation never gets EndOperation");
    }
    {
        Source source;
        source.supportsAsync = false;
        auto batch = OleFileMoveCompletion::Begin(&source, 1);
        PostQuitMessage(37);
        Check(!batch->WaitWithMessagePump() && batch->finished,
              "WM_QUIT cancels the wait without hanging");
        MSG quit{};
        Check(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) &&
              quit.message == WM_QUIT && quit.wParam == 37,
              "WM_QUIT is reposted with original exit code");
        Check(source.feedback == DROPEFFECT_NONE, "cancelled legacy wait cannot delete source files");
    }
    CoUninitialize();
    std::cout << "OLE file move smoke: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
