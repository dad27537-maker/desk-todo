#pragma once

#include "FileMoveService.h"
#include <future>

struct AsyncFileMoveRequest {
    std::wstring sourcePath;
    std::wstring targetDirectory;
    bool overwrite = false;
    bool reserveDesktopName = false;
};

struct AsyncFileMoveOutcome {
    FileMoveResult result = FileMoveResult::Failed;
    std::wstring movedPath;
};

// Owns one filesystem operation. Only copied paths/options cross the worker
// boundary; Shell view COM, fences, HWNDs and D2D objects stay on the UI thread.
class AsyncFileMoveService {
public:
    bool Start(AsyncFileMoveRequest request);
    bool TryTakeResult(AsyncFileMoveOutcome& outcome);
    bool IsBusy() const { return operation_.valid(); }
    // Used only during final teardown. Normal operation polls without waiting.
    void Wait();

private:
    std::future<AsyncFileMoveOutcome> operation_;
};
