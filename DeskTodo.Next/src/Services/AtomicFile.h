#pragma once

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <string_view>

namespace AtomicFile {
// Own only a newly created sibling. The previous file survives any failed
// write, flush or commit; concurrent writers never share a temporary file.
inline bool Write(const std::wstring& path, std::string_view bytes) {
    static std::atomic<unsigned long long> sequence{0};
    struct Temporary {
        std::wstring path;
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~Temporary() {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            if (!path.empty()) DeleteFileW(path.c_str());
        }
    } temporary;
    try {
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
            temporary.handle = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (temporary.handle != INVALID_HANDLE_VALUE) { temporary.path = std::move(candidate); break; }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        }
        if (temporary.handle == INVALID_HANDLE_VALUE) return false;
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD count = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{MAXDWORD}));
            DWORD written = 0;
            if (!WriteFile(temporary.handle, bytes.data() + offset, count, &written, nullptr) || !written) return false;
            offset += written;
        }
        if (!FlushFileBuffers(temporary.handle)) return false;
        const bool closed = CloseHandle(temporary.handle) != FALSE;
        temporary.handle = INVALID_HANDLE_VALUE;
        if (!closed || !MoveFileExW(temporary.path.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
        temporary.path.clear();
        return true;
    } catch (...) { return false; }
}
}
