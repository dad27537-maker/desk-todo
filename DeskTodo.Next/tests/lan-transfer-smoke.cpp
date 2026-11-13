#include <WinSock2.h>
#include <WS2tcpip.h>
#include <ShlObj.h>
#include "Window/LanTransferWindow.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

struct LanTransferTestAccess {
    static bool Start(LanTransferWindow& server, const std::filesystem::path& directory) {
        server.receiveDirectory_ = directory.wstring();
        return server.StartServer();
    }
    static void Stop(LanTransferWindow& server) { server.StopServer(); }
    static unsigned short Port(const LanTransferWindow& server) { return server.port_; }
    static std::string Token(const LanTransferWindow& server) { return server.token_; }
    static void Add(LanTransferWindow& server, const std::vector<std::wstring>& paths) { server.AddSendFiles(paths); }
    static void Clear(LanTransferWindow& server) { server.ClearSendFiles(); }
    static bool Active(const LanTransferWindow& server) { return server.transferActive_; }
    static bool ShareText(LanTransferWindow& server, const std::wstring& text) { return server.ShareText(text); }
    static std::wstring ReceivedText(LanTransferWindow& server) {
        std::lock_guard<std::mutex> lock(server.transferMutex_);
        return server.receivedText_;
    }
    static void ClearTexts(LanTransferWindow& server) { server.ClearTexts(); }
    static HWND TextEdit(LanTransferWindow& server) {
        server.RefreshTextControls();
        return server.receivedTextEdit_;
    }
    static unsigned long long Sent(const LanTransferWindow& server) { return server.sentCount_; }
    static HWND Window(LanTransferWindow& server) {
        return server.EnsureWindow() ? server.window_ : nullptr;
    }
};

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Connection {
    SOCKET socket = INVALID_SOCKET;
    explicit Connection(unsigned short port) {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(socket != INVALID_SOCKET, "socket");
        DWORD timeout = 5000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            closesocket(socket); socket = INVALID_SOCKET;
            throw std::runtime_error("connect");
        }
    }
    ~Connection() { if (socket != INVALID_SOCKET) closesocket(socket); }
    void Write(const std::string& data) {
        size_t written = 0;
        while (written < data.size()) {
            int count = send(socket, data.data() + written, static_cast<int>(data.size() - written), 0);
            Require(count > 0, "send");
            written += count;
        }
    }
};

std::string Request(unsigned short port, const std::string& target,
                    const std::string& method = "GET", const std::string& body = "",
                    const std::string& lengthHeader = "Content-Length") {
    Connection connection(port);
    connection.Write(method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n" + lengthHeader + ": " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    std::string response;
    char buffer[8192];
    for (;;) {
        const int count = recv(connection.socket, buffer, sizeof(buffer), 0);
        Require(count >= 0, "receive timed out");
        if (!count) break;
        response.append(buffer, count);
    }
    return response;
}

std::string Body(const std::string& response) {
    auto end = response.find("\r\n\r\n");
    Require(end != std::string::npos, "missing headers");
    return response.substr(end + 4);
}

void Status(const std::string& response, int status) {
    Require(response.rfind("HTTP/1.1 " + std::to_string(status) + " ", 0) == 0, "unexpected HTTP status");
}

std::string FirstId(const std::string& json) {
    const auto start = json.find("\"id\":\"");
    Require(start != std::string::npos, "missing file id");
    const auto end = json.find('"', start + 6);
    return json.substr(start + 6, end - start - 6);
}

void WaitActive(const LanTransferWindow& server) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!LanTransferTestAccess::Active(server) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Require(LanTransferTestAccess::Active(server), "transfer did not start");
}

void StopPromptly(LanTransferWindow& server) {
    auto start = std::chrono::steady_clock::now();
    LanTransferTestAccess::Stop(server);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed >= std::chrono::seconds(2))
        throw std::runtime_error("stop blocked on a client for " +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms");
}
}

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    const auto directory = std::filesystem::current_path() / ("lan-transfer-smoke-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    int result = 0;
    try {
        LanTransferWindow server(GetModuleHandleW(nullptr));
        const auto invalidDirectory = directory / L"not-a-directory";
        { std::ofstream marker(invalidDirectory); marker << "keep"; }
        Require(!LanTransferTestAccess::Start(server, invalidDirectory), "invalid receive directory must fail without throwing");
        Require(LanTransferTestAccess::Port(server) == 0, "failed startup must release its port and state");
        Require(LanTransferTestAccess::Start(server, directory), "start server");
        const auto port = LanTransferTestAccess::Port(server);
        const auto token = LanTransferTestAccess::Token(server);
        const auto auth = "?token=" + token;
        const auto page = Request(port, "/" + auth);
        Status(page, 200);
        Require(Body(page).find("id=downloads") != std::string::npos, "missing download interface");
        Require(Body(page).find("type=file multiple") != std::string::npos, "missing upload interface");
        Require(Body(page).find("id=phoneText") != std::string::npos, "missing text send interface");
        Require(Body(page).find("id=computerText") != std::string::npos, "missing text receive interface");
        Require(Body(page).find("execCommand('copy')") != std::string::npos, "missing HTTP clipboard fallback");
        std::ofstream("lan-transfer-smoke-page.html", std::ios::binary) << Body(page);
        Status(Request(port, "/files"), 403);
        Status(Request(port, "/files?token=" + token + "suffix"), 403);
        Status(Request(port, "/files?name=token=" + token), 403);
        Require(Body(Request(port, "/files" + auth)) == "[]", "initial list is not empty");
        Status(Request(port, "/unknown" + auth), 404);
        Status(Request(port, "/download" + auth + "&id=../../README.md"), 404);
        {
            Connection oversizedHeader(port);
            std::string header = "GET /files" + auth + " HTTP/1.1\r\nX-Padding: ";
            header.append(65537 - header.size() - 4, 'x');
            oversizedHeader.Write(header + "\r\n\r\n");
            char reply[1024]{};
            const int count = recv(oversizedHeader.socket, reply, sizeof(reply), 0);
            Require(count > 0, "oversized header did not respond");
            Status(std::string(reply, count), 431);
        }

        const auto initialText = Body(Request(port, "/text" + auth));
        Require(initialText.find("\"text\":\"\"") != std::string::npos, "text should start empty");
        const std::wstring desktopText = L"电脑中文 😀\r\n\"quoted\" & <script>alert(1)</script>\t\\路径";
        Require(LanTransferTestAccess::ShareText(server, desktopText), "share PC text");
        const auto sharedText = Request(port, "/text" + auth);
        Status(sharedText, 200);
        Require(sharedText.find("Cache-Control: no-store") != std::string::npos, "text response cached");
        Require(Body(sharedText).find("电脑中文 😀\\u000d\\u000a\\\"quoted\\\"") != std::string::npos, "Unicode text JSON escaping");
        Require(Body(sharedText) != initialText, "text version not advanced");
        Require(!LanTransferTestAccess::ShareText(server, L""), "accepted empty PC text");
        Require(!LanTransferTestAccess::ShareText(server, std::wstring(1048577, L'x')), "accepted oversized PC text");
        Require(!LanTransferTestAccess::ShareText(server, std::wstring(400000, L'文')), "PC limit must count UTF-8 bytes");
        Require(!LanTransferTestAccess::ShareText(server, std::wstring(1, static_cast<wchar_t>(0xD800))), "accepted unpaired surrogate");
        Status(Request(port, "/text"), 403);
        Status(Request(port, "/text?token=" + token + "suffix", "POST", "forbidden"), 403);
        Require(LanTransferTestAccess::ReceivedText(server).empty(), "unauthorized text accepted");
        const std::string phoneText = "手机复制文本 🐱\nsecond line\t<&>\"\\";
        Status(Request(port, "/text" + auth, "POST", phoneText, "content-length"), 200);
        Require(LanTransferTestAccess::ReceivedText(server) == L"手机复制文本 🐱\nsecond line\t<&>\"\\", "phone Unicode text differs");
        Require(Body(Request(port, "/text" + auth)) == Body(sharedText), "incoming text overwrote outgoing text");
        Status(Request(port, "/text" + auth, "POST", ""), 413);
        Status(Request(port, "/text" + auth, "POST", std::string("\xc0\xaf", 2)), 400);
        Status(Request(port, "/text" + auth, "POST", std::string("a\0b", 3)), 400);
        Require(LanTransferTestAccess::ReceivedText(server) == L"手机复制文本 🐱\nsecond line\t<&>\"\\", "rejected text overwrote received text");
        {
            Connection oversized(port);
            oversized.Write("POST /text" + auth + " HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n");
            char response[1024]{};
            const int count = recv(oversized.socket, response, sizeof(response), 0);
            Require(count > 0, "oversized text did not respond");
            Status(std::string(response, count), 413);
        }
        const std::string maxText(1048576, 'x');
        Status(Request(port, "/text" + auth, "POST", maxText), 200);
        Require(LanTransferTestAccess::ReceivedText(server).size() == maxText.size(), "large text truncated");
        Require(LanTransferTestAccess::ShareText(server, std::wstring(1048576, L'x')), "maximum PC text rejected");
        LanTransferTestAccess::ClearTexts(server);
        Require(LanTransferTestAccess::ReceivedText(server).empty(), "received text not cleared");
        Require(Body(Request(port, "/text" + auth)).find("\"text\":\"\"") != std::string::npos, "shared text not cleared");

        const auto file = directory / L"手机 & '资料'.bin";
        std::string bytes(200123, '\0');
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
        std::ofstream(file, std::ios::binary).write(bytes.data(), bytes.size());
        const auto empty = directory / L"empty.txt";
        std::ofstream(empty).close();
        LanTransferTestAccess::Add(server, {file.wstring(), file.wstring(), directory.wstring(), empty.wstring()});
        const auto list = Body(Request(port, "/files" + auth));
        Require(list.find("手机 & '资料'.bin") != std::string::npos, "Unicode filename missing");
        Require(list.find("\"size\":200123") != std::string::npos, "file size incorrect");
        Require(list.find("lan-transfer-smoke-") == std::string::npos, "local path exposed");
        const auto id = FirstId(list);
        const auto response = Request(port, "/download" + auth + "&id=" + id);
        Status(response, 200);
        Require(Body(response) == bytes, "download bytes differ");
        Require(response.find("Content-Length: 200123\r\n") != std::string::npos, "download length");
        Require(response.find("filename*=UTF-8''%E6%89%8B%E6%9C%BA%20%26%20%27") != std::string::npos, "filename encoding");
        Require(response.find("Cache-Control: no-store") != std::string::npos, "downloads must not be cached");
        const auto secondStart = list.find("\"id\":\"", list.find("\"id\":\"") + 1);
        Require(secondStart != std::string::npos, "empty file missing");
        Require(list.find("\"id\":\"", secondStart + 1) == std::string::npos, "duplicate or folder shared");
        const auto emptyId = FirstId(list.substr(secondStart));
        const auto emptyResponse = Request(port, "/download" + auth + "&id=" + emptyId);
        Status(emptyResponse, 200);
        Require(Body(emptyResponse).empty(), "empty file download");
        Require(LanTransferTestAccess::Sent(server) == 2, "completed send count");
        Status(Request(port, "/download?token=wrong&id=" + id), 403);
        LanTransferTestAccess::Clear(server);
        Require(Body(Request(port, "/files" + auth)) == "[]", "clear list");
        Status(Request(port, "/download" + auth + "&id=" + id), 404);
        LanTransferTestAccess::Add(server, {file.wstring()});
        const auto newId = FirstId(Body(Request(port, "/files" + auth)));
        Require(newId != id, "cleared IDs reused");
        const HWND window = LanTransferTestAccess::Window(server);
        Require(window != nullptr, "create native window");
        Status(Request(port, "/text" + auth, "POST", phoneText), 200);
        const auto textEdit = LanTransferTestAccess::TextEdit(server);
        Require(textEdit && (GetWindowLongPtrW(textEdit, GWL_STYLE) & ES_READONLY), "missing read-only text preview");
        wchar_t preview[256]{};
        GetWindowTextW(textEdit, preview, 256);
        Require(std::wstring(preview) == L"手机复制文本 🐱\r\nsecond line\t<&>\"\\", "text preview lost content or line endings");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(520, 536));
        Require(LanTransferTestAccess::ReceivedText(server).empty(), "clear text button failed");
        Require(GetWindowTextLengthW(textEdit) == 0, "text preview not cleared");
        Require((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0, "file drop disabled");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(230, 370));
        Require(Body(Request(port, "/files" + auth)) == "[]", "clear button did not clear files");
        const auto droppedPath = file.wstring();
        const size_t dropBytes = sizeof(DROPFILES) + (droppedPath.size() + 2) * sizeof(wchar_t);
        HGLOBAL drop = GlobalAlloc(GHND, dropBytes);
        Require(drop != nullptr, "allocate drop");
        auto* dropData = static_cast<DROPFILES*>(GlobalLock(drop));
        dropData->pFiles = sizeof(DROPFILES);
        dropData->fWide = TRUE;
        memcpy(reinterpret_cast<char*>(dropData) + sizeof(DROPFILES), droppedPath.c_str(),
               (droppedPath.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(drop);
        SendMessageW(window, WM_DROPFILES, reinterpret_cast<WPARAM>(drop), 0);
        const auto droppedList = Body(Request(port, "/files" + auth));
        Require(droppedList.find("手机 & '资料'.bin") != std::string::npos, "file drop did not add files");
        const auto droppedId = FirstId(droppedList);
        std::filesystem::remove(file);
        Status(Request(port, "/download" + auth + "&id=" + droppedId), 404);
        Require(Body(Request(port, "/files" + auth)) == "[]", "deleted file still listed");

        const auto existingPart = directory / L"手机.bin.desktodo-part";
        { std::ofstream preserved(existingPart); preserved << "existing user data"; }
        Status(Request(port, "/upload" + auth + "&name=%E6%89%8B%E6%9C%BA.bin", "POST", bytes), 200);
        { std::ifstream preserved(existingPart);
          Require(std::string(std::istreambuf_iterator<char>(preserved), {}) == "existing user data",
                  "upload must not truncate an existing temporary-looking file"); }
        std::ifstream uploaded(directory / L"手机.bin", std::ios::binary);
        std::string saved((std::istreambuf_iterator<char>(uploaded)), std::istreambuf_iterator<char>());
        Require(saved == bytes, "upload regression: bytes differ");
        uploaded.close();
        Status(Request(port, "/upload" + auth + "&name=%E6%89%8B%E6%9C%BA.bin", "POST", "second"), 200);
        Require(std::filesystem::exists(directory / L"手机 (1).bin"), "upload overwrote a file");
        {
            Connection conflict(port);
            conflict.Write("POST /upload" + auth + "&name=conflict.bin HTTP/1.1\r\nContent-Length: 4\r\n\r\nx");
            WaitActive(server);
            const auto destination = directory / L"conflict.bin";
            { std::ofstream existing(destination); existing << "created during upload"; }
            conflict.Write("yzq");
            char reply[1024]{};
            const int count = recv(conflict.socket, reply, sizeof(reply), 0);
            Require(count > 0, "conflicting destination did not respond");
            Status(std::string(reply, count), 500);
            std::ifstream existing(destination);
            Require(std::string(std::istreambuf_iterator<char>(existing), {}) == "created during upload",
                    "upload must not overwrite a destination created while receiving");
        }
        Status(Request(port, "/files" + auth), 200);
        {
            Connection stalled(port);
            stalled.Write("POST /upload" + auth + "&name=interrupted.bin HTTP/1.1\r\nContent-Length: 99999999\r\n\r\nx");
            WaitActive(server);
            std::cout << "Stopping interrupted upload..." << std::endl;
            StopPromptly(server);
            Require(!std::filesystem::exists(directory / L"interrupted.bin.desktodo-part"), "partial upload not removed");
            for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                Require(entry.path().filename().wstring().find(L"interrupted.bin.desktodo-part-") != 0,
                        "cancelled upload must remove its reserved temporary file");
            }
        }
        Require(LanTransferTestAccess::Start(server, directory), "restart server");
        const auto nextPort = LanTransferTestAccess::Port(server);
        Require(Body(Request(nextPort, "/files" + auth)) == "[]", "session files survived shutdown");
        Require(Body(Request(nextPort, "/text" + auth)).find("\"text\":\"\"") != std::string::npos, "session text survived shutdown");
        Require(LanTransferTestAccess::ReceivedText(server).empty(), "received text survived shutdown");
        const auto large = directory / L"large.bin";
        std::ofstream(large).close();
        std::filesystem::resize_file(large, 64ull * 1024 * 1024);
        LanTransferTestAccess::Add(server, {large.wstring()});
        const auto largeId = FirstId(Body(Request(nextPort, "/files" + auth)));
        {
            Connection stalled(nextPort);
            int smallBuffer = 1024;
            setsockopt(stalled.socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&smallBuffer), sizeof(smallBuffer));
            stalled.Write("GET /download" + auth + "&id=" + largeId + " HTTP/1.1\r\n\r\n");
            WaitActive(server);
            std::cout << "Stopping interrupted download..." << std::endl;
            StopPromptly(server);
            Require(LanTransferTestAccess::Sent(server) == 2, "cancelled download counted as complete");
        }
        std::cout << "LAN transfer smoke passed: bidirectional Unicode text, limits, preview, downloads, auth, upload regression, cancellation, session cleanup.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(directory);
    WSACleanup();
    return result;
}
