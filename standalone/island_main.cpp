// ─────────────────────────────────────────────────────────────────────────────
// island_main.cpp — host executable for the Dynamic Island standalone build.
//
// Drives the mod's own tool-mod lifecycle directly, with no Windhawk:
//   WhTool_ModInit()          → LoadSettings + StartThreads (island window)
//   WhTool_ModSettingsChanged() → re-run LoadSettings when island.ini changes
//   WhTool_ModUninit()        → posts WM_CLOSE to the island, joins all threads
//
// CLI:
//   DynamicIsland.exe              start the island (exits if already running)
//   DynamicIsland.exe --quit       gracefully stop a running instance
//   DynamicIsland.exe --log        append a debug log to <base>\island.log
//
// Quit also works via the registered hotkey Ctrl+Alt+Shift+X.
// Settings live in island.ini next to the exe (created from defaults on first
// run); changes reload within ~1 second. A tiny loopback HTTP server runs
// alongside the watcher: "Open Settings" opens the HTML page from it and the
// page saves island.ini with one click (falls back to the file:// flow when
// the server is not running).
// ─────────────────────────────────────────────────────────────────────────────

#include <windows.h>
#include <cwchar>
#include <cstdlib>
#include <atomic>
#include <cctype>
#include <cstring>
#include <string>

// Real lifecycle entry points of the mod (global scope in the .wh.cpp TU).
BOOL WhTool_ModInit();
void WhTool_ModSettingsChanged();
void WhTool_ModUninit();

namespace {

constexpr wchar_t kMutexName[] = L"Local\\DynamicIslandForWindowsSingleInstance";
constexpr wchar_t kQuitEventName[] = L"Local\\DynamicIslandForWindowsQuit";

constexpr unsigned kQuitHotkeyMods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
constexpr unsigned kQuitHotkeyVk = 'X';  // Ctrl+Alt+Shift+X

bool ArgContains(PCWSTR needle) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        if (lstrcmpiW(argv[i], needle) == 0) {
            LocalFree(argv);
            return true;
        }
    }
    LocalFree(argv);
    return false;
}

// Reflects Behavior.RunOnStartup in the per-user Windows startup entry
// (HKCU\Software\Microsoft\Windows\CurrentVersion\Run). The path is rewritten
// on every launch so the entry follows the exe if it is moved, and the entry
// is removed when the setting is turned off. Portable by default in every
// other respect: nothing is written unless this setting is on.
void EnsureRunOnStartup() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (Wh_GetIntSetting(L"Behavior.RunOnStartup") != 0) {
        wchar_t path[MAX_PATH + 1] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, L"DynamicIsland", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) *
                                          sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"DynamicIsland");
    }
    RegCloseKey(key);
}

// Polls island.ini's last-write time once per second and reloads the mod
// settings whenever it changes. Exits when stopEvent is signalled. Mirrors the
// API header's path rules: DYNAMIC_ISLAND_INI env override, else
// <exe dir>\island.ini.
DWORD WINAPI SettingsWatcherThread(LPVOID stopEvent) {
    HANDLE stop = static_cast<HANDLE>(stopEvent);
    wchar_t ownPath[MAX_PATH + 1] = {};
    GetModuleFileNameW(nullptr, ownPath, MAX_PATH);
    std::wstring dir = ownPath;
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir.erase(slash);

    wchar_t env[MAX_PATH + 1] = {};
    const std::wstring watch =
        (GetEnvironmentVariableW(L"DYNAMIC_ISLAND_INI", env, MAX_PATH) > 0)
            ? std::wstring(env)
            : dir + L"\\island.ini";

    FILETIME lastWrite = {};
    bool haveLast = false;
    for (;;) {
        const DWORD wait = WaitForSingleObject(stop, 1000);
        if (wait == WAIT_OBJECT_0) {
            return 0;
        }

        WIN32_FILE_ATTRIBUTE_DATA attrs = {};
        if (!GetFileAttributesExW(watch.c_str(), GetFileExInfoStandard,
                                  &attrs)) {
            haveLast = false;
            continue;
        }
        if (haveLast &&
            CompareFileTime(&attrs.ftLastWriteTime, &lastWrite) != 0) {
            WhTool_ModSettingsChanged();
            EnsureRunOnStartup();  // startup toggle applies live, not next boot
        }
        lastWrite = attrs.ftLastWriteTime;
        haveLast = true;
    }
}

// ── Loopback settings server ─────────────────────────────────────────────────
// settings.html opened from disk (file://) cannot write island.ini silently,
// which is why "save" meant download-and-replace. When the page is served from
// this tiny HTTP server instead, it is same-origin with island.ini: it loads
// the file automatically and POSTs edits straight back to disk, where the
// watcher above applies them within ~1 s. Bound to 127.0.0.1 only, on an
// OS-assigned port; only /settings.html, /island.ini and /ping are served.

std::atomic<unsigned> g_httpPort{0};

std::string Utf8FromWide(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                            out.data(), n, nullptr, nullptr);
    }
    return out;
}

std::wstring WideFromUtf8(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            out.data(), n);
    }
    return out;
}

std::wstring WideFromBytes(const std::string& s) {  // ANSI -> wide
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_ACP, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()),
                            out.data(), n);
    }
    return out;
}

std::wstring ServerExeDir() {
    wchar_t path[MAX_PATH + 1] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        dir.erase(slash);
    }
    return dir;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    BOOL ok = GetFileSizeEx(h, &size);
    if (ok && size.QuadPart > 0 && size.QuadPart <= 8 * 1024 * 1024) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read,
                      nullptr) != FALSE;
        if (ok) {
            out.resize(read);
        }
    } else if (ok) {
        out.clear();  // empty or oversized: treat as missing
        ok = FALSE;
    }
    CloseHandle(h);
    return ok != FALSE;
}

// island.ini on disk is UTF-16LE (first-run default) or plain ANSI. Serve UTF-8
// so fetch().text() in the page decodes it correctly either way.
std::string IniToUtf8(const std::string& bytes) {
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring wide((bytes.size() - 2) / 2, L'\0');
        if (!wide.empty()) {
            memcpy(wide.data(), bytes.data() + 2, wide.size() * 2);
        }
        return Utf8FromWide(wide);
    }
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return bytes.substr(3);  // already UTF-8, just drop the BOM
    }
    return Utf8FromWide(WideFromBytes(bytes));
}

// The page POSTs UTF-8; store it the way the island reads best (UTF-16LE with
// BOM, matching the first-run default file).
bool WriteIniFromUtf8(const std::string& utf8) {
    const std::wstring wide = WideFromUtf8(utf8);
    HANDLE h = CreateFileW(islandsa::IniPath(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(h, &bom, sizeof(bom), &written, nullptr);
    if (!wide.empty()) {
        WriteFile(h, wide.data(),
                  static_cast<DWORD>(wide.size() * sizeof(wchar_t)),
                  &written, nullptr);
    }
    CloseHandle(h);
    return true;
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

bool ReadHttpRequest(SOCKET s, HttpRequest& req) {
    DWORD timeoutMs = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    std::string data;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    for (int i = 0; i < 64; ++i) {  // bounded: header block <= ~256 KB
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        data.append(buf, static_cast<size_t>(n));
        headerEnd = data.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            break;
        }
        if (data.size() > 16 * 1024) {
            break;
        }
    }
    if (headerEnd == std::string::npos) {
        return false;
    }

    const size_t lineEnd = data.find("\r\n");
    if (lineEnd == std::string::npos) {
        return false;
    }
    const std::string line = data.substr(0, lineEnd);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        return false;
    }
    req.method = line.substr(0, sp1);
    req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t query = req.path.find('?');
    if (query != std::string::npos) {
        req.path.erase(query);
    }

    size_t contentLength = 0;
    std::string headers = data.substr(0, headerEnd);
    for (char& c : headers) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    const std::string needle = "content-length:";
    const size_t p = headers.find(needle);
    if (p != std::string::npos) {
        contentLength =
            static_cast<size_t>(strtoul(headers.c_str() + p + needle.size(),
                                        nullptr, 10));
    }
    if (contentLength > 2 * 1024 * 1024) {
        return false;  // island.ini is far smaller than this
    }

    std::string body = data.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        body.append(buf, static_cast<size_t>(n));
    }
    if (body.size() > contentLength) {
        body.resize(contentLength);
    }
    req.body = std::move(body);
    return true;
}

void SendAll(SOCKET s, const char* data, size_t len) {
    while (len > 0) {
        const int n = send(s, data, static_cast<int>(len), 0);
        if (n <= 0) {
            return;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

void Respond(SOCKET s, const char* status, const char* contentType,
             const std::string& body) {
    std::string head = std::string("HTTP/1.1 ") + status + "\r\n"
                       "Content-Type: " + contentType + "\r\n"
                       "Content-Length: " + std::to_string(body.size()) +
                       "\r\n"
                       "Cache-Control: no-store\r\n"
                       "Connection: close\r\n\r\n";
    SendAll(s, head.data(), head.size());
    SendAll(s, body.data(), body.size());
}

void HandleSettingsClient(SOCKET client) {
    HttpRequest req;
    if (!ReadHttpRequest(client, req)) {
        Respond(client, "400 Bad Request", "text/plain; charset=utf-8",
                "bad request");
        return;
    }

    if (req.path == "/ping") {
        Respond(client, "200 OK", "text/plain; charset=utf-8",
                "island-settings-server");
        return;
    }

    if (req.path == "/island.ini") {
        if (req.method == "POST") {
            if (WriteIniFromUtf8(req.body)) {
                WhTool_ModSettingsChanged();  // apply now, not at ~1 s
                Respond(client, "200 OK", "text/plain; charset=utf-8",
                        "saved");
            } else {
                Respond(client, "500 Internal Server Error",
                        "text/plain; charset=utf-8", "write failed");
            }
            return;
        }
        std::string bytes;
        if (ReadFileBytes(islandsa::IniPath(), bytes)) {
            Respond(client, "200 OK", "text/plain; charset=utf-8",
                    IniToUtf8(bytes));
        } else {
            Respond(client, "404 Not Found", "text/plain; charset=utf-8", "");
        }
        return;
    }

    if (req.path == "/" || req.path == "/settings.html") {
        std::string bytes;
        if (ReadFileBytes(ServerExeDir() + L"\\settings.html", bytes)) {
            Respond(client, "200 OK", "text/html; charset=utf-8", bytes);
        } else {
            Respond(client, "404 Not Found", "text/plain; charset=utf-8",
                    "settings.html not found next to DynamicIsland.exe");
        }
        return;
    }

    Respond(client, "404 Not Found", "text/plain; charset=utf-8", "not found");
}

// Serves settings.html + island.ini on 127.0.0.1 until stopEvent is signalled.
DWORD WINAPI SettingsHttpThread(LPVOID stopEvent) {
    HANDLE stop = static_cast<HANDLE>(stopEvent);
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // OS-assigned port: never clashes, per-run
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 8) != 0) {
        closesocket(listener);
        WSACleanup();
        return 0;
    }
    int addrLen = sizeof(addr);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr),
                    &addrLen) == 0) {
        g_httpPort.store(ntohs(addr.sin_port));
    }

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        timeval tv = {0, 250 * 1000};  // poll the stop event every 250 ms
        const int ready = select(0, &fds, nullptr, nullptr, &tv);
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (ready <= 0 || !FD_ISSET(listener, &fds)) {
            continue;
        }
        sockaddr_in peer = {};
        int peerLen = sizeof(peer);
        SOCKET client =
            accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == INVALID_SOCKET) {
            continue;
        }
        HandleSettingsClient(client);
        closesocket(client);
    }

    closesocket(listener);
    g_httpPort.store(0);
    WSACleanup();
    return 0;
}

}  // namespace

namespace islandsa {
unsigned HttpPort() {
    return g_httpPort.load();
}
}  // namespace islandsa

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Command-line control plane.
    if (ArgContains(L"--quit")) {
        HANDLE quitEvent =
            CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);
        if (quitEvent) {
            SetEvent(quitEvent);
            CloseHandle(quitEvent);
        }
        return 0;
    }
    if (ArgContains(L"--log")) {
        SetEnvironmentVariableW(L"DYNAMIC_ISLAND_LOG", L"1");
    }

    // Single instance.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 2;
    }

    HANDLE quitEvent = CreateEventW(nullptr, TRUE, FALSE, kQuitEventName);
    if (!quitEvent) {
        return 3;
    }

    // Crisp per-monitor DPI for the island window.
    if (HMODULE user = GetModuleHandleW(L"user32.dll")) {
        using SetCtx_t = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setCtx = reinterpret_cast<SetCtx_t>(
            GetProcAddress(user, "SetProcessDpiAwarenessContext"));
        if (setCtx) {
            setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    if (!WhTool_ModInit()) {
        if (quitEvent) CloseHandle(quitEvent);
        if (mutex) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
        return 1;
    }

    EnsureRunOnStartup();  // register/refresh/clear the Windows startup entry

    HANDLE watcherStop =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (watcherStop) {
        HANDLE thread = CreateThread(nullptr, 0, SettingsWatcherThread,
                                     watcherStop, 0, nullptr);
        if (thread) {
            CloseHandle(thread);  // we only signal via the event
        } else {
            SetEvent(watcherStop);
            CloseHandle(watcherStop);
            watcherStop = nullptr;
        }
    }

    // Loopback settings server: lets settings.html save island.ini with one
    // click (Open Settings prefers http://127.0.0.1:<port>/settings.html).
    HANDLE httpStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (httpStop) {
        HANDLE httpThread = CreateThread(nullptr, 0, SettingsHttpThread,
                                         httpStop, 0, nullptr);
        if (httpThread) {
            CloseHandle(httpThread);  // we only signal via the event
        } else {
            SetEvent(httpStop);
            CloseHandle(httpStop);
            httpStop = nullptr;
        }
    }

    // Ctrl+Alt+Shift+X quits.
    RegisterHotKey(nullptr, 1, kQuitHotkeyMods, kQuitHotkeyVk);

    bool quit = false;
    MSG msg = {};
    while (!quit) {
        const DWORD wait = MsgWaitForMultipleObjectsEx(
            1, &quitEvent, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) {
            quit = true;  // --quit was requested by another instance
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_HOTKEY && msg.wParam == 1) {
                quit = true;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (watcherStop) {
        SetEvent(watcherStop);
    }
    if (httpStop) {
        SetEvent(httpStop);  // server loop exits within ~250 ms
    }
    UnregisterHotKey(nullptr, 1);

    WhTool_ModUninit();  // posts WM_CLOSE to the island, joins every thread

    if (watcherStop) {
        CloseHandle(watcherStop);
    }
    if (httpStop) {
        CloseHandle(httpStop);
    }
    if (quitEvent) {
        CloseHandle(quitEvent);
    }
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
