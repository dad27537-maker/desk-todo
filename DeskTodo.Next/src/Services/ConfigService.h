#pragma once
#include <vector>
#include <string>
#include "../Models/Fence.h"

struct DesktopPositionRecord {
    std::wstring path;
    POINT position{};
};

enum class ConfigLoadStatus {
    Loaded,
    Missing,
    Corrupt,
    IoError,
};

struct ConfigLoadResult {
    ConfigLoadStatus status = ConfigLoadStatus::Missing;
    std::vector<Fence> fences;
    int version = 0;
    bool loadedFromLastGood = false;
};

class ConfigService {
public:
    ConfigService();
    ConfigLoadResult LoadFencesWithStatus();
    // Compatibility wrapper. Callers that may create defaults should prefer
    // LoadFencesWithStatus so Missing is not confused with Corrupt/IoError.
    std::vector<Fence> LoadFences();
    void SaveFences(const std::vector<Fence>& fences);
    bool BackupLayout(const std::vector<Fence>& fences,
                      const std::vector<DesktopPositionRecord>& desktopItems,
                      const std::wstring& backupPath);
    bool RestoreLayout(const std::wstring& backupPath,
                       std::vector<Fence>& fences,
                       std::vector<DesktopPositionRecord>& desktopItems);
    void EnsureDirectories();
    int GetLoadedVersion() const { return loadedVersion; }
    ConfigLoadStatus GetLastLoadStatus() const { return lastLoadStatus; }
    bool WasLoadedFromLastGood() const { return loadedFromLastGood; }

private:
    int loadedVersion = 0;
    ConfigLoadStatus lastLoadStatus = ConfigLoadStatus::Missing;
    bool loadedFromLastGood = false;
};
