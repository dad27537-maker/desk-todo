#include "LanTransferWindow.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shellapi.h>
#include <iphlpapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>
#include <windowsx.h>

namespace {
constexpr wchar_t WindowClass[] = L"DeskTodoLanTransferWidget";
constexpr int WindowWidth = 620;
constexpr int WindowHeight = 464;
constexpr size_t MaxHeaderBytes = 64 * 1024;
constexpr unsigned long long MaxFileBytes = 4ull * 1024 * 1024 * 1024;
constexpr UINT RefreshMessage = WM_APP + 73;

// Version 5-L QR encoder. The transfer URL is short enough to fit its
// 108 data codewords, so the window can render a QR code without an online
// service or an additional runtime dependency.
std::vector<unsigned char> QrCodeV5L(const std::string& text) {
    constexpr int size = 37, dataWords = 108, eccWords = 26;
    std::vector<bool> bits{false, true, false, false}; // byte mode: 0100
    for (int i = 7; i >= 0; --i) bits.push_back((text.size() >> i) & 1);
    for (unsigned char ch : text)
        for (int i = 7; i >= 0; --i) bits.push_back((ch >> i) & 1);
    for (int i = 0; i < 4 && bits.size() < dataWords * 8; ++i) bits.push_back(false);
    while (bits.size() % 8) bits.push_back(false);
    std::vector<unsigned char> data;
    for (size_t i = 0; i < bits.size(); i += 8) {
        unsigned char value = 0;
        for (int j = 0; j < 8; ++j) value = static_cast<unsigned char>((value << 1) | (bits[i + j] ? 1 : 0));
        data.push_back(value);
    }
    for (bool toggle = true; data.size() < dataWords; toggle = !toggle)
        data.push_back(toggle ? 0xEC : 0x11);

    auto multiply = [](unsigned char x, unsigned char y) {
        int result = 0;
        for (int i = 7; i >= 0; --i) {
            result = (result << 1) ^ ((result >> 7) * 0x11D);
            if ((y >> i) & 1) result ^= x;
        }
        return static_cast<unsigned char>(result);
    };
    std::vector<unsigned char> divisor(eccWords);
    divisor[eccWords - 1] = 1;
    unsigned char root = 1;
    for (int i = 0; i < eccWords; ++i) {
        for (int j = 0; j < eccWords; ++j) {
            divisor[j] = multiply(divisor[j], root);
            if (j + 1 < eccWords) divisor[j] ^= divisor[j + 1];
        }
        root = multiply(root, 2);
    }
    std::vector<unsigned char> remainder(eccWords);
    for (unsigned char value : data) {
        const unsigned char factor = value ^ remainder[0];
        remainder.erase(remainder.begin()); remainder.push_back(0);
        for (int i = 0; i < eccWords; ++i) remainder[i] ^= multiply(divisor[i], factor);
    }
    data.insert(data.end(), remainder.begin(), remainder.end());

    std::vector<unsigned char> modules(size * size, 0), function(size * size, 0);
    auto set = [&](int x, int y, bool dark) { modules[y * size + x] = dark; function[y * size + x] = 1; };
    auto finder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx) {
            int x = cx + dx, y = cy + dy; if (x < 0 || y < 0 || x >= size || y >= size) continue;
            int distance = (std::max)(std::abs(dx), std::abs(dy)); set(x, y, distance != 2 && distance != 4);
        }
    };
    finder(3, 3); finder(size - 4, 3); finder(3, size - 4);
    for (int i = 0; i < size; ++i) { if (!function[6 * size + i]) set(i, 6, i % 2 == 0); if (!function[i * size + 6]) set(6, i, i % 2 == 0); }
    const int centers[] = {6, 30};
    for (int cy : centers) for (int cx : centers) {
        if (function[cy * size + cx]) continue;
        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx)
            set(cx + dx, cy + dy, (std::max)(std::abs(dx), std::abs(dy)) != 1);
    }
    constexpr int format = 0x77C4; // error correction L, mask 0
    auto formatBit = [&](int i) { return ((format >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) set(8, i, formatBit(i));
    set(8, 7, formatBit(6)); set(8, 8, formatBit(7)); set(7, 8, formatBit(8));
    for (int i = 9; i < 15; ++i) set(14 - i, 8, formatBit(i));
    for (int i = 0; i < 8; ++i) set(size - 1 - i, 8, formatBit(i));
    for (int i = 8; i < 15; ++i) set(8, size - 15 + i, formatBit(i));
    set(8, size - 8, true);

    size_t bitIndex = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) --right;
        for (int vertical = 0; vertical < size; ++vertical) {
            int y = (((right + 1) & 2) == 0) ? size - 1 - vertical : vertical;
            for (int j = 0; j < 2; ++j) {
                int x = right - j; if (function[y * size + x]) continue;
                bool dark = bitIndex < data.size() * 8 && ((data[bitIndex >> 3] >> (7 - (bitIndex & 7))) & 1);
                modules[y * size + x] = dark ^ ((x + y) % 2 == 0); ++bitIndex;
            }
        }
    }
    return modules;
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(pen); DeleteObject(brush);
}

void DrawLabel(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT flags) {
    auto oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, oldFont);
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::string EncodeUrl(const std::string& value) {
    const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') result += ch;
        else { result += '%'; result += hex[ch >> 4]; result += hex[ch & 15]; }
    }
    return result;
}

std::string JsonString(const std::wstring& value) {
    std::string result = "\"";
    const char hex[] = "0123456789abcdef";
    for (unsigned char ch : WideToUtf8(value)) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += ch; }
        else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += ch;
    }
    return result + "\"";
}

// Match whole query parameters, so a token prefix or a token inside a filename
// cannot authorize a request.
std::string QueryValue(const std::string& target, const std::string& name) {
    auto pos = target.find('?');
    if (pos == std::string::npos) return {};
    while (++pos < target.size()) {
        const auto end = target.find('&', pos);
        const auto parameter = target.substr(pos, end - pos);
        if (parameter.rfind(name + "=", 0) == 0) return parameter.substr(name.size() + 1);
        if (end == std::string::npos) break;
        pos = end;
    }
    return {};
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::wstring DecodeUrl(const std::string& value) {
    std::string bytes;
    bytes.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int high = HexValue(value[i + 1]);
            const int low = HexValue(value[i + 2]);
            if (high >= 0 && low >= 0) { bytes.push_back(static_cast<char>((high << 4) | low)); i += 2; continue; }
        }
        bytes.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return Utf8ToWide(bytes);
}

std::wstring SafeFileName(std::wstring name) {
    name = std::filesystem::path(name).filename().wstring();
    for (wchar_t& ch : name) {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty() || name == L"." || name == L"..") name = L"未命名文件";
    if (name.size() > 180) name.resize(180);
    return name;
}

std::filesystem::path UniquePath(const std::filesystem::path& directory, const std::wstring& fileName) {
    std::filesystem::path candidate = directory / fileName;
    const auto stem = candidate.stem().wstring();
    const auto extension = candidate.extension().wstring();
    for (int i = 1; std::filesystem::exists(candidate) && i < 10000; ++i)
        candidate = directory / (stem + L" (" + std::to_wstring(i) + L")" + extension);
    return candidate;
}

bool WaitForSocket(SOCKET socket, bool writing, const std::atomic<bool>& running) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (running && std::chrono::steady_clock::now() < deadline) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(socket, &ready);
        timeval timeout{0, 200000};
        int count = select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &timeout);
        if (count > 0) return running;
        if (count < 0) return false;
    }
    return false;
}

// Nonblocking sockets keep all socket operations and closure on the server
// thread, while short readiness waits let the UI cancel a stalled transfer.
class TransferIo {
public:
    TransferIo(SOCKET socket, const std::atomic<bool>& running) : socket_(socket), running_(running) {}

    int Receive(char* buffer, int length) {
        while (running_) {
            int count = recv(socket_, buffer, length, 0);
            if (count != SOCKET_ERROR) return count;
            if (WSAGetLastError() != WSAEWOULDBLOCK || !WaitForSocket(socket_, false, running_)) break;
        }
        return -1;
    }

    bool SendAll(const std::string& data) {
        size_t sent = 0;
        while (running_ && sent < data.size()) {
            const int count = send(socket_, data.data() + sent,
                                   static_cast<int>(std::min<size_t>(data.size() - sent, INT_MAX)), 0);
            if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                if (WaitForSocket(socket_, true, running_)) continue;
                return false;
            }
            if (count <= 0) return false;
            sent += count;
        }
        return sent == data.size();
    }

    void Respond(int status, const char* statusText, const std::string& type,
                  const std::string& body) {
        std::ostringstream header;
        header << "HTTP/1.1 " << status << ' ' << statusText << "\r\n"
               << "Content-Type: " << type << "\r\nContent-Length: " << body.size()
               << "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";
        if (SendAll(header.str())) SendAll(body);
    }
private:
    SOCKET socket_;
    const std::atomic<bool>& running_;
};

std::string HeaderValue(const std::string& headers, const std::string& name) {
    const std::string key = "\r\n" + name + ":";
    auto pos = headers.find(key);
    if (pos == std::string::npos) return {};
    pos += key.size();
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
    const auto end = headers.find("\r\n", pos);
    return headers.substr(pos, end - pos);
}

std::wstring LocalIpv4() {
    ULONG bytes = 16 * 1024;
    std::vector<unsigned char> storage(bytes);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG adapterResult = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &bytes);
    if (adapterResult == ERROR_BUFFER_OVERFLOW) {
        storage.resize(bytes); adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        adapterResult = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &bytes);
    }
    if (adapterResult == NO_ERROR) {
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || !adapter->FirstGatewayAddress ||
                (adapter->IfType != IF_TYPE_IEEE80211 && adapter->IfType != IF_TYPE_ETHERNET_CSMACD)) continue;
            for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
                if (!item->Address.lpSockaddr || item->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto* ipv4 = reinterpret_cast<sockaddr_in*>(item->Address.lpSockaddr);
                wchar_t buffer[64]{};
                if (InetNtopW(AF_INET, &ipv4->sin_addr, buffer, 64) && wcsncmp(buffer, L"169.254.", 8) != 0)
                    return buffer;
            }
        }
    }
    char host[256]{};
    if (gethostname(host, sizeof(host)) != 0) return L"127.0.0.1";
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0) return L"127.0.0.1";
    std::wstring fallback = L"127.0.0.1";
    for (auto* item = result; item; item = item->ai_next) {
        wchar_t buffer[64]{};
        auto* address = reinterpret_cast<sockaddr_in*>(item->ai_addr);
        InetNtopW(AF_INET, &address->sin_addr, buffer, 64);
        if (wcsncmp(buffer, L"127.", 4) != 0) { fallback = buffer; break; }
    }
    freeaddrinfo(result);
    return fallback;
}

std::string UploadPage(const std::string& token) {
    return "<!doctype html><html lang=zh-CN><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>DeskTodo 局域网快传</title><style>*{box-sizing:border-box}body{margin:0;background:#101116;color:#f7f7fa;font-family:system-ui;padding:24px}"
        ".card{max-width:560px;margin:24px auto;background:#20212a;border:1px solid #393b48;border-radius:24px;padding:28px}h1{font-size:25px;margin:0 0 8px}"
        "p{color:#aaaeba;line-height:1.6}.drop{display:block;border:2px dashed #565968;border-radius:18px;padding:34px 16px;text-align:center;cursor:pointer;margin-top:22px}"
        "input{display:none}button{width:100%;border:0;border-radius:13px;background:#ff6658;color:white;padding:14px;font-size:16px;font-weight:700;margin-top:16px}"
        "button:disabled{opacity:.45}.item{padding:10px 0;border-bottom:1px solid #343641;font-size:14px}.ok{color:#7adf9b}.bad{color:#ff8076}"
        ".download{display:block;color:#ffb7ad;padding:12px 0;overflow-wrap:anywhere}.item{overflow-wrap:anywhere}.progress{height:10px;background:#343641;border-radius:8px;overflow:hidden;margin-top:8px}.bar{height:100%;width:0;background:#ff6658}.pct{color:#c9ccd6;font-size:13px;margin-top:6px}</style>"
        "<div class=card><h1>接收电脑文件</h1><p>在电脑快传窗口点击“发送到手机”，或拖入文件，再点击下面的文件保存到手机。下载进度可在浏览器下载列表查看。</p>"
        "<div id=downloads aria-live=polite>正在读取文件列表…</div><p id=downloadStatus role=status></p><button id=refresh>刷新文件列表</button></div>"
        "<div class=card><h1>传文件到电脑</h1><p>手机与电脑需连接同一 Wi‑Fi。支持图片、视频和文档，可一次选择多个文件。</p>"
        "<label class=drop>点击选择文件<input id=f type=file multiple></label><div id=list></div><button id=send disabled>开始传输</button></div>"
        "<script>const f=document.querySelector('#f'),b=document.querySelector('#send'),l=document.querySelector('#list');let files=[];"
        "f.onchange=()=>{files=[...f.files];b.disabled=!files.length;l.innerHTML=files.map(x=>`<div class=item>${esc(x.name)} · ${size(x.size)}</div>`).join('')};"
        "function esc(s){let d=document.createElement('div');d.textContent=s;return d.innerHTML}function size(n){return n>1073741824?(n/1073741824).toFixed(1)+' GB':n>1048576?(n/1048576).toFixed(1)+' MB':(n/1024).toFixed(1)+' KB'}"
        "const token='" + token + "',dl=document.querySelector('#downloads'),ds=document.querySelector('#downloadStatus'),refresh=document.querySelector('#refresh');let listing='',loading=false;"
        "async function loadFiles(){if(loading)return;loading=true;refresh.disabled=true;try{const r=await fetch('/files?token='+token,{cache:'no-store'});if(!r.ok)throw Error();const items=await r.json();"
        "const next=JSON.stringify(items);if(next!==listing){dl.replaceChildren();if(!items.length)dl.textContent='暂无文件，请先在电脑上添加。';for(const x of items){const a=document.createElement('a');a.className='download';a.href='/download?token='+token+'&id='+encodeURIComponent(x.id);a.download=x.name;a.textContent=x.name+' · '+size(x.size);dl.append(a)}listing=next}ds.textContent='';"
        "}catch(e){ds.textContent='暂时无法连接电脑，请确认快传窗口仍然打开后刷新。'}finally{loading=false;refresh.disabled=false}}"
        "refresh.onclick=loadFiles;async function poll(){await loadFiles();setTimeout(poll,3000)}poll();"
        "b.onclick=async()=>{b.disabled=true;f.disabled=true;for(const x of files){let d=document.createElement('div');d.className='item';d.innerHTML='<div>'+esc(x.name)+'</div><div class=progress><div class=bar></div></div><div class=pct>0%</div>';l.append(d);let bar=d.querySelector('.bar'),pct=d.querySelector('.pct');try{await upload(x,p=>{bar.style.width=p+'%';pct.textContent=p+'% - '+size(Math.round(x.size*p/100))+' / '+size(x.size)});d.className='item ok';pct.textContent='已完成'}catch(e){d.className='item bad';pct.textContent='传输失败：'+e.message}}f.disabled=false;b.disabled=!files.length};"
        "function upload(x,progress){return new Promise((ok,bad)=>{let q=new XMLHttpRequest();q.open('POST','/upload?token=" + token + "&name='+encodeURIComponent(x.name));q.setRequestHeader('Content-Type','application/octet-stream');q.upload.onprogress=e=>{if(e.lengthComputable)progress(Math.round(e.loaded/e.total*100))};q.onload=()=>q.status>=200&&q.status<300?ok():bad(Error(q.responseText||q.status));q.onerror=()=>bad(Error('network interrupted'));q.send(x)})}</script></html>";
}
}

LanTransferWindow::LanTransferWindow(HINSTANCE instance) : instance_(instance) {
    PWSTR downloads = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &downloads))) {
        receiveDirectory_ = (std::filesystem::path(downloads) / L"DeskTodo 快传").wstring();
        CoTaskMemFree(downloads);
    } else receiveDirectory_ = (std::filesystem::temp_directory_path() / L"DeskTodo 快传").wstring();
    LoadReceiveDirectory();
    std::array<unsigned char, 8> bytes{};
    std::random_device random;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
    const char hex[] = "0123456789abcdef";
    for (auto byte : bytes) { token_.push_back(hex[byte >> 4]); token_.push_back(hex[byte & 15]); }
}

LanTransferWindow::~LanTransferWindow() { Destroy(); }

void LanTransferWindow::ShowOrCreate() {
    if (!EnsureWindow()) return;
    StartServer();
    ShowWindow(window_, SW_SHOWNORMAL);
    SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
    InvalidateRect(window_, nullptr, FALSE);
}

void LanTransferWindow::Destroy() {
    StopServer();
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
}

bool LanTransferWindow::EnsureWindow() {
    if (window_ && IsWindow(window_)) return true;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW cls{sizeof(cls)}; cls.lpfnWndProc = StaticWindowProc; cls.hInstance = instance_;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = WindowClass;
        registered = RegisterClassExW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) return false;
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, WindowClass, L"局域网快传", WS_POPUP,
        monitor.rcWork.right - WindowWidth - 28, monitor.rcWork.bottom - WindowHeight - 28,
        WindowWidth, WindowHeight, nullptr, nullptr, instance_, this);
    if (window_) DragAcceptFiles(window_, TRUE);
    return window_ != nullptr;
}

bool LanTransferWindow::StartServer() {
    if (running_) return true;
    WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { WSACleanup(); return false; }
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(18991);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        address.sin_port = 0;
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            closesocket(listener); WSACleanup(); return false;
        }
    }
    if (listen(listener, 8) != 0) {
        closesocket(listener); WSACleanup(); return false;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(listener, FIONBIO, &nonblocking) != 0) {
        closesocket(listener); WSACleanup(); return false;
    }
    int length = sizeof(address); getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port); listenSocket_ = listener;
    address_ = L"http://" + LocalIpv4() + L":" + std::to_wstring(port_) + L"/?token=" + Utf8ToWide(token_);
    std::filesystem::create_directories(receiveDirectory_);
    running_ = true; serverThread_ = std::thread(&LanTransferWindow::ServerLoop, this);
    return true;
}

void LanTransferWindow::StopServer() {
    if (!running_.exchange(false)) { ClearSendFiles(); return; }
    if (serverThread_.joinable()) serverThread_.join();
    if (listenSocket_ != INVALID_SOCKET) closesocket(static_cast<SOCKET>(listenSocket_));
    listenSocket_ = INVALID_SOCKET;
    transferActive_ = false;
    ClearSendFiles();
    WSACleanup(); port_ = 0; address_.clear();
}

void LanTransferWindow::ServerLoop() {
    while (running_) {
        if (!WaitForSocket(static_cast<SOCKET>(listenSocket_), false, running_)) continue;
        SOCKET client = accept(static_cast<SOCKET>(listenSocket_), nullptr, nullptr);
        if (client == INVALID_SOCKET) { if (!running_) break; continue; }
        HandleClient(client);
        closesocket(client);
    }
}

void LanTransferWindow::HandleClient(uintptr_t rawSocket) {
    SOCKET client = static_cast<SOCKET>(rawSocket);
    u_long nonblocking = 1;
    if (ioctlsocket(client, FIONBIO, &nonblocking) != 0) return;
    TransferIo io(client, running_);
    std::string request; std::array<char, 16384> buffer{}; size_t headerEnd = std::string::npos;
    while (request.size() < MaxHeaderBytes && headerEnd == std::string::npos) {
        const int count = io.Receive(buffer.data(), static_cast<int>(buffer.size()));
        if (count <= 0) return; request.append(buffer.data(), count); headerEnd = request.find("\r\n\r\n");
    }
    if (headerEnd == std::string::npos) { io.Respond(431, "Headers Too Large", "text/plain; charset=utf-8", "请求头过大"); return; }
    const auto lineEnd = request.find("\r\n"); const std::string firstLine = request.substr(0, lineEnd);
    const auto firstSpace = firstLine.find(' '); const auto secondSpace = firstLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) return;
    const std::string method = firstLine.substr(0, firstSpace); const std::string target = firstLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const std::string path = target.substr(0, target.find('?'));
    if (QueryValue(target, "token") != token_) { io.Respond(403, "Forbidden", "text/plain; charset=utf-8", "访问口令无效"); return; }
    if (method == "GET" && path == "/") { io.Respond(200, "OK", "text/html; charset=utf-8", UploadPage(token_)); return; }
    if (method == "GET" && path == "/files") {
        std::string body = "[";
        {
            std::lock_guard<std::mutex> lock(transferMutex_);
            for (const auto& file : sharedFiles_) {
                std::error_code error;
                auto size = std::filesystem::file_size(file.path, error);
                if (error) continue;
                if (body.size() > 1) body += ',';
                body += "{\"id\":\"" + std::to_string(file.id) + "\",\"name\":" +
                    JsonString(std::filesystem::path(file.path).filename().wstring()) + ",\"size\":" + std::to_string(size) + "}";
            }
        }
        io.Respond(200, "OK", "application/json; charset=utf-8", body + "]");
        return;
    }
    if (method == "GET" && path == "/download") { SendFile(rawSocket, QueryValue(target, "id")); return; }
    if (method != "POST" || path != "/upload") { io.Respond(404, "Not Found", "text/plain", "Not found"); return; }
    unsigned long long contentLength = 0;
    try { contentLength = std::stoull(HeaderValue(request.substr(0, headerEnd + 2), "Content-Length")); } catch (...) {
        io.Respond(411, "Length Required", "text/plain", "Content-Length required"); return;
    }
    if (contentLength == 0 || contentLength > MaxFileBytes) { io.Respond(413, "Payload Too Large", "text/plain; charset=utf-8", "文件为空或超过 4 GB"); return; }
    const auto encodedName = QueryValue(target, "name");
    if (encodedName.empty()) { io.Respond(400, "Bad Request", "text/plain", "Missing file name"); return; }
    const std::wstring fileName = SafeFileName(DecodeUrl(encodedName));
    std::wstring directory;
    { std::lock_guard<std::mutex> lock(transferMutex_); directory = receiveDirectory_; }
    const auto destination = UniquePath(directory, fileName);
    const auto temporary = destination.wstring() + L".desktodo-part";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) { io.Respond(500, "Internal Server Error", "text/plain; charset=utf-8", "无法创建文件"); return; }
    transferTotal_ = contentLength;
    transferBytes_ = 0;
    transferActive_ = true;
    { std::lock_guard<std::mutex> lock(transferMutex_); transferFileName_ = L"接收：" + fileName; }
    if (window_) PostMessageW(window_, RefreshMessage, 0, 0);
    const size_t bodyOffset = headerEnd + 4; unsigned long long written = request.size() - bodyOffset;
    if (written > contentLength) written = contentLength;
    stream.write(request.data() + bodyOffset, static_cast<std::streamsize>(written));
    transferBytes_ = written;
    while (written < contentLength) {
        const int count = io.Receive(buffer.data(), static_cast<int>(std::min<unsigned long long>(buffer.size(), contentLength - written)));
        if (count <= 0) { transferActive_ = false; stream.close(); std::filesystem::remove(temporary); if (window_) PostMessageW(window_, RefreshMessage, 0, 0); return; }
        stream.write(buffer.data(), count); written += count; transferBytes_ = written;
        if (window_) PostMessageW(window_, RefreshMessage, 0, 0);
    }
    stream.close();
    if (!stream) {
        transferActive_ = false;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (window_) PostMessageW(window_, RefreshMessage, 0, 0);
        io.Respond(500, "Internal Server Error", "text/plain; charset=utf-8", "写入文件失败，请检查磁盘空间");
        return;
    }
    std::error_code error; std::filesystem::rename(temporary, destination, error);
    if (error) { transferActive_ = false; std::filesystem::remove(temporary); if (window_) PostMessageW(window_, RefreshMessage, 0, 0); io.Respond(500, "Internal Server Error", "text/plain; charset=utf-8", "保存文件失败"); return; }
    transferActive_ = false; ++receivedCount_; if (window_) PostMessageW(window_, RefreshMessage, 0, 0);
    io.Respond(200, "OK", "application/json", "{\"ok\":true}");
}

void LanTransferWindow::SendFile(uintptr_t rawSocket, const std::string& id) {
    const SOCKET client = static_cast<SOCKET>(rawSocket);
    TransferIo io(client, running_);
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(transferMutex_);
        for (const auto& file : sharedFiles_)
            if (std::to_string(file.id) == id) { path = file.path; break; }
    }
    // Only files explicitly selected on this computer can be downloaded.
    // Never interpret a remote parameter as a filesystem path.
    if (path.empty()) {
        io.Respond(404, "Not Found", "text/plain; charset=utf-8", "文件已移除，请刷新列表");
        return;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER length{};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        io.Respond(404, "Not Found", "text/plain; charset=utf-8", "文件不存在或无法读取，请在电脑上重新添加");
        return;
    }
    const auto fileName = std::filesystem::path(path).filename().wstring();
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " << length.QuadPart
           << "\r\nContent-Disposition: attachment; filename=\"download\"; filename*=UTF-8''"
           << EncodeUrl(WideToUtf8(fileName))
           << "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    transferTotal_ = static_cast<unsigned long long>(length.QuadPart);
    transferBytes_ = 0;
    { std::lock_guard<std::mutex> lock(transferMutex_); transferFileName_ = L"发送：" + fileName; }
    transferActive_ = true;
    if (window_) PostMessageW(window_, RefreshMessage, 0, 0);
    bool success = io.SendAll(header.str());
    std::array<char, 64 * 1024> buffer{};
    unsigned long long sent = 0;
    while (success && running_ && sent < static_cast<unsigned long long>(length.QuadPart)) {
        DWORD count = 0;
        const DWORD wanted = static_cast<DWORD>((std::min)(
            static_cast<unsigned long long>(buffer.size()), static_cast<unsigned long long>(length.QuadPart) - sent));
        success = ReadFile(file, buffer.data(), wanted, &count, nullptr) && count != 0;
        if (success) success = io.SendAll(std::string(buffer.data(), count));
        if (success) {
            sent += count;
            transferBytes_ = sent;
            if (window_) PostMessageW(window_, RefreshMessage, 0, 0);
        }
    }
    CloseHandle(file);
    if (success && sent == static_cast<unsigned long long>(length.QuadPart)) ++sentCount_;
    transferActive_ = false;
    if (window_) PostMessageW(window_, RefreshMessage, 0, 0);
}

void LanTransferWindow::AddSendFiles(const std::vector<std::wstring>& paths) {
    size_t skipped = 0;
    {
        std::lock_guard<std::mutex> lock(transferMutex_);
        for (const auto& path : paths) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error)) { ++skipped; continue; }
            const auto duplicate = std::find_if(sharedFiles_.begin(), sharedFiles_.end(),
                [&](const SharedFile& file) { return _wcsicmp(file.path.c_str(), path.c_str()) == 0; });
            if (duplicate == sharedFiles_.end()) sharedFiles_.push_back({nextFileId_++, path});
        }
    }
    if (window_) {
        InvalidateRect(window_, nullptr, FALSE);
        if (skipped) MessageBoxW(window_, L"部分项目不是可读取的文件，已跳过。文件夹请先压缩后发送。",
                                L"发送到手机", MB_OK | MB_ICONINFORMATION);
    }
}

void LanTransferWindow::ClearSendFiles() {
    { std::lock_guard<std::mutex> lock(transferMutex_); sharedFiles_.clear(); }
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void LanTransferWindow::ChooseSendFiles() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择发送到手机的文件（可多选）");
    if (SUCCEEDED(dialog->Show(window_))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            std::vector<std::wstring> paths;
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        paths.emplace_back(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            items->Release();
            AddSendFiles(paths);
        }
    }
    dialog->Release();
}

RECT LanTransferWindow::SendButtonRect() const { return {20, 354, 210, 398}; }
RECT LanTransferWindow::ClearButtonRect() const { return {220, 354, 330, 398}; }
RECT LanTransferWindow::OpenButtonRect() const { return {250, 292, 365, 338}; }
RECT LanTransferWindow::ChooseFolderButtonRect() const { return {375, 292, 480, 338}; }
RECT LanTransferWindow::FolderButtonRect() const { return {490, 292, 590, 338}; }
RECT LanTransferWindow::CloseButtonRect() const { return {582, 10, 610, 38}; }

void LanTransferWindow::Paint(HDC dc) {
    RECT client{}; GetClientRect(window_, &client); FillRounded(dc, client, 22, RGB(247,252,250));
    HFONT title = CreateFontW(-19,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI Variable Display");
    HFONT body = CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI Variable Text");
    HFONT url = CreateFontW(-16,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
    DrawLabel(dc, L"局域网快传", {20,12,250,40}, title, RGB(19,78,74), DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(dc, L"×", CloseButtonRect(), title, RGB(75,112,108), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(dc, running_ ? L"手机与电脑连接同一 Wi‑Fi，然后在手机浏览器打开：" : L"服务启动失败，请检查网络权限。",
              {250,54,590,92}, body, RGB(75,112,108), DT_LEFT|DT_WORDBREAK);
    FillRounded(dc, {250,96,590,140}, 10, RGB(225,246,239));
    DrawLabel(dc, address_, {262,96,578,140}, url, RGB(13,148,136), DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    DrawLabel(dc, L"手机扫码后可上传文件到电脑，也可下载下方添加的电脑文件。",
              {250,154,590,206}, body, RGB(47,91,87), DT_LEFT|DT_WORDBREAK);
    const std::wstring status = L"已接收 " + std::to_wstring(receivedCount_.load()) +
        L" / 已发送 " + std::to_wstring(sentCount_.load()) + L"  ·  " + receiveDirectory_;
    DrawLabel(dc, status, {250,214,590,238}, body, RGB(157,160,173), DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    const auto total = transferTotal_.load(), transferred = transferBytes_.load();
    if (transferActive_ && total > 0) {
        std::wstring name;
        { std::lock_guard<std::mutex> lock(transferMutex_); name = transferFileName_; }
        const unsigned int percent = static_cast<unsigned int>((std::min)(100ull, transferred * 100 / total));
        DrawLabel(dc, name + L"  " + std::to_wstring(percent) + L"%", {250,240,590,262}, body,
                  RGB(47,91,87), DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        FillRounded(dc, {250,268,590,278}, 6, RGB(55,57,69));
        RECT progress{250,268,250 + static_cast<int>(340 * percent / 100),278};
        if (progress.right > progress.left) FillRounded(dc, progress, 6, RGB(255,102,88));
    }
    FillRounded(dc, {20,56,226,262}, 12, RGB(255,255,255));
    if (!address_.empty()) {
        const int qrSize = 37, module = 4, originX = 49, originY = 85;
        std::string utf8;
        int count = WideCharToMultiByte(CP_UTF8, 0, address_.data(), static_cast<int>(address_.size()), nullptr, 0, nullptr, nullptr);
        if (count > 0) { utf8.resize(count); WideCharToMultiByte(CP_UTF8, 0, address_.data(), static_cast<int>(address_.size()), utf8.data(), count, nullptr, nullptr); }
        const auto qr = QrCodeV5L(utf8);
        HBRUSH black = CreateSolidBrush(RGB(10,10,12));
        for (int y = 0; y < qrSize; ++y) for (int x = 0; x < qrSize; ++x) if (qr[y * qrSize + x]) {
            RECT cell{originX + x * module, originY + y * module, originX + (x + 1) * module, originY + (y + 1) * module}; FillRect(dc, &cell, black);
        }
        DeleteObject(black);
    }
    FillRounded(dc, OpenButtonRect(), 11, RGB(244,145,128));
    FillRounded(dc, ChooseFolderButtonRect(), 11, RGB(225,246,239));
    FillRounded(dc, FolderButtonRect(), 11, RGB(225,246,239));
    DrawLabel(dc, L"本机浏览器", OpenButtonRect(), body, RGB(255,255,255), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(dc, L"更改保存位置", ChooseFolderButtonRect(), body, RGB(19,78,74), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(dc, L"打开文件夹", FolderButtonRect(), body, RGB(19,78,74), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    FillRounded(dc, SendButtonRect(), 11, RGB(13,148,136));
    FillRounded(dc, ClearButtonRect(), 11, RGB(225,246,239));
    DrawLabel(dc, L"发送到手机（可多选）", SendButtonRect(), body, RGB(255,255,255), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(dc, L"清空发送列表", ClearButtonRect(), body, RGB(19,78,74), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    std::wstring summary;
    size_t sharedCount = 0;
    {
        std::lock_guard<std::mutex> lock(transferMutex_);
        sharedCount = sharedFiles_.size();
        for (const auto& file : sharedFiles_) {
            if (!summary.empty()) summary += L"、";
            summary += std::filesystem::path(file.path).filename().wstring();
            if (summary.size() > 200) break;
        }
    }
    DrawLabel(dc, L"已添加 " + std::to_wstring(sharedCount) + L" 个文件", {344,354,590,398}, body,
              RGB(47,91,87), DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    DrawLabel(dc, summary.empty() ? L"也可将文件拖入此窗口；关闭窗口后发送列表自动清空。" : summary,
              {20,408,590,444}, body, RGB(75,112,108), DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    DeleteObject(title); DeleteObject(body); DeleteObject(url);
}

void LanTransferWindow::OpenAddress() const { if (!address_.empty()) ShellExecuteW(nullptr, L"open", address_.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
void LanTransferWindow::OpenReceiveDirectory() const { std::filesystem::create_directories(receiveDirectory_); ShellExecuteW(nullptr, L"open", receiveDirectory_.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void LanTransferWindow::LoadReceiveDirectory() {
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &roaming))) return;
    const auto path = std::filesystem::path(roaming) / L"DeskTodo.Next" / L"lan-transfer-folder.txt";
    CoTaskMemFree(roaming);
    std::wifstream stream(path);
    std::wstring saved;
    if (stream && std::getline(stream, saved) && !saved.empty() && std::filesystem::is_directory(saved))
        receiveDirectory_ = saved;
}

void LanTransferWindow::SaveReceiveDirectory() const {
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &roaming))) return;
    const auto directory = std::filesystem::path(roaming) / L"DeskTodo.Next";
    CoTaskMemFree(roaming);
    std::filesystem::create_directories(directory);
    std::wofstream(directory / L"lan-transfer-folder.txt", std::ios::trunc) << receiveDirectory_;
}

void LanTransferWindow::ChooseReceiveDirectory() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择局域网快传的保存位置");
    if (SUCCEEDED(dialog->Show(window_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                { std::lock_guard<std::mutex> lock(transferMutex_); receiveDirectory_ = path; }
                CoTaskMemFree(path);
                SaveReceiveDirectory();
                InvalidateRect(window_, nullptr, FALSE);
            }
            item->Release();
        }
    }
    dialog->Release();
}

LRESULT CALLBACK LanTransferWindow::StaticWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<LanTransferWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<LanTransferWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); self->window_ = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    return self ? self->WindowProc(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}

LRESULT LanTransferWindow::WindowProc(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc=BeginPaint(window_,&ps); RECT r{};GetClientRect(window_,&r);HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,r.right,r.bottom);auto old=SelectObject(mem,bmp);Paint(mem);BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);EndPaint(window_,&ps);return 0; }
    case RefreshMessage: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(length);
            paths.push_back(std::move(path));
        }
        DragFinish(drop);
        AddSendFiles(paths);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        auto close = CloseButtonRect(), open = OpenButtonRect(), choose = ChooseFolderButtonRect(), folder = FolderButtonRect();
        if (PtInRect(&close, p)) {
            PostMessageW(window_, WM_CLOSE, 0, 0);
            return 0;
        }
        if (PtInRect(&open, p)) { OpenAddress(); return 0; }
        if (PtInRect(&choose, p)) { ChooseReceiveDirectory(); return 0; }
        if (PtInRect(&folder, p)) { OpenReceiveDirectory(); return 0; }
        auto send = SendButtonRect(), clear = ClearButtonRect();
        if (PtInRect(&send, p)) { ChooseSendFiles(); return 0; }
        if (PtInRect(&clear, p)) { ClearSendFiles(); return 0; }
        if (p.y < 45) { ReleaseCapture(); SendMessageW(window_, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        return 0;
    }
    case WM_CLOSE:
        StopServer();
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY: window_ = nullptr; return 0;
    }
    return DefWindowProcW(window_, message, wp, lp);
}
