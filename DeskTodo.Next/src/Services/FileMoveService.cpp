#include "FileMoveService.h"
#include "DesktopPathService.h"

#include <Windows.h>
#include <ShlObj.h>
#include <filesystem>
#include <fstream>

namespace {

constexpr char ReservationMarker[] = "DeskTodo.NameReservation.v1";
constexpr wchar_t DirectoryMarker[] = L".desktodo-name-reservation";

std::wstring MakeTemporarySibling(const std::wstring& targetPath, const wchar_t* suffix) {
    return targetPath + suffix + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64());
}

}

FileMoveResult FileMoveService::MoveToDirectory(const std::wstring& sourcePath,
                                                const std::wstring& targetDirectory,
                                                bool overwrite,
                                                std::wstring* movedPath,
                                                bool reserveDesktopName) {
    if (movedPath) movedPath->clear();
    try {
        if (sourcePath.empty() || targetDirectory.empty()) return FileMoveResult::Failed;
        if (!std::filesystem::exists(sourcePath)) return FileMoveResult::Failed;

        std::error_code ec;
        std::filesystem::create_directories(targetDirectory, ec);
        if (ec) return FileMoveResult::Failed;

        const std::filesystem::path source(sourcePath);
        if (IsSamePath(source.parent_path().wstring(), targetDirectory)) {
            if (movedPath) *movedPath = sourcePath;
            return FileMoveResult::SameLocation;
        }

        const std::wstring targetPath =
            (std::filesystem::path(targetDirectory) / source.filename()).wstring();
        if (IsSamePath(sourcePath, targetPath)) {
            if (movedPath) *movedPath = sourcePath;
            return FileMoveResult::SameLocation;
        }

        if (std::filesystem::exists(targetPath) &&
            IsSamePath(targetDirectory, GetUserDesktopPath()) &&
            IsNameReservation(targetPath)) {
            RemovePath(targetPath);
        }
        const bool targetExists = std::filesystem::exists(targetPath);
        if (targetExists && !overwrite) return FileMoveResult::Conflict;

        const DWORD sourceAttributes = GetFileAttributesW(sourcePath.c_str());
        const std::wstring backupPath = targetExists
            ? MakeTemporarySibling(targetPath, L".desktodo-backup-")
            : L"";

        if (targetExists) {
            if (!MoveFileExW(targetPath.c_str(), backupPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
                return FileMoveResult::Failed;
            }
        }

        auto restoreTargetBackup = [&]() {
            if (backupPath.empty() || !std::filesystem::exists(backupPath)) return;
            RemovePath(targetPath);
            MoveFileExW(backupPath.c_str(), targetPath.c_str(), MOVEFILE_WRITE_THROUGH);
        };

        bool completed = false;
        if (IsSameVolume(sourcePath, targetDirectory)) {
            completed = MoveFileExW(sourcePath.c_str(), targetPath.c_str(),
                                    MOVEFILE_WRITE_THROUGH) == TRUE;
        }
        // 同卷原子重命名失败（文件夹被占用等）或跨卷 → fallback 到 copy+delete
        if (!completed) {
            const std::wstring stagingPath = MakeTemporarySibling(targetPath, L".desktodo-copying-");
            if (CopyPath(sourcePath, stagingPath) &&
                MoveFileExW(stagingPath.c_str(), targetPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
                if (RemovePath(sourcePath)) {
                    completed = true;
                } else {
                    RemovePath(targetPath);
                }
            } else {
                RemovePath(stagingPath);
            }
        }

        if (!completed) {
            restoreTargetBackup();
            return FileMoveResult::Failed;
        }

        if (!backupPath.empty()) RemovePath(backupPath);
        NotifyShellMove(sourcePath, targetPath, sourceAttributes);
        const std::wstring desktop = GetUserDesktopPath();
        if (reserveDesktopName && !desktop.empty() &&
            IsSamePath(source.parent_path().wstring(), desktop) &&
            !IsSamePath(targetDirectory, desktop)) {
            CreateNameReservation(sourcePath,
                sourceAttributes != INVALID_FILE_ATTRIBUTES &&
                (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
        if (movedPath) *movedPath = targetPath;
        return FileMoveResult::Success;
    } catch (...) {
        return FileMoveResult::Failed;
    }
}

bool FileMoveService::CreateNameReservation(const std::wstring& path, bool directory) const {
    if (path.empty() || std::filesystem::exists(path)) return false;
    std::error_code ec;
    if (directory) {
        if (!std::filesystem::create_directory(path, ec) || ec) return false;
        std::ofstream marker(std::filesystem::path(path) / DirectoryMarker,
                             std::ios::binary | std::ios::trunc);
        marker.write(ReservationMarker, sizeof(ReservationMarker) - 1);
        marker.close();
        if (!marker) { std::filesystem::remove_all(path, ec); return false; }
        SetFileAttributesW((std::filesystem::path(path) / DirectoryMarker).c_str(),
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    } else {
        std::ofstream marker(path, std::ios::binary | std::ios::trunc);
        marker.write(ReservationMarker, sizeof(ReservationMarker) - 1);
        marker.close();
        if (!marker) { std::filesystem::remove(path, ec); return false; }
    }
    if (!SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) {
        RemovePath(path);
        return false;
    }
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSH, path.c_str(), nullptr);
    return true;
}

bool FileMoveService::EnsureDesktopNameReserved(const std::wstring& managedPath) {
    if (managedPath.empty() || !std::filesystem::exists(managedPath)) return false;
    const std::wstring desktop = GetUserDesktopPath();
    if (desktop.empty()) return false;
    const std::filesystem::path managed(managedPath);
    if (IsSamePath(managed.parent_path().wstring(), desktop)) return true;
    const std::wstring reservation = (std::filesystem::path(desktop) / managed.filename()).wstring();
    if (std::filesystem::exists(reservation)) return IsNameReservation(reservation);
    std::error_code ec;
    const bool directory = std::filesystem::is_directory(managed, ec);
    return !ec && CreateNameReservation(reservation, directory);
}

bool FileMoveService::RemoveDesktopNameReservation(
    const std::wstring& managedPath) {
    if (managedPath.empty()) return false;
    const std::wstring desktop = GetUserDesktopPath();
    if (desktop.empty()) return false;
    const std::filesystem::path managed(managedPath);
    const std::wstring reservation =
        (std::filesystem::path(desktop) / managed.filename()).wstring();
    if (!std::filesystem::exists(reservation)) return true;
    if (!IsNameReservation(reservation)) return false;
    const DWORD attributes = GetFileAttributesW(reservation.c_str());
    const bool directory = attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!RemovePath(reservation)) return false;
    SHChangeNotify(directory ? SHCNE_RMDIR : SHCNE_DELETE,
                   SHCNF_PATHW | SHCNF_FLUSH,
                   reservation.c_str(), nullptr);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH,
                   desktop.c_str(), nullptr);
    return true;
}

bool FileMoveService::IsNameReservation(const std::wstring& path) const {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return false;
    std::filesystem::path markerPath(path);
    if (std::filesystem::is_directory(path, ec)) markerPath /= DirectoryMarker;
    if (ec || !std::filesystem::is_regular_file(markerPath, ec) || ec) return false;
    std::ifstream marker(markerPath, std::ios::binary);
    std::string value(sizeof(ReservationMarker) - 1, '\0');
    marker.read(value.data(), static_cast<std::streamsize>(value.size()));
    return marker.gcount() == static_cast<std::streamsize>(value.size()) &&
           value == ReservationMarker && marker.peek() == std::char_traits<char>::eof();
}

bool FileMoveService::CopyPath(const std::wstring& sourcePath, const std::wstring& targetPath) const {
    std::error_code ec;
    if (std::filesystem::is_directory(sourcePath, ec)) {
        ec.clear();
        std::filesystem::copy(sourcePath, targetPath,
                              std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::copy_symlinks, ec);
        return !ec && std::filesystem::exists(targetPath);
    }

    ec.clear();
    const bool copied = std::filesystem::copy_file(
        sourcePath, targetPath, std::filesystem::copy_options::none, ec);
    if (!copied || ec) return false;

    const auto sourceSize = std::filesystem::file_size(sourcePath, ec);
    if (ec) return false;
    ec.clear();
    const auto targetSize = std::filesystem::file_size(targetPath, ec);
    return !ec && sourceSize == targetSize;
}

bool FileMoveService::RemovePath(const std::wstring& path) const {
    if (path.empty() || !std::filesystem::exists(path)) return true;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        ec.clear();
        std::filesystem::remove_all(path, ec);
        return !ec && !std::filesystem::exists(path);
    }
    ec.clear();
    const bool removed = std::filesystem::remove(path, ec);
    return removed && !ec;
}

bool FileMoveService::IsSamePath(const std::wstring& path1, const std::wstring& path2) const {
    try {
        const auto p1 = std::filesystem::weakly_canonical(path1);
        const auto p2 = std::filesystem::weakly_canonical(path2);
        return _wcsicmp(p1.wstring().c_str(), p2.wstring().c_str()) == 0;
    } catch (...) {
        return _wcsicmp(path1.c_str(), path2.c_str()) == 0;
    }
}

bool FileMoveService::IsSameVolume(const std::wstring& path1, const std::wstring& path2) const {
    wchar_t volume1[MAX_PATH]{};
    wchar_t volume2[MAX_PATH]{};
    if (!GetVolumePathNameW(path1.c_str(), volume1, MAX_PATH) ||
        !GetVolumePathNameW(path2.c_str(), volume2, MAX_PATH)) {
        return _wcsicmp(std::filesystem::path(path1).root_name().wstring().c_str(),
                        std::filesystem::path(path2).root_name().wstring().c_str()) == 0;
    }
    return _wcsicmp(volume1, volume2) == 0;
}

void FileMoveService::NotifyShellMove(const std::wstring& sourcePath,
                                      const std::wstring& targetPath,
                                      DWORD sourceAttributes) const {
    const bool isDirectory = sourceAttributes != INVALID_FILE_ATTRIBUTES &&
                             (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY);
    SHChangeNotify(isDirectory ? SHCNE_RMDIR : SHCNE_DELETE,
                   SHCNF_PATHW | SHCNF_FLUSH,
                   sourcePath.c_str(), nullptr);
    SHChangeNotify(isDirectory ? SHCNE_MKDIR : SHCNE_CREATE,
                   SHCNF_PATHW | SHCNF_FLUSH,
                   targetPath.c_str(), nullptr);

    const std::wstring sourceParent = std::filesystem::path(sourcePath).parent_path().wstring();
    const std::wstring targetParent = std::filesystem::path(targetPath).parent_path().wstring();
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH,
                   sourceParent.c_str(), nullptr);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH,
                   targetParent.c_str(), nullptr);
}
