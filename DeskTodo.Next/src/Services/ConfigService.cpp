#include "ConfigService.h"
#include "../Utils/JsonUtils.h"
#include "DesktopPathService.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

namespace {

constexpr char BackupMagic[] = "DESKTODO.BAK.1\n";
std::mutex configIoMutex;
volatile LONG temporaryFileSequence = 0;

std::wstring GetLastGoodConfigPath() {
    return GetConfigPath() + L".last-good";
}

std::wstring MakeTemporaryConfigPath(const std::wstring& targetPath) {
    const LONG sequence = InterlockedIncrement(&temporaryFileSequence);
    return targetPath + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetCurrentThreadId()) +
        L"-" + std::to_wstring(GetTickCount64()) +
        L"-" + std::to_wstring(sequence);
}

bool WriteDurableTemporaryFile(const std::wstring& targetPath,
                               const std::string& contents,
                               std::wstring& temporaryPath) {
    temporaryPath.clear();
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::wstring candidate = MakeTemporaryConfigPath(targetPath);
        HANDLE file = CreateFileW(
            candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return false;
        }

        bool success = true;
        size_t offset = 0;
        while (offset < contents.size()) {
            const size_t remaining = contents.size() - offset;
            const DWORD chunk = static_cast<DWORD>(
                remaining > static_cast<size_t>(MAXDWORD)
                    ? static_cast<size_t>(MAXDWORD) : remaining);
            DWORD written = 0;
            if (!WriteFile(file, contents.data() + offset, chunk, &written, nullptr) ||
                written == 0) {
                success = false;
                break;
            }
            offset += written;
        }
        if (success) success = FlushFileBuffers(file) == TRUE;
        if (!CloseHandle(file)) success = false;

        if (!success) {
            DeleteFileW(candidate.c_str());
            return false;
        }
        temporaryPath = candidate;
        return true;
    }
    return false;
}

bool CommitTemporaryFile(const std::wstring& temporaryPath,
                         const std::wstring& targetPath) {
    // The temporary file is created beside the target, so this is a same-volume
    // atomic rename. WRITE_THROUGH makes the rename durable before returning.
    return MoveFileExW(
        temporaryPath.c_str(), targetPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == TRUE;
}

bool IsMissingFileError(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

ConfigLoadStatus LoadConfigFile(const std::wstring& path,
                                std::vector<Fence>& fences, int& version) {
    fences.clear();
    version = 0;

    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return IsMissingFileError(GetLastError())
            ? ConfigLoadStatus::Missing : ConfigLoadStatus::IoError;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize)) {
        CloseHandle(file);
        return ConfigLoadStatus::IoError;
    }
    if (fileSize.QuadPart <= 0 ||
        fileSize.QuadPart >
            static_cast<LONGLONG>((std::numeric_limits<int>::max)())) {
        CloseHandle(file);
        return ConfigLoadStatus::Corrupt;
    }

    std::string utf8(static_cast<size_t>(fileSize.QuadPart), 0);
    size_t offset = 0;
    while (offset < utf8.size()) {
        const size_t remaining = utf8.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(remaining, static_cast<size_t>(MAXDWORD)));
        DWORD bytesRead = 0;
        if (!ReadFile(file, utf8.data() + offset, chunk, &bytesRead, nullptr) ||
            bytesRead == 0) {
            CloseHandle(file);
            return ConfigLoadStatus::IoError;
        }
        offset += bytesRead;
    }
    if (!CloseHandle(file)) {
        return ConfigLoadStatus::IoError;
    }

    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return ConfigLoadStatus::Corrupt;

    std::wstring decoded(static_cast<size_t>(length), 0);
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), decoded.data(), length) != length) {
        return ConfigLoadStatus::Corrupt;
    }

    std::vector<Fence> parsedFences;
    int parsedVersion = 0;
    if (!JsonDeserialize(decoded, parsedFences, &parsedVersion)) {
        return ConfigLoadStatus::Corrupt;
    }
    fences = std::move(parsedFences);
    version = parsedVersion;
    return ConfigLoadStatus::Loaded;
}

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), 0);
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), 0);
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

template <typename T>
bool WriteValue(std::ofstream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    return stream.good();
}

template <typename T>
bool ReadValue(std::ifstream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), sizeof(T));
    return stream.good();
}

}

ConfigService::ConfigService() {
    EnsureDirectories();
}

ConfigLoadResult ConfigService::LoadFencesWithStatus() {
    std::lock_guard<std::mutex> lock(configIoMutex);
    loadedVersion = 0;
    lastLoadStatus = ConfigLoadStatus::Missing;
    loadedFromLastGood = false;

    ConfigLoadResult result;
    const ConfigLoadStatus primaryStatus = LoadConfigFile(
        GetConfigPath(), result.fences, result.version);
    if (primaryStatus == ConfigLoadStatus::Loaded) {
        result.status = ConfigLoadStatus::Loaded;
        loadedVersion = result.version;
        lastLoadStatus = result.status;
        return result;
    }

    result.fences.clear();
    result.version = 0;
    const ConfigLoadStatus lastGoodStatus = LoadConfigFile(
        GetLastGoodConfigPath(), result.fences, result.version);
    if (lastGoodStatus == ConfigLoadStatus::Loaded) {
        result.status = ConfigLoadStatus::Loaded;
        result.loadedFromLastGood = true;
        loadedVersion = result.version;
        lastLoadStatus = result.status;
        loadedFromLastGood = true;
        return result;
    }

    result.fences.clear();
    result.version = 0;
    // An inaccessible file may become readable again; never collapse that
    // state into Missing and let a caller overwrite it with first-run defaults.
    if (primaryStatus == ConfigLoadStatus::IoError ||
        lastGoodStatus == ConfigLoadStatus::IoError) {
        result.status = ConfigLoadStatus::IoError;
    } else if (primaryStatus == ConfigLoadStatus::Corrupt ||
               lastGoodStatus == ConfigLoadStatus::Corrupt) {
        result.status = ConfigLoadStatus::Corrupt;
    } else {
        result.status = ConfigLoadStatus::Missing;
    }
    lastLoadStatus = result.status;
    return result;
}

std::vector<Fence> ConfigService::LoadFences() {
    ConfigLoadResult result = LoadFencesWithStatus();
    return std::move(result.fences);
}

void ConfigService::SaveFences(const std::vector<Fence>& fences) {
    std::lock_guard<std::mutex> lock(configIoMutex);
    std::wstring json = JsonSerialize(fences);
    if (json.empty() ||
        json.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return;
    }
    const int jsonLength = static_cast<int>(json.size());
    int len = WideCharToMultiByte(CP_UTF8, 0, json.c_str(), jsonLength,
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return;

    std::string utf8((size_t)len, 0);
    if (WideCharToMultiByte(CP_UTF8, 0, json.c_str(), jsonLength,
                            utf8.data(), len, nullptr, nullptr) != len) {
        return;
    }

    const std::wstring configPath = GetConfigPath();
    std::wstring temporaryPath;
    if (!WriteDurableTemporaryFile(configPath, utf8, temporaryPath)) return;
    if (!CommitTemporaryFile(temporaryPath, configPath)) {
        DeleteFileW(temporaryPath.c_str());
        return;
    }

    // last-good is itself replaced atomically. If this secondary update is
    // interrupted, the prior last-good remains intact while config.json has
    // already reached a complete, durable state.
    const std::wstring lastGoodPath = GetLastGoodConfigPath();
    std::wstring lastGoodTemporaryPath;
    if (!WriteDurableTemporaryFile(
            lastGoodPath, utf8, lastGoodTemporaryPath)) {
        return;
    }
    if (!CommitTemporaryFile(lastGoodTemporaryPath, lastGoodPath)) {
        DeleteFileW(lastGoodTemporaryPath.c_str());
    }
}

bool ConfigService::BackupLayout(
    const std::vector<Fence>& fences,
    const std::vector<DesktopPositionRecord>& desktopItems,
    const std::wstring& backupPath) {
    if (backupPath.empty()) return false;
    const std::string config = ToUtf8(JsonSerialize(fences));
    if (config.empty()) return false;

    std::ofstream stream(std::filesystem::path(backupPath), std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) return false;
    stream.write(BackupMagic, sizeof(BackupMagic) - 1);
    const uint32_t configLength = static_cast<uint32_t>(config.size());
    if (!WriteValue(stream, configLength)) return false;
    stream.write(config.data(), config.size());
    const uint32_t itemCount = static_cast<uint32_t>(desktopItems.size());
    if (!WriteValue(stream, itemCount)) return false;
    for (const auto& item : desktopItems) {
        const std::string path = ToUtf8(item.path);
        const uint32_t pathLength = static_cast<uint32_t>(path.size());
        if (!WriteValue(stream, pathLength)) return false;
        stream.write(path.data(), path.size());
        const int32_t x = item.position.x;
        const int32_t y = item.position.y;
        if (!WriteValue(stream, x) || !WriteValue(stream, y)) return false;
    }
    return stream.good();
}

bool ConfigService::RestoreLayout(
    const std::wstring& backupPath,
    std::vector<Fence>& fences,
    std::vector<DesktopPositionRecord>& desktopItems) {
    fences.clear();
    desktopItems.clear();
    std::ifstream stream(std::filesystem::path(backupPath), std::ios::binary);
    if (!stream.is_open()) return false;

    char magic[sizeof(BackupMagic) - 1]{};
    stream.read(magic, sizeof(magic));
    if (!stream.good() || std::memcmp(magic, BackupMagic, sizeof(magic)) != 0) return false;

    uint32_t configLength = 0;
    if (!ReadValue(stream, configLength) || configLength == 0 || configLength > 64 * 1024 * 1024) {
        return false;
    }
    std::string config(configLength, 0);
    stream.read(config.data(), config.size());
    if (!stream.good() || !JsonDeserialize(FromUtf8(config), fences, nullptr) || fences.empty()) {
        return false;
    }

    uint32_t itemCount = 0;
    if (!ReadValue(stream, itemCount) || itemCount > 100000) return false;
    desktopItems.reserve(itemCount);
    for (uint32_t index = 0; index < itemCount; ++index) {
        uint32_t pathLength = 0;
        if (!ReadValue(stream, pathLength) || pathLength > 1024 * 1024) return false;
        std::string path(pathLength, 0);
        stream.read(path.data(), path.size());
        int32_t x = 0;
        int32_t y = 0;
        if (!stream.good() || !ReadValue(stream, x) || !ReadValue(stream, y)) return false;
        desktopItems.push_back({ FromUtf8(path), POINT{ x, y } });
    }
    return true;
}

void ConfigService::EnsureDirectories() {
    EnsureDirectoryExists(GetAppDataPath());
    EnsureDirectoryExists(GetAppDataPath() + L"\\Fences");
}
