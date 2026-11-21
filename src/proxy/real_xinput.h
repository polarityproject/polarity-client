#pragma once
// The XInput the xinput1_3 proxy passes the game's calls to, and the mod
// hooks: System32's xinput1_3 on Windows. Under Wine (Proton, the Steam
// Deck) its xinput1_4 instead: the xinput1_3=n,b override that loads the
// proxy also makes System32's xinput1_3 load as "native", a copy that sees
// no pads (the game then had no pad input at all), while xinput1_4 loads as
// Wine's own.

#include <windows.h>

namespace sfv::proxy {

inline bool RunningOnWine() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version")) return true;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Wine", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

inline const wchar_t* RealXInputPath() {
    return RunningOnWine() ? L"xinput1_4.dll" : L"C:\\Windows\\System32\\xinput1_3.dll";
}

// Loaded once (and kept).
inline HMODULE RealXInput() {
    static HMODULE module = LoadLibraryW(RealXInputPath());
    return module;
}

// Where a call goes that came back into the proxy through the real XInput:
// System32's xinput1_4 on Windows (Wine's real one is that already: none).
// Special K hooks every XInput and takes the first xinput1_3 loaded, the
// proxy, for the original of System32's: proxy and hook called each other
// until the stack overflowed (a 0.2.1 crash report).
inline HMODULE FallbackXInput() {
    static HMODULE module = [] {
        if (RunningOnWine()) return HMODULE{};
        wchar_t path[MAX_PATH];
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 20) return HMODULE{};
        lstrcatW(path, L"\\xinput1_4.dll");
        return LoadLibraryW(path);
    }();
    return module;
}

} // namespace sfv::proxy
