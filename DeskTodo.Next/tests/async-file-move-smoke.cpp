#include <Windows.h>
#include "Services/AsyncFileMoveService.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool passed, const char* name) {
        ++checks;
        if (!passed) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    const fs::path root = fs::current_path() / "async-file-move-smoke-data" /
        std::to_wstring(GetCurrentProcessId());
    fs::create_directories(root / "source" / "tree" / "nested");
    fs::create_directories(root / "target");
    std::ofstream(root / "source" / "tree" / "nested" / "payload.txt") << "preserved";
    AsyncFileMoveService worker;
    check(!worker.IsBusy(), "new worker is idle");
    AsyncFileMoveOutcome outcome;
    check(!worker.TryTakeResult(outcome), "idle polling returns immediately");
    check(worker.Start({ (root / "source" / "tree").wstring(),
                         (root / "target").wstring() }), "folder move starts");
    check(worker.IsBusy(), "submitted result is owned until collected");
    check(!worker.Start({ (root / "source").wstring(), root.wstring() }),
          "second operation cannot replace an uncollected result");
    const ULONGLONG deadline = GetTickCount64() + 20000;
    bool finished = false;
    // Poll with a real Windows message pump; the operation never needs this
    // thread to execute its filesystem work or send a synchronous UI message.
    while (!(finished = worker.TryTakeResult(outcome)) && GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
    check(finished && outcome.result == FileMoveResult::Success, "folder move completes");
    check(fs::exists(root / "target" / "tree" / "nested" / "payload.txt") &&
          !fs::exists(root / "source" / "tree"), "tree moves with its contents");

    fs::create_directories(root / "source" / "tree");
    std::ofstream(root / "source" / "tree" / "new.txt") << "replacement";
    check(worker.Start({ (root / "source" / "tree").wstring(),
                         (root / "target").wstring() }), "conflict check starts");
    worker.Wait();
    check(worker.TryTakeResult(outcome) && outcome.result == FileMoveResult::Conflict,
          "conflict is reported without overwrite");
    check(fs::exists(root / "source" / "tree" / "new.txt") &&
          fs::exists(root / "target" / "tree" / "nested" / "payload.txt"),
          "conflict preserves source and destination");
    check(worker.Start({ (root / "source" / "tree").wstring(),
                         (root / "target").wstring(), true }), "approved overwrite starts");
    worker.Wait();
    check(worker.TryTakeResult(outcome) && outcome.result == FileMoveResult::Success &&
          fs::exists(root / "target" / "tree" / "new.txt"), "approved overwrite succeeds");

    check(worker.Start({ (root / "target" / "tree").wstring(),
                         (root / "target" / "tree" / "child").wstring() }),
          "self-descendant request can be inspected on worker");
    worker.Wait();
    check(worker.TryTakeResult(outcome) && outcome.result == FileMoveResult::Failed &&
          !fs::exists(root / "target" / "tree" / "child"),
          "self-descendant is rejected before creating directories");
    check(worker.Start({ (root / "target" / "tree").wstring(),
                         (root / "target" / "tree").wstring() }),
          "self-directory request starts");
    worker.Wait();
    check(worker.TryTakeResult(outcome) && outcome.result == FileMoveResult::Failed &&
          fs::exists(root / "target" / "tree" / "new.txt"),
          "self-directory is rejected without changing contents");
    check(worker.Start({ (root / "target" / "tree").wstring(),
                         (root / "target" / "tree" / ".." / "tree" / "child").wstring() }),
          "normalized descendant request starts");
    worker.Wait();
    check(worker.TryTakeResult(outcome) && outcome.result == FileMoveResult::Failed &&
          !fs::exists(root / "target" / "tree" / "child"),
          "dot-dot normalization cannot bypass descendant protection");
    check(worker.Start({ (root / "missing").wstring(), (root / "target").wstring() }),
          "missing source can be inspected");
    worker.Wait();
    check(worker.TryTakeResult(outcome) && outcome.result == FileMoveResult::Failed,
          "worker failure is delivered without an exception");
    check(!worker.IsBusy(), "worker returns idle after failed operation");

    fs::create_directories(root / "source" / "locked-tree" / "nested");
    std::ofstream(root / "source" / "locked-tree" / "nested" / "payload.txt") << "complete-copy";
    HANDLE directoryLock = CreateFileW((root / "source" / "locked-tree").c_str(),
        FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    check(directoryLock != INVALID_HANDLE_VALUE, "source directory can be locked against deletion");
    check(worker.Start({ (root / "source" / "locked-tree").wstring(),
                         (root / "target").wstring() }), "locked directory move starts");
    worker.Wait();
    check(worker.TryTakeResult(outcome) &&
          outcome.result == FileMoveResult::CopyCompletedSourceRetained,
          "incomplete source cleanup reports retained destination copy");
    check(fs::exists(root / "target" / "locked-tree" / "nested" / "payload.txt"),
          "source cleanup failure never deletes the complete destination");
    if (directoryLock != INVALID_HANDLE_VALUE) CloseHandle(directoryLock);

    fs::create_directories(root / "source" / "overwrite-tree");
    fs::create_directories(root / "target" / "overwrite-tree");
    std::ofstream(root / "source" / "overwrite-tree" / "new.txt") << "new";
    std::ofstream(root / "target" / "overwrite-tree" / "old.txt") << "old";
    directoryLock = CreateFileW((root / "source" / "overwrite-tree").c_str(),
        FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    check(directoryLock != INVALID_HANDLE_VALUE, "overwrite source directory is locked");
    check(worker.Start({ (root / "source" / "overwrite-tree").wstring(),
                         (root / "target").wstring(), true }), "locked overwrite starts");
    worker.Wait();
    check(worker.TryTakeResult(outcome) &&
          outcome.result == FileMoveResult::CopyCompletedSourceRetained &&
          fs::exists(root / "target" / "overwrite-tree" / "new.txt"),
          "failed source cleanup does not roll back the complete replacement");
    bool backupKept = false;
    for (const auto& entry : fs::directory_iterator(root / "target")) {
        if (entry.path().filename().wstring().find(L"overwrite-tree.desktodo-backup-") == 0 &&
            fs::exists(entry.path() / "old.txt")) backupKept = true;
    }
    check(backupKept, "old overwritten destination backup is preserved on partial cleanup");
    if (directoryLock != INVALID_HANDLE_VALUE) CloseHandle(directoryLock);
    std::error_code error;
    fs::remove_all(root, error);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
