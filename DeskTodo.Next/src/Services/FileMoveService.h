#pragma once

#include <Windows.h>
#include <string>

enum class FileMoveResult {
    Success,
    SameLocation,
    Conflict,
    Failed
};

class FileMoveService {
public:
    FileMoveResult MoveToDirectory(const std::wstring& sourcePath,
                                   const std::wstring& targetDirectory,
                                   bool overwrite,
                                   std::wstring* movedPath = nullptr,
                                   bool reserveDesktopName = false);
    bool EnsureDesktopNameReserved(const std::wstring& managedPath);
    bool RemoveDesktopNameReservation(const std::wstring& managedPath);
    bool IsSamePath(const std::wstring& path1, const std::wstring& path2) const;
    bool IsSameVolume(const std::wstring& path1, const std::wstring& path2) const;

private:
    bool CopyPath(const std::wstring& sourcePath, const std::wstring& targetPath) const;
    bool RemovePath(const std::wstring& path) const;
    bool CreateNameReservation(const std::wstring& path, bool directory) const;
    bool IsNameReservation(const std::wstring& path) const;
    void NotifyShellMove(const std::wstring& sourcePath,
                         const std::wstring& targetPath,
                         DWORD sourceAttributes) const;
};
