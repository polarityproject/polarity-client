#pragma once
// Whether the loader proxy loads polarity.dll into this game. It loads
// nothing, and only forwards XInput, in two cases:
//
// - SFV Ember's launcher started the game. It injects sfv_ember.dll and passes
//   -sfve_loader=launcher. Ember hooks battle functions that polarity.dll
//   checks before it hooks anything, so polarity.dll would go inert anyway,
//   but its crash handler would still be in and would file Ember's crashes
//   as Polarity's.
// - The player chose plain SFV: [loader] load=none in polarity.ini.

#include <windows.h>

namespace sfv::proxy {

// True when `word` is one whole argument of `line`, space- or tab-separated.
inline bool HasArgument(const wchar_t* line, const wchar_t* word) {
    if (!line) return false;
    const int n = lstrlenW(word);
    const int length = lstrlenW(line);
    for (int i = 0; i + n <= length; i++) {
        const bool starts = i == 0 || line[i - 1] == L' ' || line[i - 1] == L'\t';
        const bool ends = line[i + n] == 0 || line[i + n] == L' ' || line[i + n] == L'\t';
        if (starts && ends && CompareStringOrdinal(line + i, n, word, n, FALSE) == CSTR_EQUAL) return true;
    }
    return false;
}

inline bool StartedByEmber() {
    // The DLL check covers an Ember that renames its flag but not its DLL.
    return HasArgument(GetCommandLineW(), L"-sfve_loader=launcher") || GetModuleHandleW(L"sfv_ember.dll");
}

inline bool LoadsPolarity(const wchar_t* ini) {
    if (StartedByEmber()) return false;
    wchar_t load[16];
    GetPrivateProfileStringW(L"loader", L"load", L"polarity", load, 16, ini);
    return lstrcmpiW(load, L"none") != 0;
}

} // namespace sfv::proxy
