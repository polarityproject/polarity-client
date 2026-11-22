// Local input: the game's input manager, the XInput hook, virtual pads and the window (docs/local-input.md).

#include "mod/local_input.h"

#include "mod/crash_report.h"
#include "mod/flow.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/overlay.h"
#include "mod/pacing.h"
#include "mod/status.h"
#include "proxy/real_xinput.h"

#include <MinHook.h>

#include <limits>

namespace sfv::mod {

namespace {

namespace inputmgr {
constexpr uint32_t kGlobal = 0x3A750C0;
constexpr uint32_t kPadsOff = 0x08;
constexpr uint32_t kUpdate = 0x4DC7D0;
constexpr uint8_t kUpdateBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55,
                                    0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
constexpr int kPads = 2;
constexpr uint32_t kPadStride = 0x200;
constexpr uint32_t kKeysDownOff = 0x04;
constexpr uint32_t kBattleValueOff = 0x14;
constexpr uint32_t kStickXOff = 0x20, kStickYOff = 0x24;
constexpr uint32_t kStateBeginOff = 0x04, kStateEndOff = 0x28;
constexpr uint32_t kKeyTableOff = 0x30;
} // namespace inputmgr

constexpr WORD kPauseButtons = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
constexpr ULONGLONG kScriptedExpiryMs = 500;
constexpr SHORT kStickThreshold = 16000;
constexpr BYTE kTriggerThreshold = 64;
constexpr ULONGLONG kTitleEveryMs = 1000;
constexpr ULONGLONG kPollReportEveryMs = 60000;
constexpr WPARAM kDevNodesChanged = 0x0007;

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using HwndFn = HWND(WINAPI*)();
using InputUpdateFn = uint64_t (*)(void* manager, float deltaSeconds);

struct VirtualPad {
    std::atomic<bool> connected{false};
    std::atomic<WORD> buttons{0};
    std::atomic<BYTE> lt{0}, rt{0};
    std::atomic<SHORT> lx{0}, ly{0};
    std::atomic<DWORD> packet{0};
    std::atomic<bool> scripted{false};
    std::atomic<ULONGLONG> scriptedAt{0};
    std::atomic<WORD> scriptedButtons{0};
    std::atomic<BYTE> scriptedRt{0};
};

const uintptr_t g_imageBase = ImageBase();
XInputGetStateFn g_origGetState = nullptr;
HwndFn g_origForeground = nullptr;
HwndFn g_origActive = nullptr;
HwndFn g_origFocus = nullptr;
InputUpdateFn g_origInputUpdate = nullptr;
WNDPROC g_origWndProc = nullptr;

VirtualPad g_pads[XUSER_MAX_COUNT];
std::atomic<uint64_t> g_polls[XUSER_MAX_COUNT];
std::atomic<PackedInput> g_padInput[XUSER_MAX_COUNT];
std::atomic<uint16_t> g_realPauseButtons[XUSER_MAX_COUNT] = {};
std::atomic<bool> g_blockPause{false};
std::atomic<bool> g_keyboardStart{false};
std::atomic<bool> g_isolatePads{false};
std::atomic<uint16_t> g_uiButtons{0};
std::atomic<uint64_t> g_uiButtonsUntil{0};

std::atomic<HWND> g_gameWindow{nullptr};
std::atomic<bool> g_alwaysActive{false};
std::atomic<bool> g_activePending{false};

std::atomic<PackedInput> g_mappedInput{0};
std::atomic<int> g_mappedLog{0};
std::atomic<uint32_t> g_gameKeys{0};
uint8_t g_padsKept[inputmgr::kPads][inputmgr::kStateEndOff - inputmgr::kStateBeginOff] = {};
bool g_padsCleared = false;

void KeepWindowActive();
void SubclassWindow();

HWND FindGameWindow() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND w, LPARAM out) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            wchar_t cls[64];
            GetClassNameW(w, cls, 64);
            if (pid == GetCurrentProcessId() && lstrcmpW(cls, L"UnrealWindow") == 0 && IsWindowVisible(w)) {
                *reinterpret_cast<HWND*>(out) = w;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

HWND WINAPI ForegroundDetour() {
    HookGuard guard;
    HWND w = g_gameWindow;
    return w ? w : g_origForeground();
}

HWND WINAPI ActiveDetour() {
    HookGuard guard;
    HWND w = g_gameWindow;
    return w ? w : g_origActive();
}

HWND WINAPI FocusDetour() {
    HookGuard guard;
    HWND w = g_gameWindow;
    return w ? w : g_origFocus();
}

uint8_t* ManagerPads() {
    auto* pads = *reinterpret_cast<uint8_t* const*>(g_imageBase + inputmgr::kGlobal + inputmgr::kPadsOff);
    return pads && IsReadable(pads, inputmgr::kPads * inputmgr::kPadStride) ? pads : nullptr;
}

uint32_t PadWord(const uint8_t* pads, int pad, uint32_t off) {
    return *reinterpret_cast<const uint32_t*>(pads + pad * inputmgr::kPadStride + off);
}

uint8_t* PadBytes(uint8_t* pads, int pad) { return pads + pad * inputmgr::kPadStride + inputmgr::kStateBeginOff; }

void RestorePads(uint8_t* pads) {
    for (int pad = 0; pad < inputmgr::kPads; pad++) std::memcpy(PadBytes(pads, pad), g_padsKept[pad], sizeof(g_padsKept[pad]));
}

void ClearPads(uint8_t* pads) {
    for (int pad = 0; pad < inputmgr::kPads; pad++) {
        std::memcpy(g_padsKept[pad], PadBytes(pads, pad), sizeof(g_padsKept[pad]));
        std::memset(PadBytes(pads, pad), 0, sizeof(g_padsKept[pad]));
    }
    g_padsCleared = true;
}

void LogMapped(const uint8_t* pads, PackedInput local) {
    if (g_mappedLog <= 0) return;
    g_mappedLog--;
    static uint32_t keysWere = 0;
    const uint32_t keys = PadWord(pads, 0, inputmgr::kKeysDownOff);
    if (local != g_mappedInput || keys != keysWere) {
        Log("mappedlog: pad 0 %08X, keys %08X, stick %08X %08X (pad 1 %08X, keys %08X)", local, keys,
            PadWord(pads, 0, inputmgr::kStickXOff), PadWord(pads, 0, inputmgr::kStickYOff),
            PadWord(pads, 1, inputmgr::kBattleValueOff), PadWord(pads, 1, inputmgr::kKeysDownOff));
    }
    keysWere = keys;
}

uint64_t InputUpdateDetour(void* self, float deltaSeconds) {
    HookGuard guard;
    PaceFrame();
    uint8_t* pads = ManagerPads();
    if (pads && g_padsCleared) RestorePads(pads);
    g_padsCleared = false;
    const uint64_t result = g_origInputUpdate(self, deltaSeconds);
    if (!pads) return result;
    const PackedInput local = PadWord(pads, 0, inputmgr::kBattleValueOff);
    LogMapped(pads, local);
    g_mappedInput = OverlayTakesInput() ? 0 : local;
    g_gameKeys = PadWord(pads, 0, inputmgr::kKeysDownOff) | PadWord(pads, 1, inputmgr::kKeysDownOff);
    if (g_isolatePads || OverlayTakesInput()) ClearPads(pads);
    return result;
}

PackedInput ToGameInput(const XINPUT_GAMEPAD& g) {
    PackedInput in = 0;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > kStickThreshold) in |= kUp;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -kStickThreshold) in |= kDown;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -kStickThreshold) in |= kLeft;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > kStickThreshold) in |= kRight;
    if (g.wButtons & XINPUT_GAMEPAD_X) in |= kLP;
    if (g.wButtons & XINPUT_GAMEPAD_Y) in |= kMP;
    if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) in |= kHP;
    if (g.wButtons & XINPUT_GAMEPAD_A) in |= kLK;
    if (g.wButtons & XINPUT_GAMEPAD_B) in |= kMK;
    if (g.bRightTrigger > kTriggerThreshold) in |= kHK;
    if (g.wButtons & XINPUT_GAMEPAD_START) in |= kStart;
    return in;
}

DWORD ComposeState(DWORD user, XINPUT_STATE* state) {
    DWORD result = g_origGetState(user, state);
    if (user >= XUSER_MAX_COUNT) return result;
    g_polls[user]++;
    g_realPauseButtons[user] = result == ERROR_SUCCESS ? static_cast<uint16_t>(state->Gamepad.wButtons & kPauseButtons) : 0;
    if (g_blockPause && result == ERROR_SUCCESS) state->Gamepad.wButtons &= ~kPauseButtons;

    VirtualPad& pad = g_pads[user];
    if (!pad.connected) return result;
    if (result != ERROR_SUCCESS) std::memset(state, 0, sizeof(*state));
    state->dwPacketNumber = pad.packet.load() + (result == ERROR_SUCCESS ? state->dwPacketNumber : 0);
    if (pad.scripted && GetTickCount64() - pad.scriptedAt > kScriptedExpiryMs) pad.scripted = false;
    if (pad.scripted) {
        state->Gamepad = {};
        state->Gamepad.wButtons = (pad.scriptedButtons.load() & (g_blockPause ? ~kPauseButtons : 0xFFFF)) | pad.buttons.load();
        state->Gamepad.bRightTrigger = pad.scriptedRt.load();
        return ERROR_SUCCESS;
    }
    state->Gamepad.wButtons |= pad.buttons.load();
    state->Gamepad.bLeftTrigger = std::max(state->Gamepad.bLeftTrigger, pad.lt.load());
    state->Gamepad.bRightTrigger = std::max(state->Gamepad.bRightTrigger, pad.rt.load());
    if (pad.lx) state->Gamepad.sThumbLX = pad.lx;
    if (pad.ly) state->Gamepad.sThumbLY = pad.ly;
    return ERROR_SUCCESS;
}

DWORD WINAPI GetStateDetour(DWORD user, XINPUT_STATE* state) {
    HookGuard guard;
    if (user == 0) FlowPumpGameThread();
    const DWORD result = ComposeState(user, state);
    if (user >= XUSER_MAX_COUNT) return result;
    if (result == ERROR_SUCCESS) {
        XINPUT_GAMEPAD pad = state->Gamepad;
        pad.wButtons |= g_realPauseButtons[user];
        OverlayPad(user, pad);
    }
    if (OverlayTakesInput() && result == ERROR_SUCCESS) state->Gamepad = {};
    XINPUT_GAMEPAD forMatch = state->Gamepad;
    if (!OverlayTakesInput()) forMatch.wButtons |= g_realPauseButtons[user] & XINPUT_GAMEPAD_START;
    g_padInput[user] = result == ERROR_SUCCESS ? ToGameInput(forMatch) : 0;
    if (user == 0 && result == ERROR_SUCCESS && GetTickCount64() < g_uiButtonsUntil) {
        state->Gamepad.wButtons |= g_uiButtons;
        static uint64_t loggedFor = 0;
        if (loggedFor != g_uiButtonsUntil) Log("control: buttons %04x on the game's pad input", g_uiButtons.load());
        loggedFor = g_uiButtonsUntil;
    }
    return result;
}

LRESULT CALLBACK ActiveWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    HookGuard guard;
    if (OverlayWndProc(w, msg, wp, lp)) {
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && GetCapture() == w) {
            ReleaseCapture();
            static bool told = false;
            if (!std::exchange(told, true)) Log("overlay: released the game's mouse capture (the title bar works again)");
        }
        return msg == WM_INPUT ? DefWindowProcW(w, msg, wp, lp) : 0;
    }
    const bool active = g_alwaysActive;
    switch (msg) {
        case WM_ACTIVATEAPP:
            if (active && !wp) return 0;
            break;
        case WM_ACTIVATE:
            if (active && LOWORD(wp) == WA_INACTIVE) return 0;
            break;
        case WM_KEYDOWN:
        case WM_KEYUP:
            if (g_blockPause && (wp == VK_RETURN || wp == VK_ESCAPE)) {
                if (wp == VK_RETURN) g_keyboardStart = msg == WM_KEYDOWN;
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            g_keyboardStart = false;
            if (active) return 0;
            break;
        case WM_CLOSE:
        case WM_ENDSESSION:
            CrashOnQuitting("window closed");
            NetLeave();
            break;
        default:
            break;
    }
    return CallWindowProcW(g_origWndProc, w, msg, wp, lp);
}

void SubclassWindow() {
    if (!g_gameWindow || g_origWndProc) return;
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ActiveWndProc)));
}

void KeepWindowActive() {
    g_alwaysActive = true;
    SubclassWindow();
    PostMessageW(g_gameWindow, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(g_gameWindow, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(g_gameWindow, WM_SETFOCUS, 0, 0);
}

bool HookFocusFunctions() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto hook = [&](const char* name, void* detour, HwndFn* original) {
        void* target = reinterpret_cast<void*>(GetProcAddress(user32, name));
        return target && MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK && MH_EnableHook(target) == MH_OK;
    };
    return hook("GetForegroundWindow", reinterpret_cast<void*>(&ForegroundDetour), &g_origForeground) &&
           hook("GetActiveWindow", reinterpret_cast<void*>(&ActiveDetour), &g_origActive) &&
           hook("GetFocus", reinterpret_cast<void*>(&FocusDetour), &g_origFocus);
}

void* GameImport(const char* module, WORD ordinal, const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(g_imageBase);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), module) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            const bool match = IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
                                   ? IMAGE_ORDINAL(names->u1.Ordinal) == ordinal
                                   : std::strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, name) == 0;
            if (match) return reinterpret_cast<void*>(iat->u1.Function);
        }
    }
    return nullptr;
}

bool HookXInput() {
    void* target = GameImport("xinput1_3.dll", 2, "XInputGetState");
    if (!target) {
        HMODULE xinput = sfv::proxy::RealXInput();
        target = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    }
    return target && MH_CreateHook(target, reinterpret_cast<void*>(&GetStateDetour), reinterpret_cast<void**>(&g_origGetState)) == MH_OK &&
           MH_EnableHook(target) == MH_OK;
}

bool HookInputManager() {
    auto* update = reinterpret_cast<uint8_t*>(g_imageBase + inputmgr::kUpdate);
    const bool ok = std::memcmp(update, inputmgr::kUpdateBytes, sizeof(inputmgr::kUpdateBytes)) == 0 &&
                    MH_CreateHook(update, reinterpret_cast<void*>(&InputUpdateDetour), reinterpret_cast<void**>(&g_origInputUpdate)) == MH_OK &&
                    MH_EnableHook(update) == MH_OK;
    if (!ok) g_origInputUpdate = nullptr;
    return ok;
}

void AskForDevices() {
    EnumWindows(
        [](HWND w, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(w)) PostMessageW(w, WM_DEVICECHANGE, kDevNodesChanged, 0);
            return TRUE;
        },
        0);
}

void LogPolls() {
    static uint64_t lastPolls[XUSER_MAX_COUNT] = {};
    char buf[128];
    int n = 0;
    for (int u = 0; u < XUSER_MAX_COUNT; u++) {
        const uint64_t p = g_polls[u];
        n += std::snprintf(buf + n, sizeof(buf) - n, " %llu", static_cast<unsigned long long>(p - lastPolls[u]));
        lastPolls[u] = p;
    }
    Log("xinput polls per 60 s by user:%s; %s", buf, FlowPumpStats().c_str());
}

} // namespace

bool InputInstall(int virtualPads, bool alwaysActive) {
    g_gameWindow = FindGameWindow();
    SubclassWindow();
    if (alwaysActive) {
        const bool ok = HookFocusFunctions();
        if (ok && g_gameWindow) KeepWindowActive();
        else if (ok) g_activePending = true;
        Log("control: always active %s (window %p)", !ok ? "FAILED" : g_gameWindow ? "on"
                                                                                   : "waiting for the window",
            g_gameWindow.load());
    }
    for (int u = 0; u < virtualPads && u < XUSER_MAX_COUNT; u++) g_pads[u].connected = true;
    if (!HookXInput()) {
        Log("control: cannot hook XInputGetState");
        return false;
    }
    if (!HookInputManager()) Log("control: cannot hook the input manager; netplay reads the pads with the default layout");
    AskForDevices();
    Log("control: virtual pads ready");
    return true;
}

void InputTick() {
    static ULONGLONG lastReport = GetTickCount64();
    static ULONGLONG lastTitle = 0;
    if (!g_gameWindow && !g_activePending) {
        if (HWND w = FindGameWindow()) {
            g_gameWindow = w;
            SubclassWindow();
        }
    }
    if (g_activePending) {
        if (HWND w = FindGameWindow()) {
            g_gameWindow = w;
            g_activePending = false;
            KeepWindowActive();
            RefreshStatus();
            Log("control: always active on (window %p)", w);
        }
    }
    if (!g_activePending && GetTickCount64() - lastTitle > kTitleEveryMs) {
        lastTitle = GetTickCount64();
        RefreshStatus();
    }
    if (GetTickCount64() - lastReport > kPollReportEveryMs) {
        lastReport = GetTickCount64();
        LogPolls();
    }
}

bool InputParseButtons(const std::string& spec, PadState* out) {
    static const std::pair<const char*, int> kButtons[] = {
        {"up", XINPUT_GAMEPAD_DPAD_UP}, {"down", XINPUT_GAMEPAD_DPAD_DOWN}, {"left", XINPUT_GAMEPAD_DPAD_LEFT}, {"right", XINPUT_GAMEPAD_DPAD_RIGHT}, {"start", XINPUT_GAMEPAD_START}, {"back", XINPUT_GAMEPAD_BACK}, {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"a", XINPUT_GAMEPAD_A}, {"b", XINPUT_GAMEPAD_B}, {"x", XINPUT_GAMEPAD_X}, {"y", XINPUT_GAMEPAD_Y}};
    std::stringstream ss(spec);
    std::string b;
    while (std::getline(ss, b, '+')) {
        const auto button = std::find_if(std::begin(kButtons), std::end(kButtons), [&b](const auto& k) { return b == k.first; });
        if (button != std::end(kButtons)) out->buttons |= static_cast<WORD>(button->second);
        else if (b == "lt") out->lt = std::numeric_limits<BYTE>::max();
        else if (b == "rt") out->rt = std::numeric_limits<BYTE>::max();
        else if (b == "sup") out->ly = std::numeric_limits<SHORT>::max();
        else if (b == "sdown") out->ly = std::numeric_limits<SHORT>::min();
        else if (b == "sleft") out->lx = std::numeric_limits<SHORT>::min();
        else if (b == "sright") out->lx = std::numeric_limits<SHORT>::max();
        else if (b != "none") return false;
    }
    return true;
}

void InputSetVirtual(int user, const PadState& state) {
    if (user < 0 || user >= XUSER_MAX_COUNT) return;
    VirtualPad& pad = g_pads[user];
    pad.connected = true;
    pad.buttons = state.buttons;
    pad.lt = state.lt;
    pad.rt = state.rt;
    pad.lx = state.lx;
    pad.ly = state.ly;
    pad.packet++;
}

void InputConnectVirtual(int user, bool connected) {
    if (user >= 0 && user < XUSER_MAX_COUNT) g_pads[user].connected = connected;
}

HWND InputGameWindow() {
    const HWND w = g_gameWindow;
    return w ? w : FindGameWindow();
}

HWND InputRealForegroundWindow() { return g_origForeground ? g_origForeground() : GetForegroundWindow(); }

void InputRestoreWindowProc() {
    if (g_origWndProc && g_gameWindow) {
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
    }
}

void InputBlockPause(bool block) {
    g_blockPause = block;
    if (!block) g_keyboardStart = false;
}

void InputPressUi(uint16_t buttons, int ms) {
    g_uiButtons = buttons;
    g_uiButtonsUntil = GetTickCount64() + ms;
}

void InputIsolatePads(bool isolate) { g_isolatePads = isolate; }

uint32_t InputGamePadKeys() { return g_gameKeys; }

PackedInput InputMappedLocal(int) {
    PackedInput start = g_keyboardStart ? kStart : 0;
    for (int u = 0; u < XUSER_MAX_COUNT; u++) start |= g_padInput[u].load() & kStart;
    if (!g_origInputUpdate) return g_padInput[0].load() | start;
    return g_mappedInput.load() | start;
}

void InputScriptedPad(int user, bool enabled, uint16_t buttons, uint8_t rightTrigger) {
    if (user < 0 || user >= XUSER_MAX_COUNT) return;
    VirtualPad& pad = g_pads[user];
    pad.scriptedButtons = buttons;
    pad.scriptedRt = rightTrigger;
    pad.scriptedAt = GetTickCount64();
    pad.scripted = enabled;
    pad.packet++;
}

uint64_t InputPadPolls(int user) { return user >= 0 && user < XUSER_MAX_COUNT ? g_polls[user].load() : 0; }

void InputLogMapped(int updates) { g_mappedLog = updates; }

uint32_t* InputKeyTable() {
    uint8_t* pads = ManagerPads();
    return pads ? reinterpret_cast<uint32_t*>(pads + inputmgr::kKeyTableOff) : nullptr;
}

} // namespace sfv::mod
