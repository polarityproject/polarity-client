#pragma once
// Shared by the loader proxies: a thread loads polarity.dll from the proxy's
// own folder and reloads it on request; sfv_rollback.dll, SFVR's name, when
// there is no polarity.dll.
//
// The mod is loaded from a copy (polarity.live.<instance>.<n>.dll) so the
// build can overwrite polarity.dll while the game runs. Creating the file
// sfv_reload (instance 0) or sfv_reload.<instance> makes the loader call the
// mod's SfvShutdown export, unload it, and load a fresh copy, unless the
// mod's SfvCanUnload export says no (a netplay session is on): then the
// request is dropped.
//
// Several game instances can share the folder: each takes the lowest free
// instance number (named mutex) and tells the mod through SFV_INSTANCE. With
// [mod] multi_instance=1 in polarity.ini (sfv_rollback.ini until the mod has
// moved it) the game's own single-instance
// check (a mutex named "StreetFighterV") is made per process. Two instances
// write the same save files, so this is for testing only.
//
// A game that SFV Ember's launcher started, or [loader] load=none, loads
// nothing (loader_choice.h).

#include "proxy/loader_choice.h"

#include <windows.h>

namespace sfv::proxy {

inline wchar_t g_dir[MAX_PATH];
inline int g_instance = 0;
inline decltype(&CreateMutexW) g_realCreateMutexW = nullptr;

inline HANDLE WINAPI CreateMutexWHook(LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name)
{
    if (name && lstrcmpW(name, L"StreetFighterV") == 0) {
        wchar_t unique[64];
        wsprintfW(unique, L"StreetFighterV-%lu", GetCurrentProcessId());
        return g_realCreateMutexW(attributes, owner, unique);
    }
    return g_realCreateMutexW(attributes, owner, name);
}

// Points the exe's CreateMutexW import at the hook above.
inline void AllowMultipleInstances()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; desc++) {
        if (lstrcmpiA(reinterpret_cast<char*>(base + desc->Name), "KERNEL32.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (lstrcmpA(reinterpret_cast<char*>(byName->Name), "CreateMutexW") != 0) continue;
            DWORD old;
            if (VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old)) {
                g_realCreateMutexW = reinterpret_cast<decltype(&CreateMutexW)>(slots->u1.Function);
                slots->u1.Function = reinterpret_cast<ULONGLONG>(&CreateMutexWHook);
                VirtualProtect(&slots->u1.Function, sizeof(ULONGLONG), old, &old);
            }
            return;
        }
    }
}

inline bool Exists(const wchar_t* path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

// SFVR's loader made sfv_rollback.live.<instance>.<n>.dll copies; any a crash
// of the last SFVR run left behind are deleted here (one still loaded by
// another game stays: Windows refuses to delete it).
inline void DeleteOldLiveCopies()
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    wsprintfW(pattern, L"%ssfv_rollback.live.*.dll", g_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (lstrlenW(g_dir) + lstrlenW(fd.cFileName) >= MAX_PATH) continue;
        wsprintfW(path, L"%s%s", g_dir, fd.cFileName);
        DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

inline DWORD WINAPI LoadMod(LPVOID)
{
    wchar_t source[MAX_PATH], trigger[MAX_PATH], live[MAX_PATH];
    DeleteOldLiveCopies();
    wsprintfW(source, L"%spolarity.dll", g_dir);
    if (!Exists(source)) wsprintfW(source, L"%ssfv_rollback.dll", g_dir);
    if (g_instance == 0) wsprintfW(trigger, L"%ssfv_reload", g_dir);
    else wsprintfW(trigger, L"%ssfv_reload.%d", g_dir, g_instance);

    HMODULE mod = nullptr;
    for (int generation = 0;; generation++) {
        wsprintfW(live, L"%spolarity.live.%d.%d.dll", g_dir, g_instance, generation);
        DeleteFileW(live);
        mod = CopyFileW(source, live, FALSE) ? LoadLibraryW(live) : nullptr;

        for (;;) {
            while (GetFileAttributesW(trigger) == INVALID_FILE_ATTRIBUTES) Sleep(250);
            DeleteFileW(trigger);
            using CanUnloadFn = bool (*)();
            auto canUnload = mod ? reinterpret_cast<CanUnloadFn>(GetProcAddress(mod, "SfvCanUnload")) : nullptr;
            if (!canUnload || canUnload()) break;
        }

        if (mod) {
            using ShutdownFn = void (*)();
            if (auto shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(mod, "SfvShutdown"))) shutdown();
            FreeLibrary(mod);
            DeleteFileW(live);
        }
    }
}

// After an update the mod starts the game again with POLARITY_WAIT_PID set
// to the old game's process (SFVR_WAIT_PID from SFVR's updater, which
// installs the first Polarity build): wait here (before the game's
// single-instance check) until it has closed, at most a minute.
inline void WaitForRestartedGame()
{
    wchar_t text[32];
    DWORD n = GetEnvironmentVariableW(L"POLARITY_WAIT_PID", text, 32);
    if (!n || n >= 32) n = GetEnvironmentVariableW(L"SFVR_WAIT_PID", text, 32);
    SetEnvironmentVariableW(L"POLARITY_WAIT_PID", nullptr);
    SetEnvironmentVariableW(L"SFVR_WAIT_PID", nullptr);
    if (!n || n >= 32) return;
    DWORD pid = 0;
    for (DWORD i = 0; i < n && text[i] >= L'0' && text[i] <= L'9'; i++) pid = pid * 10 + (text[i] - L'0');
    if (!pid) return;
    if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(old, 60000);
        CloseHandle(old);
    }
    SetEnvironmentVariableW(L"POLARITY_RESTARTED", L"1");
}

inline void Start(HMODULE self)
{
    DWORD n = GetModuleFileNameW(self, g_dir, MAX_PATH);
    while (n > 0 && g_dir[n - 1] != L'\\' && g_dir[n - 1] != L'/') n--;
    g_dir[n] = 0;
    if (n + 40 >= MAX_PATH) return;
    wchar_t ini[MAX_PATH];
    wsprintfW(ini, L"%spolarity.ini", g_dir);
    if (!Exists(ini)) wsprintfW(ini, L"%ssfv_rollback.ini", g_dir);
    // Before anything below changes the game: a game without Polarity is
    // left exactly as it would be without this proxy.
    if (!LoadsPolarity(ini)) return;

    // Tells the mod that Polarity's loader runs: only then does it delete
    // SFVR's DLL (migrate.h). SFVR's loader loads nothing else.
    SetEnvironmentVariableW(L"POLARITY_LOADER", L"1");
    WaitForRestartedGame();

    for (g_instance = 0; g_instance < 8; g_instance++) {
        wchar_t name[64];
        wsprintfW(name, L"Local\\polarity_instance_%d", g_instance);
        HANDLE m = CreateMutexW(nullptr, TRUE, name);
        if (m && GetLastError() != ERROR_ALREADY_EXISTS) break; // kept open for the process lifetime
        if (m) CloseHandle(m);
    }
    wchar_t value[8];
    wsprintfW(value, L"%d", g_instance);
    SetEnvironmentVariableW(L"SFV_INSTANCE", value);

    if (GetPrivateProfileIntW(L"mod", L"multi_instance", 0, ini)) AllowMultipleInstances();

    if (HANDLE t = CreateThread(nullptr, 0, LoadMod, nullptr, 0, nullptr)) CloseHandle(t);
}

} // namespace sfv::proxy

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        sfv::proxy::Start(module);
    }
    return TRUE;
}
