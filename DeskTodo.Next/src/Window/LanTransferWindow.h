#pragma once

#include <Windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>

class LanTransferWindow {
public:
    explicit LanTransferWindow(HINSTANCE instance);
    ~LanTransferWindow();

    void ShowOrCreate();
    void Destroy();

private:
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
    std::atomic<unsigned long long> transferBytes_{0};
    std::atomic<unsigned long long> transferTotal_{0};
    std::atomic<bool> transferActive_{false};
    std::mutex transferMutex_;
    std::wstring transferFileName_;

    bool EnsureWindow();
    bool StartServer();
    void StopServer();
    void ServerLoop();
    void HandleClient(uintptr_t socket);
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

    static LRESULT CALLBACK StaticWindowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WindowProc(UINT, WPARAM, LPARAM);
};
