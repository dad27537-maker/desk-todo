#pragma once

#include <Windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

class LanTransferWindow {
public:
    explicit LanTransferWindow(HINSTANCE instance);
    ~LanTransferWindow();

    void ShowOrCreate();
    void Destroy();

private:
    friend struct LanTransferTestAccess;
    struct SharedFile {
        unsigned long long id;
        std::wstring path;
    };
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread serverThread_;
    uintptr_t listenSocket_ = static_cast<uintptr_t>(~0ull);
    unsigned short port_ = 0;
    std::wstring address_;
    std::wstring receiveDirectory_;
    std::string token_;
    std::atomic<unsigned long long> receivedCount_{0};
    std::atomic<unsigned long long> sentCount_{0};
    std::atomic<unsigned long long> transferBytes_{0};
    std::atomic<unsigned long long> transferTotal_{0};
    std::atomic<bool> transferActive_{false};
    std::mutex transferMutex_;
    std::wstring transferFileName_;
    std::vector<SharedFile> sharedFiles_;
    unsigned long long nextFileId_ = 1;

    bool EnsureWindow();
    bool StartServer();
    void StopServer();
    void ServerLoop();
    void HandleClient(uintptr_t socket);
    void SendFile(uintptr_t socket, const std::string& id);
    void ChooseSendFiles();
    void AddSendFiles(const std::vector<std::wstring>& paths);
    void ClearSendFiles();
    void Paint(HDC dc);
    void OpenAddress() const;
    void OpenReceiveDirectory() const;
    void ChooseReceiveDirectory();
    void LoadReceiveDirectory();
    void SaveReceiveDirectory() const;
    RECT OpenButtonRect() const;
    RECT FolderButtonRect() const;
    RECT ChooseFolderButtonRect() const;
    RECT CloseButtonRect() const;
    RECT SendButtonRect() const;
    RECT ClearButtonRect() const;

    static LRESULT CALLBACK StaticWindowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WindowProc(UINT, WPARAM, LPARAM);
};
