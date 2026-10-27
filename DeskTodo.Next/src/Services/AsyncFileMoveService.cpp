#include "AsyncFileMoveService.h"
#include <objbase.h>
#include <chrono>

bool AsyncFileMoveService::Start(AsyncFileMoveRequest request) {
    if (IsBusy() || request.sourcePath.empty() ||
        request.targetDirectory.empty()) return false;
    try {
        operation_ = std::async(std::launch::async,
            [request = std::move(request)] {
                const HRESULT initialized = CoInitializeEx(
                    nullptr, COINIT_MULTITHREADED);
                AsyncFileMoveOutcome outcome;
                FileMoveService service;
                outcome.result = service.MoveToDirectory(
                    request.sourcePath, request.targetDirectory,
                    request.overwrite, &outcome.movedPath,
                    request.reserveDesktopName);
                if (SUCCEEDED(initialized)) CoUninitialize();
                return outcome;
            });
        return true;
    } catch (...) {
        return false;
    }
}

bool AsyncFileMoveService::TryTakeResult(AsyncFileMoveOutcome& outcome) {
    if (!operation_.valid() || operation_.wait_for(
            std::chrono::milliseconds(0)) != std::future_status::ready) {
        return false;
    }
    try {
        outcome = operation_.get();
    } catch (...) {
        outcome = {};
    }
    return true;
}

void AsyncFileMoveService::Wait() {
    if (operation_.valid()) operation_.wait();
}
