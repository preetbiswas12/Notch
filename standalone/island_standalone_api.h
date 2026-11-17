#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// island_standalone_api.h
//
// Standalone replacement for Windhawk's windhawk_api.h. Force-included at the
// top of dynamic-island-for-windows.wh.cpp when building DynamicIsland.exe
// (clang++ -include island_standalone_api.h), so the mod compiles unmodified
// without any Windhawk dependency.
//
// Implements the exact Wh_* API surface the mod uses:
//   Wh_Log                      → OutputDebugStringW (+ optional file log)
//   Wh_GetIntSetting            → island.ini next to the exe, then the baked-in
//                                 YAML defaults (island_defaults.h)
//   Wh_GetStringSetting / Free  → same lookup chain
//   Wh_GetIntValue / SetIntValue→ storage\state.ini (persists across runs)
//   Wh_GetModStoragePath        → <base>\storage directory (created on demand)
//   Wh_SetFunctionHook          → stub (the island hooks nothing in this build)
//
// Base directory: the exe's own folder when writable (portable install),
// otherwise %LOCALAPPDATA%\DynamicIsland. island.ini is created on first run
// from the baked-in defaults so every setting is ready to hand-edit.
//
// The mod entry points Wh_ModInit / Wh_ModAfterInit / Wh_ModUninit still
// compile (they reference WH_MOD_ID) but are never executed: island_main.cpp
// drives the mod directly through WhTool_ModInit / WhTool_ModSettingsChanged /
// WhTool_ModUninit, which contain the entire real lifecycle.
// ─────────────────────────────────────────────────────────────────────────────

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdarg>
#include <cwchar>
#include <new>
#include <string>

#include "island_defaults.h"

#ifndef WH_MOD_ID
#define WH_MOD_ID L"dynamic-island-for-windows"
#endif
#ifndef WH_MOD_VERSION
#define WH_MOD_VERSION L"1.4.0"
#endif

#ifndef ISLAND_STANDALONE
#error "island_standalone_api.h is only meant for the standalone EXE build"
#endif

namespace islandsa {

// Sentinel GetPrivateProfileIntW result that means "key not present".
constexpr UINT kIniMissing = 0x7FFFFFFFu;

inline bool g_dirsReady = false;
inline std::wstring g_baseDir;
inline std::wstring g_iniPath;
inline std::wstring g_statePath;
inline std::wstring g_storagePath;
inline CRITICAL_SECTION g_valueCs;

// ── Base directory / paths ───────────────────────────────────────────────────

inline std::wstring OwnExeDir() {
    wchar_t path[MAX_PATH + 1] = {};
    UINT n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) {
        return L"C:\\ProgramData\\DynamicIsland";
    }
    std::wstring dir = path;
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        dir.erase(slash);
    }
    return dir;
}

inline void MakeDir(const std::wstring& dir) {
    std::wstring built;
    size_t pos = 0;
    while (pos <= dir.size()) {
        const size_t slash = dir.find(L'\\', pos);
        const size_t end = (slash == std::wstring::npos) ? dir.size() : slash;
        if (end > pos) {
            built = dir.substr(0, end);
            CreateDirectoryW(built.c_str(), nullptr);  // succeeds or exists
        }
        if (slash == std::wstring::npos) {
            break;
        }
        pos = slash + 1;
    }
}

// A directory is considered writable when a temp file can be created in it.
inline bool DirWritable(const std::wstring& dir) {
    std::wstring probe = dir + L"\\~island_probe.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

inline void EnsurePaths() {
    if (g_dirsReady) {
        return;
    }
    InitializeCriticalSection(&g_valueCs);

    // Optional overrides for power users.
    wchar_t env[MAX_PATH + 1] = {};
    if (GetEnvironmentVariableW(L"DYNAMIC_ISLAND_INI", env, MAX_PATH) > 0) {
        g_iniPath = env;
    }
    if (GetEnvironmentVariableW(L"DYNAMIC_ISLAND_DIR", env, MAX_PATH) > 0) {
        g_baseDir = env;
    }

    if (g_baseDir.empty()) {
        const std::wstring exeDir = OwnExeDir();
        if (DirWritable(exeDir)) {
            g_baseDir = exeDir;  // portable install
        } else {
            wchar_t local[MAX_PATH + 1] = {};
            const DWORD localLen =
                GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            g_baseDir = (localLen > 0 ? std::wstring(local)
                                      : std::wstring(L"C:")) +
                        L"\\DynamicIsland";
        }
    }

    MakeDir(g_baseDir);
    g_storagePath = g_baseDir + L"\\storage";
    MakeDir(g_storagePath);
    if (g_iniPath.empty()) {
        g_iniPath = g_baseDir + L"\\island.ini";
    }
    g_statePath = g_storagePath + L"\\state.ini";

    g_dirsReady = true;
}

inline const wchar_t* IniPath() {
    EnsurePaths();
    return g_iniPath.c_str();
}

inline const wchar_t* StatePath() {
    EnsurePaths();
    return g_statePath.c_str();
}

// ── First-run island.ini from the baked-in defaults ──────────────────────────

inline void WriteDefaultIni() {
    EnsurePaths();
    if (GetFileAttributesW(g_iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;  // keep user's file
    }

    std::wstring text = L"\ufeff[island]\r\n"
                        L"; Dynamic Island for Windows - standalone settings\r\n"
                        L"; Every line below is a default; edit and save, the\r\n"
                        L"; island reloads settings live (up to ~1 s).\r\n"
                        L"; Boolean: 1 = on, 0 = off.\r\n\r\n";
    for (size_t i = 0; i < kIslandDefaultCount; ++i) {
        const IslandDefaultEntry& d = kIslandDefaults[i];
        if (d.isInt) {
            wchar_t num[16] = {};
            swprintf(num, 16, L"%d", d.intVal);
            text += std::wstring(d.name) + L"=" + num + L"\r\n";
        } else {
            text += std::wstring(d.name) + L"=" + d.strVal + L"\r\n";
        }
    }

    HANDLE h = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    DWORD written = 0;
    WriteFile(h, text.c_str(), bytes, &written, nullptr);
    CloseHandle(h);
}

// ── Defaults lookup ──────────────────────────────────────────────────────────

inline const IslandDefaultEntry* FindDefault(PCWSTR name) {
    for (size_t i = 0; i < kIslandDefaultCount; ++i) {
        if (wcscmp(kIslandDefaults[i].name, name) == 0) {
            return &kIslandDefaults[i];
        }
    }
    return nullptr;
}

// ── Logging ──────────────────────────────────────────────────────────────────

inline void LogLine(const std::wstring& line) {
    OutputDebugStringW(line.c_str());
    OutputDebugStringW(L"\n");

    wchar_t flag[8] = {};
    static bool wantFileLog =
        GetEnvironmentVariableW(L"DYNAMIC_ISLAND_LOG", flag, 8) > 0;
    if (!wantFileLog) {
        return;
    }
    EnsurePaths();
    static CRITICAL_SECTION logCs = {};
    static bool logCsReady = false;
    {
        // One-time safe init without a race: several threads may log at boot.
        if (!logCsReady) {
            static SRWLOCK initLock = SRWLOCK_INIT;
            AcquireSRWLockExclusive(&initLock);
            if (!logCsReady) {
                InitializeCriticalSection(&logCs);
                logCsReady = true;
            }
            ReleaseSRWLockExclusive(&initLock);
        }
    }
    EnterCriticalSection(&logCs);
    HANDLE h = CreateFileW((g_baseDir + L"\\island.log").c_str(),
                           FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line.c_str(),
                  static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written,
                  nullptr);
        WriteFile(h, L"\r\n", 2, &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&logCs);
}

}  // namespace islandsa

// ─────────────────────────────────────────────────────────────────────────────
// Wh_* API — the mod-facing surface (signatures match windhawk_api.h)
// ─────────────────────────────────────────────────────────────────────────────

inline void Wh_Log(PCWSTR message, ...) {
    wchar_t text[2048] = {};
    va_list args;
    va_start(args, message);
    vswprintf(text, 2047, message, args);
    va_end(args);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t line[2100] = {};
    swprintf(line, 2099, L"[%02d:%02d:%02d] %s", st.wHour, st.wMinute,
             st.wSecond, text);
    islandsa::LogLine(line);
}

inline int Wh_GetIntSetting(PCWSTR valueName, ...) {
    // The mod only ever passes a plain key, so extra varargs (accepted for
    // signature parity with Windhawk's printf-style placeholder API) stay
    // unused here.
    va_list consumed;
    va_start(consumed, valueName);
    va_end(consumed);

    const UINT ini = GetPrivateProfileIntW(L"island", valueName,
                                           islandsa::kIniMissing,
                                           islandsa::IniPath());
    if (ini != islandsa::kIniMissing) {
        return static_cast<int>(ini);
    }
    if (const IslandDefaultEntry* d = islandsa::FindDefault(valueName)) {
        if (d->isInt) {
            return d->intVal;
        }
        return static_cast<int>(wcstol(d->strVal, nullptr, 10));
    }
    return 0;
}

inline PCWSTR Wh_GetStringSetting(PCWSTR valueName, ...) {
    va_list consumed;
    va_start(consumed, valueName);
    va_end(consumed);

    wchar_t buf[1024] = {};
    const DWORD n = GetPrivateProfileStringW(L"island", valueName, L"", buf,
                                             1024, islandsa::IniPath());
    if (n > 0 && buf[0] != L'\0') {
        const size_t len = n;
        wchar_t* heap = new wchar_t[len + 1];
        wmemcpy(heap, buf, len);
        heap[len] = L'\0';
        return heap;
    }

    // Missing or empty: fall back to the baked-in default, like the Windhawk
    // settings store does for freshly installed mods.
    if (const IslandDefaultEntry* d = islandsa::FindDefault(valueName)) {
        if (!d->isInt && d->strVal[0] != L'\0') {
            const size_t len = wcslen(d->strVal);
            wchar_t* heap = new wchar_t[len + 1];
            wmemcpy(heap, d->strVal, len);
            heap[len] = L'\0';
            return heap;
        }
    }
    wchar_t* empty = new wchar_t[1];
    empty[0] = L'\0';
    return empty;
}

inline void Wh_FreeStringSetting(PCWSTR string) {
    delete[] const_cast<PWSTR>(string);
}

// ── Persistent storage values (island state across sessions) ────────────────

inline int Wh_GetIntValue(PCWSTR valueName, int defaultValue) {
    return static_cast<int>(
        GetPrivateProfileIntW(L"state", valueName,
                              static_cast<UINT>(defaultValue),
                              islandsa::StatePath()));
}

inline BOOL Wh_SetIntValue(PCWSTR valueName, int value) {
    wchar_t num[16] = {};
    swprintf(num, 16, L"%d", value);
    EnterCriticalSection(&islandsa::g_valueCs);
    const BOOL ok =
        WritePrivateProfileStringW(L"state", valueName, num, islandsa::StatePath());
    LeaveCriticalSection(&islandsa::g_valueCs);
    return ok;
}

inline size_t Wh_GetStringValue(PCWSTR valueName, PWSTR stringBuffer,
                                size_t bufferChars) {
    if (!stringBuffer || bufferChars == 0) {
        return 0;
    }
    stringBuffer[0] = L'\0';
    GetPrivateProfileStringW(L"state", valueName, L"", stringBuffer,
                             static_cast<DWORD>(bufferChars),
                             islandsa::StatePath());
    return wcslen(stringBuffer);
}

inline BOOL Wh_SetStringValue(PCWSTR valueName, PCWSTR value) {
    EnterCriticalSection(&islandsa::g_valueCs);
    const BOOL ok = WritePrivateProfileStringW(L"state", valueName, value,
                                               islandsa::StatePath());
    LeaveCriticalSection(&islandsa::g_valueCs);
    return ok;
}

inline BOOL Wh_DeleteValue(PCWSTR valueName) {
    EnterCriticalSection(&islandsa::g_valueCs);
    const BOOL ok = WritePrivateProfileStringW(L"state", valueName, nullptr,
                                               islandsa::StatePath());
    LeaveCriticalSection(&islandsa::g_valueCs);
    return ok;
}

inline size_t Wh_GetModStoragePath(PWSTR pathBuffer, size_t bufferChars) {
    islandsa::EnsurePaths();
    if (!pathBuffer || bufferChars == 0 ||
        islandsa::g_storagePath.size() >= bufferChars) {
        return 0;
    }
    wmemcpy(pathBuffer, islandsa::g_storagePath.c_str(),
            islandsa::g_storagePath.size());
    pathBuffer[islandsa::g_storagePath.size()] = L'\0';
    return islandsa::g_storagePath.size();
}

// The island never hooks host functions in the standalone build; keep the
// official ordering contract (register → apply) but treat it as a success.
inline BOOL Wh_SetFunctionHook(void* /*targetFunction*/, void* /*hookFunction*/,
                               void** /*originalFunction*/) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        Wh_Log(L"Wh_SetFunctionHook suppressed (standalone build)");
    }
    return TRUE;
}

inline BOOL Wh_RemoveFunctionHook(void* /*targetFunction*/) {
    return TRUE;
}

inline BOOL Wh_ApplyHookOperations() {
    return TRUE;
}

// One-time state-file bootstrap so GetPrivateProfileIntW sees a real file and
// the user can see where the island keeps its runtime values.
namespace islandsa {
// Port of the loopback settings server hosted by island_main.cpp (0 = not
// running). Defined in island_main.cpp; ShowSettingsDashboard uses it to open
// settings.html in server mode, where the page saves island.ini directly.
unsigned HttpPort();

inline void BootstrapStateFile() {
    EnsurePaths();
    if (GetFileAttributesW(g_statePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(g_statePath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const wchar_t header[] = L"\ufeff[state]\r\n";
            DWORD written = 0;
            WriteFile(h, header, static_cast<DWORD>(wcslen(header) * sizeof(wchar_t)),
                      &written, nullptr);
            CloseHandle(h);
        }
    }
}
}  // namespace islandsa
