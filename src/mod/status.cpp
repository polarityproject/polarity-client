// The game window's title: the instance and the mod's status (docs/local-input.md).

#include "mod/status.h"

#include <windows.h>

#include <algorithm>
#include <mutex>

namespace sfv::mod {

namespace {

constexpr int kClassChars = 64, kTitleChars = 256;
constexpr UINT kSetTextTimeoutMs = 200;

std::mutex g_mutex;
int g_instance = 0;
std::string g_text;

BOOL CALLBACK Apply(HWND w, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    wchar_t cls[kClassChars];
    GetClassNameW(w, cls, kClassChars);
    if (lstrcmpW(cls, L"UnrealWindow") != 0) return TRUE;

    std::string title = "StreetFighterV [sfv " + std::to_string(g_instance) + "]";
    if (!g_text.empty()) title += " " + g_text;
    std::wstring wide(title.size(), L'\0');
    wide.resize(static_cast<size_t>(std::max(0, MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()),
                                                                    wide.data(), static_cast<int>(wide.size())))));
    wchar_t current[kTitleChars] = {};
    InternalGetWindowText(w, current, kTitleChars);
    if (wide == current) return TRUE;
    SendMessageTimeoutW(w, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(wide.c_str()), SMTO_ABORTIFHUNG, kSetTextTimeoutMs, nullptr);
    return TRUE;
}

} // namespace

void StatusSetInstance(int instance) {
    std::lock_guard lock(g_mutex);
    g_instance = instance;
    EnumWindows(Apply, 0);
}

std::string StatusText() {
    std::lock_guard lock(g_mutex);
    return g_text;
}

int StatusInstance() {
    std::lock_guard lock(g_mutex);
    return g_instance;
}

void StatusRefresh() {
    std::lock_guard lock(g_mutex);
    EnumWindows(Apply, 0);
}

void StatusSet(const std::string& text) {
    std::lock_guard lock(g_mutex);
    if (text == g_text) return;
    g_text = text;
    EnumWindows(Apply, 0);
}

} // namespace sfv::mod
