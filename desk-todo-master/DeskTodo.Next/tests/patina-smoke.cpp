#include "Services/PatinaTrackerService.h"

#include <Windows.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <algorithm>

int wmain() {
    int failures = 0;
    auto check = [&](bool condition, const wchar_t* name) {
        if (!condition) {
            ++failures;
            std::wcerr << L"FAIL: " << name << L"\n";
        }
    };

    check(PatinaTrackerService::CategoryForProcess(L"chrome.exe") == L"浏览",
          L"browser category");
    check(PatinaTrackerService::CategoryForProcess(L"Code.exe") == L"开发",
          L"developer category");
    check(PatinaTrackerService::CategoryForProcess(L"Obsidian.exe") == L"文档",
          L"document category");
    check(PatinaTrackerService::FormatDuration(65) == L"1分 5秒",
          L"minute formatting");
    check(PatinaTrackerService::FormatDuration(3661) == L"1小时 1分",
          L"hour formatting");
    check(PatinaTrackerService::ColorForCategory(L"浏览") !=
          PatinaTrackerService::ColorForCategory(L"开发"),
          L"category colors differ");

    {
        PatinaTrackerService tracker;
        const std::wstring smokeProcess = L"DeskTodoPatinaSmoke.exe";
        tracker.ClearAppRecords(smokeProcess);
        tracker.AddManualSampleForTest(smokeProcess, L"Patina smoke", 5);
        auto snapshot = tracker.Snapshot();
        auto findSmoke = [&](const PatinaSnapshot& value) {
            return std::find_if(value.apps.begin(), value.apps.end(), [&](const auto& app) {
                return _wcsicmp(app.processName.c_str(), smokeProcess.c_str()) == 0;
            });
        };
        auto smoke = findSmoke(snapshot);
        check(smoke != snapshot.apps.end() && smoke->seconds >= 5, L"manual tracking sample");
        const long long totalBeforeExclude = snapshot.totalSeconds;
        const long long smokeSeconds = smoke == snapshot.apps.end() ? 0 : smoke->seconds;
        tracker.SetAppCategory(smokeProcess, L"工具");
        snapshot = tracker.Snapshot();
        smoke = findSmoke(snapshot);
        check(smoke != snapshot.apps.end() && smoke->category == L"工具", L"category edit persists in memory");
        tracker.SetAppTitleRecording(smokeProcess, false);
        snapshot = tracker.Snapshot();
        smoke = findSmoke(snapshot);
        check(smoke != snapshot.apps.end() && !smoke->titleRecording, L"per-app title preference persists");
        tracker.SetAppExcluded(smokeProcess, true);
        check(tracker.Snapshot().totalSeconds == totalBeforeExclude - smokeSeconds,
              L"excluded app omitted from effective total");
        check(tracker.DailyTotals(7).size() == 7, L"daily totals range");
        const int originalIdle = tracker.IdleTimeoutMinutes();
        tracker.SetIdleTimeoutMinutes(4);
        check(tracker.IdleTimeoutMinutes() == 4, L"idle timeout setting");
        tracker.SetIdleTimeoutMinutes(originalIdle);
        tracker.ClearAppRecords(smokeProcess);
        tracker.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(16000));
        tracker.Stop();
        tracker.ClearAppRecords(smokeProcess);
    }

    if (failures == 0) {
        std::wcout << L"Patina smoke checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
