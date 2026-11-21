#include "mod/control.h"

#include "core/random_stage.h"
#include "core/version.h"
#include "mod/benchmark.h"
#include "mod/battle_data.h"
#include "mod/battle_log_screen.h"
#include "mod/hub_screen.h"
#include "mod/rooms_screen.h"
#include "mod/settings_screen.h"
#include "mod/room_screen.h"
#include "mod/battle_driver.h"
#include "mod/frame_meter.h"
#include "mod/hitbox_display.h"
#include "mod/ui_kit.h"
#include "mod/crash_report.h"
#include "mod/files.h"
#include "mod/pacing.h"
#include "mod/report_upload.h"
#include "mod/updater.h"
#include "proxy/real_xinput.h"
#include "mod/flow.h"

#include "mod/lifecycle.h"
#include "mod/stack.h"
#include "mod/battle_setup.h"
#include "mod/camera_rollback.h"
#include "mod/hud_step.h"
#include "mod/boot.h"
#include "mod/weyve_service.h"
#include "mod/rooms_client.h"
#include "mod/sfv_game.h"
#include "mod/status.h"
#include "mod/watch.h"
#include "mod/log.h"
#include "mod/memscan.h"
#include "mod/ue4.h"
#include "mod/netplay_driver.h"
#include "mod/outside_probe.h"
#include "mod/overlay.h"
#include "mod/pak_textures.h"
#include "mod/textures.h"
#include "mod/replay_driver.h"
#include "mod/tracer.h"
#include "mod/ui_sound.h"
#include "mod/watch_chat.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d11.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

namespace sfv::mod {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

struct Step {
    enum Kind { Press,
                Wait,
                Connect,
                Disconnect,
                Game,
                Setup,
                ShowSetup,
                ClearSetup,
                Scan,
                Rescan,
                ScanShow,
                ScanSmall,
                ScanDelta,
                ScanRecord,
                Trace,
                TraceReport,
                Net } kind;

    std::string line;
    MatchSetup setup;
    std::vector<uint32_t> rvas;
    int user = 0;
    WORD buttons = 0;
    BYTE lt = 0, rt = 0;
    SHORT lx = 0, ly = 0; // the left stick
    int holdMs = 0;
    int gapMs = 0;
    GameRequest request;
};

struct VirtualPad {
    std::atomic<bool> connected{false};
    std::atomic<WORD> buttons{0};
    std::atomic<BYTE> lt{0}, rt{0};
    std::atomic<SHORT> lx{0}, ly{0};
    std::atomic<DWORD> packet{0};
    std::atomic<bool> scripted{false};
    std::atomic<ULONGLONG> scriptedAt{0}; // scripted state expires: a dead driver must not keep pressing
    std::atomic<WORD> scriptedButtons{0};
    std::atomic<BYTE> scriptedRt{0};
};

XInputGetStateFn g_origGetState = nullptr;
std::atomic<bool> g_blockPause{false};
std::atomic<uint16_t> g_uiButtons{0}; // PressUiButtons
std::atomic<uint64_t> g_uiButtonsUntil{0};
using HwndFn = HWND(WINAPI*)();
HwndFn g_origForeground = nullptr;
HwndFn g_origActive = nullptr;
HwndFn g_origFocus = nullptr;
std::atomic<HWND> g_gameWindow{nullptr};
// always_active was asked for but the window did not exist yet (cold start).
std::atomic<bool> g_activePending{false};
std::atomic<uint16_t> g_realStartBack[XUSER_MAX_COUNT] = {};
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

uintptr_t g_imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
VirtualPad g_pads[XUSER_MAX_COUNT];
std::atomic<uint64_t> g_polls[XUSER_MAX_COUNT];
std::mutex g_mutex;
std::deque<GameRequest> g_gameRequests;

std::atomic<bool> g_isolatePads{false};
std::atomic<PackedInput> g_padInput[XUSER_MAX_COUNT];
// The keyboard's Start (Enter) held down while the window procedure keeps it
// from the game (a netplay match: SetBlockPause).
std::atomic<bool> g_keyboardStart{false};

// The game's input manager (rva 0x3A750C0; its +8 holds two pads of 0x200
// bytes) maps the keyboard and the pads, with the player's button config,
// once per frame (rva 0x4DC7D0). Per pad: +0x4 the pad keys down (bit k =
// EKWKeyConfigPadKeyId k: X 0x10, Y 0x20, A 0x40, ... LB 0x100), +0x8
// pressed this frame, +0xC released, +0x14 the value the battle reads (rva
// 0x4CED60 via 0x4CEDF0; the InputUnit's values come from it, rva 0x443D20):
// the keys after the player's Button Settings, each function as the default
// layout's keys, shortcuts expanded (3P on LB: X+Y+RB). +0x20 / +0x24 the
// stick. The local input is +0x14, as the battle reads it offline: +0x4
// skipped the Button Settings, so shortcut keys and custom layouts (keyboard
// players have both) came out wrong online. The local player plays with pad 0
// (keyboard included). In a netplay match the battle must see none of it,
// only the injected inputs: the game read local input past the InputUnit and
// each machine saw its own player's pad (netplay-testing.md, finding 7), so
// the manager's pads are cleared right after each update, and put back just
// before the next one (the update derives pressed and held from them).
constexpr uint32_t kInputManager = 0x3A750C0;
constexpr uint32_t kInputManagerUpdate = 0x4DC7D0;
constexpr uint8_t kInputManagerUpdateBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55,
                                                0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
constexpr uint32_t kPadStride = 0x200;
constexpr uint32_t kPadDown = 0x04;
constexpr uint32_t kPadBattle = 0x14;
constexpr uint32_t kPadStateBegin = 0x04, kPadStateEnd = 0x28;
// The live Button Settings of a pad: what each pad key (EKWKeyConfigPadKeyId
// 0..15) gives as default-layout keys, shortcuts expanded (default: X 0x10,
// LB 0x230 = 3P). Built from the player's settings; `layout` rewrites it for tests.
constexpr uint32_t kPadKeyTable = 0x30;
using InputUpdateFn = uint64_t (*)(void* manager, float deltaSeconds);
InputUpdateFn g_origInputUpdate = nullptr;
std::atomic<PackedInput> g_mappedInput{0};
std::atomic<int> g_mappedLog{0}; // research (`mappedlog <updates>`): log the mapped value on change
// The two pads' state as the last update left it, before the clearing (game thread only).
uint8_t g_padsKept[2][kPadStateEnd - kPadStateBegin] = {};
bool g_padsCleared = false;
std::atomic<uint32_t> g_gameKeys{0}; // both pads' keys (kPadDown) as the last update made them

uint64_t InputUpdateDetour(void* self, float deltaSeconds) {
    HookGuard guard;
    // During a netplay match the frame's wait goes here, before the pads are
    // read, so it adds no input lag (pacing.h).
    PaceFrame();
    auto* pads = *reinterpret_cast<uint8_t* const*>(g_imageBase + kInputManager + 8);
    const bool padsOk = pads && IsReadable(pads, 2 * kPadStride);
    // Put back what the clearing took, so this update sees the last one's state.
    if (padsOk && g_padsCleared) {
        for (int pad = 0; pad < 2; pad++) std::memcpy(pads + pad * kPadStride + kPadStateBegin, g_padsKept[pad], sizeof(g_padsKept[pad]));
    }
    g_padsCleared = false;
    const uint64_t result = g_origInputUpdate(self, deltaSeconds);
    if (padsOk) {
        const PackedInput local = *reinterpret_cast<const uint32_t*>(pads + kPadBattle);
        if (g_mappedLog > 0) {
            g_mappedLog--;
            static uint32_t keysWere = 0;
            const uint32_t keys = *reinterpret_cast<const uint32_t*>(pads + kPadDown);
            if (local != g_mappedInput || keys != keysWere) {
                Log("mappedlog: pad 0 %08X, keys %08X, stick %08X %08X (pad 1 %08X, keys %08X)", local, keys,
                    *reinterpret_cast<const uint32_t*>(pads + 0x20), *reinterpret_cast<const uint32_t*>(pads + 0x24),
                    *reinterpret_cast<const uint32_t*>(pads + kPadStride + kPadBattle),
                    *reinterpret_cast<const uint32_t*>(pads + kPadStride + kPadDown));
            }
            keysWere = keys;
        }
        // While the overlay takes the input, the match gets none of it either
        // (an XInput pad is neutral by then; a pad read past XInput is not).
        g_mappedInput = OverlayTakesInput() ? 0 : local;
        // The pads' keys for the overlay, from any pad the game reads (XInput,
        // DirectInput, Steam Input): one PC's pad never went through XInput,
        // and the select screen had no pad there. While the overlay takes the
        // input, the game's menus behind it see none of it.
        g_gameKeys = *reinterpret_cast<const uint32_t*>(pads + kPadDown) | *reinterpret_cast<const uint32_t*>(pads + kPadStride + kPadDown);
        if (g_isolatePads || OverlayTakesInput()) {
            for (int pad = 0; pad < 2; pad++) {
                std::memcpy(g_padsKept[pad], pads + pad * kPadStride + kPadStateBegin, sizeof(g_padsKept[pad]));
                std::memset(pads + pad * kPadStride + kPadStateBegin, 0, kPadStateEnd - kPadStateBegin);
            }
            g_padsCleared = true;
        }
    }
    return result;
}

// The game's input value for a pad state, with the default button layout
// (type A: X LP, Y MP, RB HP, A LK, B MK, RT HK), d-pad or left stick.
PackedInput ToGameInput(const XINPUT_GAMEPAD& g) {
    constexpr SHORT kStick = 16000;
    PackedInput in = 0;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > kStick) in |= kUp;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -kStick) in |= kDown;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -kStick) in |= kLeft;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > kStick) in |= kRight;
    if (g.wButtons & XINPUT_GAMEPAD_X) in |= kLP;
    if (g.wButtons & XINPUT_GAMEPAD_Y) in |= kMP;
    if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) in |= kHP;
    if (g.wButtons & XINPUT_GAMEPAD_A) in |= kLK;
    if (g.wButtons & XINPUT_GAMEPAD_B) in |= kMK;
    if (g.bRightTrigger > 64) in |= kHK;
    if (g.wButtons & XINPUT_GAMEPAD_START) in |= kStart;
    return in;
}

DWORD ComposeState(DWORD user, XINPUT_STATE* state) {
    DWORD result = g_origGetState(user, state);
    if (user >= XUSER_MAX_COUNT) return result;
    g_polls[user]++;
    // Start and Back as the player pressed them (the pause filter below
    // removes them), for the overlay's Back + Start.
    g_realStartBack[user] = result == ERROR_SUCCESS
                                ? static_cast<uint16_t>(state->Gamepad.wButtons & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK))
                                : 0;

    // Real pads too: a pause on one side of a netplay match would desync it.
    if (g_blockPause && result == ERROR_SUCCESS) state->Gamepad.wButtons &= ~(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK);

    VirtualPad& pad = g_pads[user];
    if (!pad.connected) return result;
    if (result != ERROR_SUCCESS) std::memset(state, 0, sizeof(*state));
    state->dwPacketNumber = pad.packet.load() + (result == ERROR_SUCCESS ? state->dwPacketNumber : 0);
    if (pad.scripted && GetTickCount64() - pad.scriptedAt > 500) pad.scripted = false;
    if (pad.scripted) {
        state->Gamepad = {};
        // Pad steps (testing) still add their buttons, as on an unscripted pad.
        state->Gamepad.wButtons =
            (pad.scriptedButtons.load() & (g_blockPause ? ~(XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK) : 0xFFFF)) |
            pad.buttons.load();
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
    // The engine polls the pads on the game thread once per frame.
    if (user == 0) PumpGameThread();
    const DWORD result = ComposeState(user, state);
    if (user >= XUSER_MAX_COUNT) return result;
    // The overlay sees the pads as composed (real and virtual), with the real
    // Start and Back.
    if (result == ERROR_SUCCESS) {
        XINPUT_GAMEPAD pad = state->Gamepad;
        pad.wButtons |= g_realStartBack[user];
        OverlayPad(user, pad);
    }
    // The overlay's panel (or the post-match vote) is open: the game (and a
    // netplay match) gets neutral pads.
    if (OverlayTakesInput() && result == ERROR_SUCCESS) state->Gamepad = {};
    // With the real Start (the pause filter took it out of the state): a
    // netplay match passes it on only when both players pressed it.
    XINPUT_GAMEPAD forMatch = state->Gamepad;
    if (!OverlayTakesInput()) forMatch.wButtons |= g_realStartBack[user] & XINPUT_GAMEPAD_START;
    g_padInput[user] = result == ERROR_SUCCESS ? ToGameInput(forMatch) : 0;
    if (user == 0 && result == ERROR_SUCCESS && GetTickCount64() < g_uiButtonsUntil) {
        state->Gamepad.wButtons |= g_uiButtons;
        static uint64_t loggedFor = 0;
        if (loggedFor != g_uiButtonsUntil) Log("control: buttons %04x on the game's pad input", g_uiButtons.load());
        loggedFor = g_uiButtonsUntil;
    }
    return result;
}

// host [random] [costumes] | join <code> | spectate <code> | leave | ready [0|1]
// | choose rematch|select | netsim <latency ms> <jitter ms> <loss %> | mash <seed>
// | dumps <every N frames> [max] (state dumps in match recordings)
// | recordings full|compact (region hashes for every frame, or only around desyncs)
// | server [host:port] (the rooms server for the next host/join, this game only)
// | packlogs (the Polarity folder's logs and latest matches into a zip to share)
// | update [check [url] | install] (the updater: check the site, install what it offers)
// | pick <code costume color comment vskill vtrigger> | stage <n> | netstatus
// | name [text] (the name test sessions use; none: back to "Player <instance + 1>")
// | delay <frames> (this player's input delay for the next host/join)
// | replay <file> [stress <distance>] [watch <from> <to> <region>] | replay stop (plays a .sfvr back in the game, replay_driver.h)
// Host and join from commands are ready at once (scripts drive the lobby),
// and their player is named "Player <instance + 1>" (the panel's or Steam's
// name stays for sessions started by hand); `name` sets another, e.g. the
// second PC's instance 0 in a two-machine test.
bool ParseButtons(const std::string& spec, Step& step);

std::string g_testName; // from `name`; empty: "Player <instance + 1>"

std::string TestName() {
    return g_testName.empty() ? "Player " + std::to_string(StatusInstance() + 1) : g_testName;
}

// Research commands of the box display work (hitbox_display.h, battle_driver.h
// region research): kept out of RunNetCommand's chain, which is at the
// compiler's nesting limit. Returns whether `cmd` was one of them.
bool RunResearchCommand(const std::string& cmd, std::stringstream& ss) {
    if (cmd == "canceljump") {
        // canceljump: research (calls off the jump under way, as a session
        // going back to the lobby mid-jump does; `battlenow; canceljump`)
        CancelBattleJump();
        return true;
    }
    if (cmd == "wheel") {
        // wheel <x> <y> <notches>: the mouse wheel turned over a spot of the
        // mod's screens (design coordinates, 1600 x 900; up positive), next frame.
        float x = 0, y = 0, notches = 0;
        ss >> x >> y >> notches;
        ui::TestWheel(x, y, notches);
        Log("control: wheel %.0f,%.0f %+.1f", x, y, notches);
        return true;
    }
    if (cmd == "watchchat") {
        // watchchat toggle | type <text> | say <text>: the spectators' chat
        // box (test): show or hide it, start typing with that text, send it.
        std::string what, text;
        ss >> what;
        std::getline(ss >> std::ws, text);
        if (what == "toggle") WatchChatToggle();
        else if (what == "type" || what == "say") WatchChatTestText(text, what == "say");
        else Log("control: watchchat toggle | type <text> | say <text>");
        return true;
    }
    if (cmd == "watchname") {
        // watchname <region> <hex offset> <len>: research (a write watch on a region by name; `watchreport`)
        std::string name, off;
        int len = 4;
        ss >> name >> off >> len;
        const size_t offset = std::stoull(off.empty() ? "0" : off, nullptr, 16);
        WatchRegionNamed(name, offset, len); // from this thread: a watch covers every thread but the one arming it
    } else if (cmd == "watchphase") {
        // watchphase: research (a write watch on the phase machine's requested phase, +0x134; `watchreport`)
        uint8_t* m = PhaseMachineObject();
        if (!m) Log("watchphase: no phase machine");
        else Log("watchphase: %s", ArmWatch(1, m + 0x134, 4, g_imageBase) ? "armed on +0x134" : "not armed");
    } else if (cmd == "camrollback" || cmd == "camlog") {
        // camrollback 0|1: camera views follow rollbacks (on by default); camlog 1|0: research (every view request)
        int on = 1;
        ss >> on;
        if (cmd == "camlog") RunOnGameThread([on] { SetCameraRollbackLog(on != 0); });
        else RunOnGameThread([on] { SetCameraRollback(on != 0); });
    } else if (cmd == "hudstep") {
        // hudstep 0|1: replay runs keep the battle HUD's movies in step (on by default; research)
        int on = 1;
        ss >> on;
        RunOnGameThread([on] { SetHudStep(on); });
    } else if (cmd == "camtrace") {
        // camtrace <from tick> <to tick> [class text]: research (camera objects per tick and frame)
        uint32_t from = 0, to = 0;
        std::string text = "Camera";
        ss >> from >> to >> text;
        RunOnGameThread([=] { TraceCameraObjects(from, to, text); });
    } else if (cmd == "cinelog") {
        // cinelog 2|1|0: research (2: every tick and frame; (cinematics, scripts per tick; the camera while one plays)
        int on = 1;
        ss >> on;
        RunOnGameThread([on] { LogCinematics(on); });
    } else if (cmd == "actorlog") {
        // actorlog <ticks>: research (script runners' move, frame; position regions)
        int ticks = 120;
        ss >> ticks;
        RunOnGameThread([ticks] { LogActors(ticks); });
    } else if (cmd == "rcmp") {
        // rcmp <a> <b> <hex from, - allowed> <hex to>: research
        std::string a, b, f, t;
        ss >> a >> b >> f >> t;
        auto hex = [](const std::string& s) { return s.rfind('-', 0) == 0 ? -std::stoll(s.substr(1), nullptr, 16) : std::stoll(s, nullptr, 16); };
        const long long from = hex(f), to = hex(t);
        RunOnGameThread([=] { CompareRegions(a, b, from, to); });
    } else if (cmd == "rpeek") {
        // rpeek <region> <hex offset, - allowed> [qwords]: research
        std::string name, off;
        int qwords = 4;
        ss >> name >> off >> qwords;
        const long long offset = off.rfind('-', 0) == 0 ? -std::stoll(off.substr(1), nullptr, 16) : std::stoll(off, nullptr, 16);
        RunOnGameThread([=] { PeekRegion(name, offset, qwords); });
    } else if (cmd == "ptrscan") {
        // ptrscan <from region> <hex before> <hex after> <to region>: research
        std::string from, to;
        size_t before = 0, after = 0;
        ss >> from >> std::hex >> before >> after >> std::dec >> to;
        RunOnGameThread([=] { ScanForPointers(from, before, after, to); });
    } else if (cmd == "rsnap") {
        RunOnGameThread([] { SnapRegions(); });
    } else if (cmd == "boxlog") {
        int ticks = 60;
        ss >> ticks;
        LogHitboxes(ticks);
    } else if (cmd == "winsize") {
        // winsize <client width> <client height>: research (the game window resized)
        int w = 0, h = 0;
        ss >> w >> h;
        if (HWND win = FindGameWindow(); win && w > 0 && h > 0) {
            RECT r{0, 0, w, h};
            AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongPtrW(win, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(win, GWL_EXSTYLE)));
            SetWindowPos(win, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT c{};
            GetClientRect(win, &c);
            Log("winsize: client %ldx%ld", c.right, c.bottom);
        }
    } else if (cmd == "ftrace") {
        // ftrace <ticks>: research (changed snapshot words per tick to Polarity\research\ftrace.bin)
        int ticks = 600;
        ss >> ticks;
        RunOnGameThread([ticks] { TraceRegions(ticks); });
    } else if (cmd == "freeze") {
        // freeze 1|0: research (the battle held on its frame)
        int on = 1;
        ss >> on;
        SetBattleFreeze(on != 0);
    } else if (cmd == "framemeter") {
        // framemeter 0|1: research (the frame meter in any battle)
        int on = 1;
        ss >> on;
        SetFrameMeterForced(on != 0);
    } else if (cmd == "battledata") {
        // battledata [dump]: research (the loaded move data against the game's own; dump: their hashes to Polarity/research)
        std::string what;
        ss >> what;
        if (what == "dump") RunOnGameThread([] { DumpLoadedBattleData(); });
        else CheckBattleDataSoon("asked");
        Log("battledata: mod paks: %s", ScanModPaks().Text().c_str());
    } else if (cmd == "benchmark") {
        // benchmark: test (the built-in round under stress 8, as Settings' Run benchmark starts it)
        // | benchmark dump <frame>|off: research (runs save that frame's state, a reference for a failing run's) | benchmark file <path>
        std::string what;
        ss >> what;
        if (what == "dump") {
            std::string frame;
            ss >> frame;
            SetBenchmarkDumpFrame(frame.empty() || frame == "off" ? -1 : std::atoi(frame.c_str()));
        } else if (what == "file") {
            // benchmark file <recording in Polarity\matches, or a full path>: research (any recording as the benchmark plays it)
            std::string file;
            std::getline(ss >> std::ws, file);
            const std::string path = file.find(':') != std::string::npos ? file : Utf8Path(MatchesDir()) + "\\" + file;
            if (!RequestBenchmark(path)) Log("benchmark: not started");
        } else if (!StartBenchmark(kPolarityVersion)) Log("benchmark: not started");
    } else if (cmd == "fmlog") {
        // fmlog <ticks>: research (each tick's frame meter classification)
        int ticks = 120;
        ss >> ticks;
        LogFrameMeter(ticks);
    } else if (cmd == "hitboxes") {
        // hitboxes 0|1: research (the box display in any battle)
        int on = 1;
        ss >> on;
        SetHitboxesForced(on != 0);
    } else if (cmd == "rdump") {
        // rdump <region> <tag>: research (region bytes to Polarity\research\<tag>.bin)
        std::string name, tag;
        ss >> name >> tag;
        RunOnGameThread([=] { DumpRegion(name, tag); });
    } else if (cmd == "rdiff") {
        // rdiff side|all [region name prefix]: research (snapshot region words changed since rsnap)
        std::string mode = "side", prefix;
        ss >> mode >> prefix;
        RunOnGameThread([mode, prefix] { DiffRegions(mode, prefix); });
    } else if (cmd == "dumpbac") {
        // dumpbac <BAC_RYU|BCH_RYU|...>: research (the loaded move file to Polarity\research)
        std::string name;
        ss >> name;
        RunOnGameThread([name] { DumpMoveFile(name); });
    } else if (cmd == "rfind") {
        // rfind f <float> | i <int> | bac <KWBAC name>: research (in the snapshot regions)
        std::string kind, a, b;
        ss >> kind >> a >> b;
        RunOnGameThread([kind, a, b] { FindInRegions(kind, a, b); });
    } else if (cmd == "uobj") {
        // uobj <name>: research (objects of that name, their class and owner)
        std::string name, how;
        ss >> name >> how;
        RunOnGameThread([name, dump = how == "dump"] { LogObjectsNamed(name, dump); });
    } else return false;
    return true;
}

void RunNetCommand(const std::string& line) {
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (RunResearchCommand(cmd, ss)) return;
    NetplayOptions& o = NetOptions();
    if (cmd == "host") {
        // host [random [costumes]] [manual]: manual is not ready by itself in
        // the lobby (to see the lobby's windows in a test).
        std::string word;
        o.randomSetup = false;
        o.randomCostumes = false;
        bool manual = false;
        while (ss >> word) {
            if (word == "random") o.randomSetup = true;
            if (word == "costumes") o.randomCostumes = true;
            if (word == "manual") manual = true;
        }
        NetplayOptions command = o;
        command.autoReady = !manual;
        command.name = TestName();
        NetHost(command);
    } else if (cmd == "join" || cmd == "spectate") {
        std::string code, word;
        ss >> code >> word;
        NetplayOptions command = o;
        command.autoReady = word != "manual";
        command.name = TestName();
        NetJoin(code, command, cmd == "spectate");
    } else if (cmd == "ticket") {
        // ticket host | join <code> | watch <code>, then any of: token <n>
        // (the P2 seat's), set (a table's set instead of a vote), rules
        // <r1 ...> (the rest of the line, core/rules.h): a session from a
        // match ticket (core/match_ticket.h), as tables will start them (testing).
        std::string what, word;
        ss >> what;
        MatchTicket t = what == "host" ? DirectMatchHost() : DirectMatchJoin("", what == "watch");
        if (what != "host") ss >> t.roomCode;
        while (ss >> word) {
            if (word == "token") ss >> t.seatToken;
            else if (word == "set") t.after = AfterMatch::Set;
            else if (word == "rules") {
                std::string rest;
                std::getline(ss >> std::ws, rest);
                std::string error;
                if (!ParseRules(rest, &t.rules, &error)) {
                    Log("ticket: rules '%s' not read: %s", rest.c_str(), error.c_str());
                    return;
                }
            }
        }
        NetplayOptions command = o;
        command.autoReady = true;
        command.name = TestName();
        NetPlayTicket(t, command);
    } else if (cmd == "presence") {
        // presence open | status | close: a second Weyvelength connection
        // beside any session's, as the hub's presence room will be (M3): it
        // connects (5555, then 443 for the public server), creates a room and
        // logs what it sees (testing that the two coexist).
        static std::unique_ptr<WeyveService> presence;
        std::string what;
        ss >> what;
        if (what == "open") {
            const std::string server = o.server;
            presence = WeyveService::Start(server, TestName() + " (presence)", server == kDefaultServer ? kDefaultServerAlsoPort : 0);
            presence->CreateRoom();
            Log("presence: connecting to %s", server.c_str());
        } else if (what == "close") {
            presence.reset();
            Log("presence: closed");
        } else if (presence) {
            const WeyveRoomView v = presence->View();
            static const char* const kServer[] = {"connecting", "connected", "failed", "lost"};
            Log("presence: %s (%s), room %s, %zu members, self %u", kServer[static_cast<int>(v.server)], v.serverText.c_str(),
                v.room.empty() ? "-" : WeyveService::DisplayCode(v.room).c_str(), v.members.size(), v.self);
            WeyveEvent e;
            while (presence->NextEvent(e)) Log("presence: event %d %s", static_cast<int>(e.type), e.text.c_str());
        } else {
            Log("presence: not open");
        }
    } else if (cmd == "settings") {
        // settings [row] | settings <row> type [text] | settings close: the
        // settings screen (test), on a row, typing text in it.
        std::string row, what;
        ss >> row >> what;
        if (row == "close") CloseSettingsScreen();
        else if (what == "type") {
            std::string text;
            std::getline(ss >> std::ws, text);
            OpenSettingsScreenTyping(row, text);
        } else OpenSettingsScreen(row);
    } else if (cmd == "rooms") {
        // rooms open | close | status | create [tables] [public 0|1] [title: 5, 3,RYU,KEN] |
        // join <code> | leave | chat <text> | table <request> [args]: the hub's presence (rooms_client.h),
        // driven by hand until the hub's screens (M3).
        std::string what;
        ss >> what;
        if (what == "open") {
            RoomsOpen(o.server, TestName(), o.server == kDefaultServer ? kDefaultServerAlsoPort : o.serverAlsoPort, o.keeperKey);
            // As from the rooms screen: in the keeper's desk, so requests
            // (create, peek) go through (the hub alone stays in no room).
            OpenRoomsScreen();
        } else if (what == "close") {
            RoomsClose();
        } else if (what == "create") {
            keeper::RoomSettings s;
            int pub = 1;
            std::string title = "0";
            ss >> s.tables >> pub >> title;
            s.isPublic = pub != 0;
            if (!keeper::ParseTitle(title, &s.title)) {
                Log("rooms: no such title '%s' (5, or 3,RYU,KEN)", title.c_str());
                return;
            }
            // As the rooms screen's Create: from the desk (joined first if
            // this game was on the hub or in a room just now).
            OpenRoomsScreen();
            if (!RoomsCreate(s)) Log("rooms: not open");
        } else if (what == "join") {
            std::string code;
            ss >> code;
            if (!RoomsJoin(code)) Log("rooms: '%s' is no room code (or not open)", code.c_str());
        } else if (what == "leave") {
            RoomsLeave();
        } else if (what == "table") {
            // rooms table <seat|stand|queue|unqueue|ready|decline|rules|kick> [args...]
            std::string kind, a;
            ss >> kind;
            std::vector<std::string> args;
            if (kind == "rules") {
                std::string table, rest;
                ss >> table;
                std::getline(ss, rest);
                if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                args = {table, rest};
            } else {
                while (ss >> a) args.push_back(a);
            }
            RoomsTable(kind, args);
        } else if (what == "watch") {
            // rooms watch <table, from 0> | -1: as X on the room screen.
            int table = -1;
            ss >> table;
            RoomsWatch(table);
        } else if (what == "mute") {
            // rooms mute <member id>: as A on that player's row (testing).
            uint32_t id = 0;
            ss >> id;
            ToggleMuteForTest(id);
        } else if (what == "chat") {
            std::string text;
            std::getline(ss, text);
            if (!text.empty() && text[0] == ' ') text.erase(0, 1);
            if (!RoomsChat(text)) Log("rooms: chat not sent (not in a room, or past the limits)");
        } else {
            const RoomsView v = RoomsSnapshot();
            Log("rooms: %s", DescribeRooms(v).c_str());
            for (const keeper::ListedRoom& r : v.publicRooms)
                Log("rooms:   public %s '%s' by %s, %d members, %d tables, %d open seats", r.code.c_str(),
                    keeper::TitleText(r.title).c_str(), r.owner.c_str(), r.members, r.tables, r.openSeats);
            for (size_t i = v.chat.size() > 5 ? v.chat.size() - 5 : 0; i < v.chat.size(); i++)
                Log("rooms:   chat %s: %s", v.chat[i].from ? v.chat[i].name.c_str() : "*", v.chat[i].text.c_str());
        }
    } else if (cmd == "matchchat") {
        // matchchat <text>: chat into the session's match room (testing the
        // host's ban of a modified client).
        std::string text;
        std::getline(ss >> std::ws, text);
        NetTestMatchChat(text);
    } else if (cmd == "delay") {
        // delay <frames>: this player's input delay for the next host/join.
        int frames = 2;
        ss >> frames;
        o.inputDelay = static_cast<uint8_t>(std::clamp(frames, 0, 10));
        Log("netplay: input delay %u for the next session", o.inputDelay);
    } else if (cmd == "name") {
        std::getline(ss >> std::ws, g_testName);
        Log("netplay: test sessions play as %s", TestName().c_str());
    } else if (cmd == "crash") {
        // crash game|thread|overflow|hang|fastfail|raise|stub|exit: testing crash reports
        // (crash_report.h). The game ends (or freezes) for real.
        std::string how;
        ss >> how;
        Log("crash: %s on purpose (testing)", how.c_str());
        if (how == "thread") {
            std::thread([] { *reinterpret_cast<volatile int*>(16) = 1; }).detach();
        } else if (how == "overflow") {
            RunOnGameThread([] {
                struct Deep {
#pragma warning(suppress : 4717) // endless on purpose
                    static int Down(volatile int n) {
                        volatile char pad[4096];
                        pad[0] = static_cast<char>(n);
                        return Down(n + 1) + pad[0];
                    }
                };
                Deep::Down(0);
            });
        } else if (how == "hang") {
            RunOnGameThread([] {
                for (volatile bool spin = true; spin;) {
                }
            });
        } else if (how == "fastfail") {
            RunOnGameThread([] { __fastfail(7); });
        } else if (how == "raise") {
            // An exception that is not a fault, left to the engine (as its
            // own errors are): it ends the game itself.
            RunOnGameThread([] { RaiseException(1, 0, 0, nullptr); });
        } else if (how == "stub") {
            // What Wine raises for a function it does not have (cabinet.dll's
            // Compress, imported by ordinal, before 0.2.6.1).
            RunOnGameThread([] {
                static const char dll[] = "cabinet.dll";
                const ULONG_PTR args[2] = {reinterpret_cast<ULONG_PTR>(dll), 33};
                RaiseException(0x80000100, EXCEPTION_NONCONTINUABLE, 2, args);
            });
        } else if (how == "exit") {
            // The game ending itself with an error code (as the engine does after an error).
            RunOnGameThread([] { TerminateProcess(GetCurrentProcess(), 3); });
        } else {
            RunOnGameThread([] { *reinterpret_cast<volatile int*>(16) = 1; });
        }
    } else if (cmd == "reports") {
        // reports [send] [url <address>]: desync report uploads (report_upload.h).
        std::string what, url;
        ss >> what;
        if (what == "url") {
            ss >> url;
            SetReportUrl(url);
            Log("reports: server %s", url.empty() ? "as configured" : url.c_str());
        } else SendReadyReports();
    } else if (cmd == "update") {
        // update [check [url]] | update install | update pretend <version> |
        // update restart: the updater (updater.h).
        std::string what, url;
        ss >> what >> url;
        if (what == "check") {
            if (!url.empty()) SetUpdateUrl(url);
            UpdateCheck();
        } else if (what == "install") UpdateInstall();
        else if (what == "pretend" && !url.empty()) UpdatePretendVersion(url);
        else if (what == "restart") {
            std::string error;
            if (!RestartGame(FindGameWindow(), &error)) Log("update: restart: %s", error.c_str());
        }
        const UpdateView v = UpdateViewNow();
        Log("update: state %d, this build %s, site's %s, %llu of %llu bytes%s%s", static_cast<int>(v.state), v.current.c_str(),
            v.latest.empty() ? "-" : v.latest.c_str(), static_cast<unsigned long long>(v.done),
            static_cast<unsigned long long>(v.total), v.message.empty() ? "" : ": ", v.message.c_str());
    } else if (cmd == "packlogs") {
        // packlogs: what Settings, Logs, "Pack logs to share" does (files.h).
        std::string error;
        if (PackLogsToShare(&error).empty()) Log("files: cannot pack the logs: %s", error.c_str());
    } else if (cmd == "server") {
        // server [host:port [also port]]: the rooms server the next host/join
        // uses, in this game only (the panel keeps its own choice). None:
        // back to the default. The second port is tried at the same time
        // (testing the public server's 443 fallback).
        std::string address;
        int alsoPort = 0;
        ss >> address >> alsoPort;
        o.server = !address.empty() ? address : !o.savedServer.empty() ? o.savedServer
                                                                       : kDefaultServer;
        o.serverAlsoPort = static_cast<uint16_t>(std::clamp(alsoPort, 0, 65535));
        Log("netplay: rooms server %s for the next sessions", o.server.c_str());
    } else if (cmd == "transport") {
        // transport udp|weyve: for the next host/join (testing both paths).
        std::string which;
        ss >> which;
        o.udp = which == "udp";
        Log("netplay: transport %s", o.udp ? "UDP" : ("Weyvelength " + o.server).c_str());
    } else if (cmd == "ready") {
        int on = 1;
        ss >> on;
        NetSetReady(on != 0);
    } else if (cmd == "choose") {
        std::string what;
        ss >> what;
        NetChoose(what == "rematch" ? lobby::Choice::Rematch : what == "select" ? lobby::Choice::CharacterSelect
                                                                                : lobby::Choice::None);
    } else if (cmd == "leave") {
        NetLeave();
    } else if (cmd == "park") {
        NetPark();
    } else if (cmd == "members") {
        // members <hex address> [bytes]: names the UObjects its fields point at.
        unsigned long long address = 0;
        int bytes = 0x400;
        ss >> std::hex >> address >> std::dec >> bytes;
        Log("members of %s:", ue4::Describe(reinterpret_cast<const void*>(address)).c_str());
        for (int off = 0; off < bytes; off += 8) {
            uint64_t v = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + off), &v, 8, &got)) break;
            if (v < 0x10000000000ull || v >= 0x800000000000ull) continue;
            const std::string d = ue4::Describe(reinterpret_cast<const void*>(v));
            if (d.find('?') == std::string::npos && d.size() > 3 && d.front() != ' ') Log("  +0x%x: %s (%llx)", off, d.c_str(), static_cast<unsigned long long>(v));
        }
    } else if (cmd == "classes") {
        // classes <text>: classes whose name contains `text`, with how many
        // live objects each has (diagnostics).
        std::string text;
        ss >> text;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        std::vector<const void*> classes;
        ue4::ForEachObject([&](const void* object) {
            const std::string kind = ue4::ObjectName(ue4::ClassOf(object));
            if ((kind == "Class" || kind == "BlueprintGeneratedClass") && ue4::ObjectName(object).find(text) != std::string::npos &&
                classes.size() < 60) {
                classes.push_back(object);
            }
        });
        for (const void* cls : classes) {
            int count = 0;
            const void* first = nullptr;
            ue4::ForEachObject([&](const void* object) {
                for (const void* c = ue4::ClassOf(object); c; c = ue4::SuperOf(c)) {
                    if (c != cls) continue;
                    if (!count++) first = object;
                    break;
                }
            });
            Log("class %s (%p): %d objects, first %s (%p)", ue4::ObjectName(cls).c_str(), cls, count,
                ue4::Describe(first).c_str(), first);
        }
    } else if (cmd == "packages") {
        // packages mark: remembers the loaded packages. packages new: logs
        // the packages loaded since (with how many objects each holds) and
        // those gone since (diagnostics: what a battle load brings in).
        std::string what;
        ss >> what;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        std::map<std::string, int> packages; // name -> objects inside
        ue4::ForEachObject([&](const void* object) {
            const void* root = object;
            while (const void* outer = ue4::OuterOf(root)) root = outer;
            if (ue4::ObjectName(ue4::ClassOf(root)) != "Package") return;
            packages[ue4::ObjectName(root)]++;
        });
        static std::map<std::string, int> marked;
        if (what == "mark") {
            marked = packages;
            Log("packages: %zu marked", marked.size());
            return;
        }
        size_t added = 0, objects = 0;
        for (const auto& [name, count] : packages) {
            if (marked.count(name)) continue;
            Log("package+ %s %d", name.c_str(), count);
            added++;
            objects += count;
        }
        size_t gone = 0;
        for (const auto& [name, count] : marked) {
            if (packages.count(name)) continue;
            Log("package- %s %d", name.c_str(), count);
            gone++;
        }
        Log("packages: %zu now, %zu new (%zu objects), %zu gone since the mark", packages.size(), added, objects, gone);
    } else if (cmd == "preload") {
        // preload <code> [costume]: into the game's character cache.
        std::string code;
        int costume = 0;
        ss >> code >> costume;
        PreloadCharacter(code, costume);
    } else if (cmd == "versions") {
        LogCharaVersions();
    } else if (cmd == "uistart") {
        // uistart [ms] [buttons]: testing, buttons (default start) on the
        // game's own pad input (PressUiButtons).
        int ms = 150;
        std::string buttons = "start";
        ss >> ms >> buttons;
        Step parsed;
        if (ParseButtons(buttons, parsed)) PressUiButtons(parsed.buttons, ms);
    } else if (cmd == "textures") {
        // textures <filter> [limit]: loaded Texture2D objects by name or package.
        std::string filter;
        int limit = 80;
        ss >> filter >> limit;
        LogTextures(filter, limit);
    } else if (cmd == "showtex") {
        // showtex <object name | /Game/... path> | showtex off: the texture's
        // shader resource view drawn by the overlay (research: portraits and
        // stage pictures for the controller UI).
        std::string which;
        ss >> which;
        if (which.empty() || which == "off") {
            SetOverlayTexture(nullptr, 0, 0, "");
            return;
        }
        RunOnGameThread([which] {
            const void* texture = FindOrLoadTexture(which);
            if (!texture) {
                Log("showtex: no texture %s", which.c_str());
                return;
            }
            const TextureView view = ViewOfTexture(texture);
            if (view.srv) SetOverlayTexture(view.srv, view.width, view.height, which);
        });
    } else if (cmd == "paktex") {
        // paktex on|off | status | show <path> [max side] | save <path> [max side]
        // | format <path> | dump <path>: textures read from the paks (pak_textures.h): on
        // or off for the screens, figures, one drawn by the overlay or
        // written as Polarity\textures\pak_<name>.dds (to compare with savetex's
        // copy of the game's own), or the view format of the game's own.
        std::string what, path;
        uint32_t maxSide = 0;
        ss >> what >> path >> maxSide;
        if (what == "on" || what == "off") SetPakTexturesEnabled(what == "on");
        else if (what == "status") Log("%s", PakTextureStatus().c_str());
        else if (what == "dump") {
            std::thread([path] { DumpPakPackage(path, Utf8Path(DataDir()) + "\\textures\\" + path.substr(path.rfind('/') + 1) + ".uasset"); }).detach();
        } else if (what == "format") {
            RunOnGameThread([path] {
                const TextureView view = ViewOfTexture(FindOrLoadTexture(path + "." + path.substr(path.rfind('/') + 1)), true);
                if (!view.srv) return;
                auto* srv = static_cast<ID3D11ShaderResourceView*>(view.srv);
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                srv->GetDesc(&desc);
                Log("paktex: the game's view of %s: %ux%u, view format %d, %u mips", path.c_str(), view.width, view.height,
                    int(desc.Format), desc.Texture2D.MipLevels);
                srv->Release();
            });
        } else if (what == "show" || what == "save") {
            std::thread([what, path, maxSide] {
                for (int i = 0; i < 200; i++) {
                    bool failed = false;
                    if (const TextureView* t = PakTexture(path, maxSide, true, &failed)) {
                        static_cast<IUnknown*>(t->srv)->AddRef();
                        Log("paktex: %s from the paks: %ux%u", path.c_str(), t->width, t->height);
                        if (what == "show") SetOverlayTexture(t->srv, t->width, t->height, path + " (paks)");
                        else SaveTextureLater(t->srv, "pak_" + path.substr(path.rfind('/') + 1));
                        return;
                    }
                    if (failed) {
                        Log("paktex: %s: the paks could not give it", path.c_str());
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                Log("paktex: %s: not ready after 5 s", path.c_str());
            }).detach();
        }
    } else if (cmd == "savetex") {
        // savetex <object name | /Game/... path> ...: each texture's top mip
        // written to Polarity\textures\<name>.dds (research).
        std::vector<std::string> which;
        std::string word;
        while (ss >> word) which.push_back(word);
        RunOnGameThread([which] {
            for (const std::string& w : which) {
                const void* texture = FindOrLoadTexture(w);
                const TextureView view = texture ? ViewOfTexture(texture) : TextureView{};
                if (!view.srv) {
                    Log("savetex: no texture %s", w.c_str());
                    continue;
                }
                // The path without /Game/ and the object name, / as _
                // (Chara_RYU_Hud_PartCharacter).
                std::string name = w.rfind("/Game/", 0) == 0 ? w.substr(6) : w;
                name = name.substr(0, name.find('.'));
                std::replace(name.begin(), name.end(), '/', '_');
                SaveTextureLater(view.srv, name);
            }
        });
    } else if (cmd == "watchmatch") {
        // watchmatch <n | live>: a spectator watches the session's n-th kept
        // match (1: the oldest) or the newest from its start (the panel's
        // Previous / Next / Live).
        std::string which;
        ss >> which;
        const NetView v = NetViewNow();
        uint64_t id = 0;
        if (which != "live") {
            const int n = std::atoi(which.c_str());
            if (n < 1 || n > static_cast<int>(v.matches.size())) {
                Log("watchmatch: no match %s (%zu kept)", which.c_str(), v.matches.size());
                return;
            }
            id = v.matches[n - 1].id;
        }
        Log("watchmatch: %s of %zu", which.c_str(), v.matches.size());
        NetWatchMatch(id);
    } else if (cmd == "warmupsleep") {
        // warmupsleep <ms>: each neutral warm-up tick before the battle's
        // objects exist waits first (a slower PC's warm-up; sfv_game.h).
        int ms = 0;
        ss >> ms;
        SetWarmupSleep(ms);
        Log("control: warm-up ticks wait %d ms", ms);
    } else if (cmd == "watchcamera") {
        // watchcamera: write watches on the battle camera's +0x94 and +0xE4
        // from the next freeze (research; watchreport, unwatch).
        // watchcamera new: from the next camera's construction instead
        // (+0x90, +0xE0, +0x1D0, +0x1F0).
        std::string when;
        ss >> when;
        if (when == "new") {
            WatchNextCamera();
            Log("control: the next battle camera is watched from its construction");
        } else {
            WatchCameraAtFreeze();
            Log("control: the camera is watched from the next freeze");
        }
    } else if (cmd == "emptystage") {
        TestEmptyStageRequest();
        Log("control: the next jump's stage request is emptied (test)");
    } else if (cmd == "warmupextra") {
        // warmupextra <n>: n more neutral warm-up ticks once the battle's
        // objects exist (test; sfv_game.h).
        int n = 0;
        ss >> n;
        SetWarmupExtra(n);
        Log("control: %d warm-up ticks after the objects", n);
    } else if (cmd == "shot") {
        // shot <path> [scale]: the next frame as shown, to a PNG (overlay.h
        // RequestScreenshot; path relative to Win64).
        std::string path;
        int scale = 1;
        ss >> path >> scale;
        if (path.empty()) Log("control: shot <path> [scale]");
        else RequestScreenshot(path, scale > 0 ? scale : 1);
    } else if (cmd == "burston") {
        int code = 5, frames = 300, scale = 4, count = 4;
        std::string dir;
        ss >> code >> dir >> frames >> scale >> count;
        SetBurstOnEvent(code, dir, frames, scale, count);
    } else if (cmd == "shotburst") {
        // shotburst <dir> <frames> [scale]: research (overlay.h RequestShotBurst; dir relative to Win64)
        std::string dir;
        int frames = 60, scale = 2;
        ss >> dir >> frames >> scale;
        RequestShotBurst(dir, frames, scale);
    } else if (cmd == "echo") {
        // echo <text>: logs it once every step before it has run (scripts
        // wait for it: cmd.ps1 -Wait).
        std::string rest;
        std::getline(ss, rest);
        Log("cmd: echo%s", rest.c_str());
    } else if (cmd == "uipads") {
        // uipads <0|1>: testing, 1 lets the game see the local pads (with
        // Start and Back) during a netplay match, 0 isolates them again.
        int on = 0;
        ss >> on;
        SetPadIsolation(!on);
        SetBlockPause(!on);
        Log("control: game sees the local pads during a match: %d", on);
    } else if (cmd == "mappedlog") {
        int updates = 0;
        ss >> updates;
        g_mappedLog = updates;
        Log("mappedlog: the input manager's pad 0 for the next %d updates, on change", updates);
    } else if (cmd == "inputlog") {
        int ticks = 0;
        ss >> ticks;
        LogGameInputs(ticks);
    } else if (cmd == "resimtasks") {
        // resimtasks <task rva (hex)> | v<class vtable rva> (the generic task
        // 0x3EB3F4 by its object's class)... | off | default (the lean replayed ticks' list)
        std::vector<uintptr_t> rvas;
        std::string word;
        bool standard = false;
        while (ss >> word) {
            if (word == "off") continue;
            if (word == "default") {
                standard = true;
                continue;
            }
            if (word[0] == 'v') rvas.push_back(0x80000000u | static_cast<uintptr_t>(std::stoull(word.substr(1), nullptr, 16)));
            else rvas.push_back(static_cast<uintptr_t>(std::stoull(word, nullptr, 16)));
        }
        SetResimTaskSkip(standard ? DefaultLeanResimTasks() : std::move(rvas));
    } else if (cmd == "resimskip") {
        int on = 1;
        ss >> on;
        SetResimPresentationSkip(on != 0);
    } else if (cmd == "phasecost") {
        int on = 1;
        ss >> on;
        SetPhaseTiming(on != 0);
    } else if (cmd == "serialjobs") {
        // serialjobs 0|1: driven ticks run the battle's jobs in order on the game thread (1, the default) or on its threads
        int on = 1;
        ss >> on;
        SetSerialJobs(on != 0);
    } else if (cmd == "joborder") {
        int calls = 1;
        ss >> calls;
        LogJobOrderNext(calls);
    } else if (cmd == "startprobe") {
        LogStartProbe();
    } else if (cmd == "soundrollback") {
        // soundrollback 0|1: the game's own rollback of sounds (on by default; 0: resimulated ticks play theirs again)
        int on = 1;
        ss >> on;
        SetSoundRollback(on != 0);
    } else if (cmd == "frameids") {
        // frameids 0|1: the game's restore callbacks get the game's own frame id (on by default; 0: the mod's old one)
        int on = 1;
        ss >> on;
        SetGameFrameIds(on != 0);
        Log("restore: callbacks get %s frame id", on ? "the game's" : "the mod's old");
    } else if (cmd == "leanresim") {
        // leanresim 0|1: replayed ticks without phase 8 and some phase-4 tasks (on by default; 0: whole updates)
        int on = 1;
        ss >> on;
        SetLeanResim(on != 0);
        SetResimTaskSkip(on ? DefaultLeanResimTasks() : std::vector<uintptr_t>{});
        Log("resim: replayed ticks %s", on ? "without presentation the newest tick makes up for" : "each a whole battle update");
    } else if (cmd == "forcerollback") {
        // forcerollback <frame>... [xor <hex>]: research (mispredict replays reload before
        // those frames; with xor, the frames first ran on a wrong guess of the remote input)
        std::vector<int> frames;
        uint32_t guessXor = 0;
        std::string w;
        while (ss >> w) {
            if (w == "xor") ss >> std::hex >> guessXor >> std::dec;
            else frames.push_back(std::atoi(w.c_str()));
        }
        SetForcedRollbacks(std::move(frames), guessXor);
    } else if (cmd == "ptrtest") {
        // ptrtest old|new: research (flow.h SetLegacyPointerTest)
        std::string how;
        ss >> how;
        SetLegacyPointerTest(how == "old");
    } else if (cmd == "mapat") {
        // mapat <hex address>...: research (commits 64 KB at each, zeroed: an address
        // a player's process had mapped, for a test that depends on it)
        std::string a;
        while (ss >> a) {
            const uintptr_t at = static_cast<uintptr_t>(std::stoull(a, nullptr, 16)) & ~uintptr_t{0xFFFF};
            void* p = VirtualAlloc(reinterpret_cast<void*>(at), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            Log("mapat: %p %s", reinterpret_cast<void*>(at), p ? "committed" : "not free");
        }
    } else if (cmd == "kowaitmode") {
        // kowaitmode 0|1: loads put back the mode the round's end waits on (on by default; 0: as before)
        int on = 1;
        ss >> on;
        SetKoWaitRollback(on != 0);
    } else if (cmd == "uisound") {
        // uisound <cursor|step|decide|cancel|error|open|close|notice|...|GameSoundSECue number>:
        // plays one of the mod's menu sounds now (ui_sound.h).
        std::string name;
        ss >> name;
        if (!PlayUiNamed(name)) Log("uisound: no sound %s", name.c_str());
    } else if (cmd == "soundlog") {
        // soundlog <count>: the next cue plays (research: sounds a rollback plays again)
        int count = 400;
        ss >> count;
        LogSoundPlays(count);
    } else if (cmd == "bonelog") {
        // bonelog <count> [<caller rva, hex>]
        int count = 200;
        std::string from;
        ss >> count >> from;
        LogBonePositions(count, from.empty() ? 0 : std::stoull(from, nullptr, 16));
    } else if (cmd == "drawlog") {
        // drawlog <count> | drawlog ticks <from> <to> (every draw in those driven ticks)
        std::string first;
        ss >> first;
        if (first == "ticks") {
            uint32_t from = 0, to = 0;
            ss >> from >> to;
            LogRandomDrawsInTicks(from, to);
        } else {
            LogRandomDraws(std::atoi(first.c_str()));
        }
    } else if (cmd == "charcache") {
        LogCharaCache();
    } else if (cmd == "datatable") {
        std::string name, filter;
        int limit = 200;
        ss >> name >> filter >> limit;
        if (filter == "-") filter.clear();
        LogDataTable(name, filter, limit);
    } else if (cmd == "variants") {
        LogAllVariants();
    } else if (cmd == "uniqueparam") {
        int player = 0, index = 0, value = -1;
        ss >> player >> index >> value;
        SetUniqueParamOverride(player, index, value);
        Log("uniqueparam: P%d [%d] = %d", player + 1, index, value);
    } else if (cmd == "voicelang") {
        // voicelang ja|en|auto: every character's voice in the next battles
        // (testing), or each one's from the Sound options (auto).
        std::string what;
        ss >> what;
        SetVoiceLanguageOverride(what == "ja" ? 0 : what == "en" ? 1
                                                                 : -1);
        Log("voicelang: %s (Ryu speaks %d)", what.c_str(), VoiceLanguageFor("RYU"));
    } else if (cmd == "vsync") {
        std::string what;
        ss >> what;
        if (what == "on" || what == "off") SetVsync(what == "on");
        Log("overlay: %s", VsyncInfo().c_str());
    } else if (cmd == "countryflag") {
        // countryflag on|off: the Country flag setting for this run (testing; not saved).
        std::string what;
        ss >> what;
        WeyveService::SetHideCountry(what == "off");
    } else if (cmd == "wordfilter") {
        // wordfilter on|off: the Word filter setting for this run (testing; not saved).
        std::string what;
        ss >> what;
        SetRoomsWordFilter(what != "off");
    } else if (cmd == "relayonly") {
        // relayonly on|off: the next sessions' links go only through TURN (testing).
        std::string what;
        ss >> what;
        WeyveService::SetRelayOnly(what != "off");
    } else if (cmd == "freeplay") {
        std::string what;
        ss >> what;
        SetFreePlayGuard(what);
    } else if (cmd == "presets") {
        std::string code;
        ss >> code;
        LogCostumePresets(code);
    } else if (cmd == "costumes") {
        std::string code;
        ss >> code;
        LogCharaCostumes(code);
    } else if (cmd == "charas") {
        std::string list;
        for (int i = 0; i < CharaCount(); i++) {
            list += " " + CharaCode(i) + "=" + CharaName(i) + "(" + std::to_string(CharaCostumeCount(i)) + ")";
        }
        Log("characters (%d):%s", CharaCount(), list.c_str());
    } else if (cmd == "watchregion") {
        // watchregion <region name> <hex offset> [bytes]: for the next battle.
        std::string name;
        size_t offset = 0;
        int bytes = 8;
        ss >> name >> std::hex >> offset >> std::dec >> bytes;
        WatchRegionOfNextBattle(name, offset, bytes);
    } else if (cmd == "overlay") {
        // overlay show|hide|toggle|preview <words>|select [stage|grid|loadout|ready] [spectate]
        // | rooms (the hub's custom rooms screen, open) | hub (back to the hub from it)
        // | battlelog (the hub's Battle Log, open)
        std::string what;
        ss >> what;
        if (what == "show") SetOverlayVisible(true);
        else if (what == "select") {
            std::string phase, watching;
            ss >> phase >> watching;
            RequestSelectPreview(phase == "grid" ? 1 : phase == "loadout" ? 2
                                                   : phase == "ready"     ? 3
                                                                          : 0,
                                 watching == "spectate");
        } else if (what == "hide") SetOverlayVisible(false);
        else if (what == "rooms") {
            // Tests: the pad's A on the hub goes to the tile the mouse is over.
            OpenHub();
            OpenRoomsScreen();
        } else if (what == "hub") CloseRoomsScreen();
        else if (what == "benchresult") {
            ShowLastBenchmarkResult();
        } else if (what == "battlelog") {
            OpenHub();
            OpenBattleLog();
        } else if (what == "preview") {
            std::string rest;
            std::getline(ss, rest);
            PreviewOverlay(rest);
        } else ToggleOverlay();
    } else if (cmd == "menu") {
        ReturnToMainMenu(1);
    } else if (cmd == "relocate") {
        // Finds the object array again (diagnostics).
        const auto engine = *reinterpret_cast<uint8_t* const*>(g_imageBase + 0x3D878C8);
        ue4::LocateObjectsFrom(engine ? *reinterpret_cast<void* const*>(engine + 0x1E0) : nullptr);
    } else if (cmd == "scene") {
        // scene <StateClass> [init message object name]
        std::string name, param;
        ss >> name >> param;
        ChangeScene(name, param);
    } else if (cmd == "named") {
        // named <object name>: every object with that name, with its outer.
        std::string name;
        ss >> name;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        int shown = 0;
        ue4::ForEachObject([&](const void* object) {
            if (shown >= 20 || ue4::ObjectName(object) != name) return;
            shown++;
            Log("named %s (%p) in %s", ue4::Describe(object).c_str(), object, ue4::Describe(ue4::OuterOf(object)).c_str());
        });
    } else if (cmd == "ofclass") {
        // ofclass <class name> [max]: live objects of that class or a subclass,
        // with their class and outer (research).
        std::string name;
        int max = 60;
        ss >> name >> max;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        int shown = 0, total = 0;
        ue4::ForEachObject([&](const void* object) {
            for (const void* c = ue4::ClassOf(object); c; c = ue4::SuperOf(c)) {
                if (ue4::ObjectName(c) != name) continue;
                total++;
                if (shown < max && ue4::ObjectName(object).rfind("Default__", 0) != 0) {
                    shown++;
                    Log("ofclass %s (%p) in %s", ue4::Describe(object).c_str(), object, ue4::Describe(ue4::OuterOf(object)).c_str());
                }
                break;
            }
        });
        Log("ofclass %s: %d of %d shown", name.c_str(), shown, total);
    } else if (cmd == "gfx") {
        // gfx [vtable words]: the live Scaleform players (ScaleformUtilGFxPlayer
        // and subclasses): movie asset path (+0x300), step mode (+0x3A0), the
        // GFxMovie object (+0x318), the native movie player (+0x398) with its
        // vtable as RVAs (research: the battle HUD's banners).
        int words = 0, actorSlots = 0;
        ss >> words >> actorSlots;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        const auto rva = [](uint64_t v) { return v >= g_imageBase && v < g_imageBase + 0x5000000 ? v - g_imageBase : 0; };
        // The player class's own virtuals: slots that differ from AActor's.
        if (actorSlots > 0) {
            const auto* actor = static_cast<const uint8_t*>(ue4::FindObject("Default__Actor", "Actor"));
            const auto* player = static_cast<const uint8_t*>(ue4::FindObject("Default__ScaleformUtilGFxPlayer", "ScaleformUtilGFxPlayer"));
            if (actor && player) {
                const auto* va = *reinterpret_cast<const uint64_t* const*>(actor);
                const auto* vp = *reinterpret_cast<const uint64_t* const*>(player);
                std::string w;
                for (int i = 0; i < actorSlots && IsReadable(va + i, 8) && IsReadable(vp + i, 8); i++) {
                    if (va[i] == vp[i]) continue;
                    char b[40];
                    std::snprintf(b, sizeof(b), " %d:%llX/%llX", i, static_cast<unsigned long long>(rva(va[i])), static_cast<unsigned long long>(rva(vp[i])));
                    w += b;
                }
                Log("gfx player virtuals (slot:actor/player):%s", w.c_str());
            } else Log("gfx: no Default__Actor (%p) or Default__ScaleformUtilGFxPlayer (%p)", actor, player);
        }
        ue4::ForEachObject([&](const void* object) {
            bool is = false;
            for (const void* c = ue4::ClassOf(object); c && !is; c = ue4::SuperOf(c)) is = ue4::ObjectName(c) == "ScaleformUtilGFxPlayer";
            if (!is || ue4::ObjectName(object).rfind("Default__", 0) == 0) return;
            const auto* p = static_cast<const uint8_t*>(object);
            if (!IsReadable(p, 0x3A8)) return;
            std::string path;
            const auto* text = *reinterpret_cast<const wchar_t* const*>(p + 0x300);
            const int len = *reinterpret_cast<const int32_t*>(p + 0x308);
            if (text && len > 0 && len < 400 && IsReadable(text, len * 2)) {
                for (int i = 0; i < len && text[i]; i++) path += static_cast<char>(text[i] < 128 ? text[i] : '?');
            }
            const auto* movie = *reinterpret_cast<const uint8_t* const*>(p + 0x318);
            const auto* native = *reinterpret_cast<const uint8_t* const*>(p + 0x398);
            const uint64_t vt = native && IsReadable(native, 8) ? *reinterpret_cast<const uint64_t*>(native) : 0;
            const void* asset = *reinterpret_cast<const void* const*>(p + 0x390);
            const void* asset2 = *reinterpret_cast<const void* const*>(p + 0x310);
            Log("gfx %s (%p): '%s' asset %s / %s, step %d, movie %s (%p), native %p vtable rva 0x%llX, object vtable rva 0x%llX, draw %d",
                ue4::Describe(object).c_str(), object, path.c_str(), ue4::Describe(asset).c_str(), ue4::Describe(asset2).c_str(), p[0x3A0],
                ue4::Describe(movie).c_str(), movie, native,
                static_cast<unsigned long long>(rva(vt)), static_cast<unsigned long long>(rva(*reinterpret_cast<const uint64_t*>(p))),
                *reinterpret_cast<const int32_t*>(p + 0x374));
            if (native && IsReadable(native, 0x80)) {
                std::string w;
                for (int i = 0; i < 16; i++) {
                    char b[24];
                    std::snprintf(b, sizeof(b), " %llx", static_cast<unsigned long long>(reinterpret_cast<const uint64_t*>(native)[i]));
                    w += b;
                }
                Log("gfx   native words:%s", w.c_str());
            }
            if (words > 0 && vt && IsReadable(reinterpret_cast<const void*>(vt), words * 8)) {
                std::string w;
                for (int i = 0; i < words; i++) {
                    char b[24];
                    std::snprintf(b, sizeof(b), " %d:%llX", i, static_cast<unsigned long long>(rva(reinterpret_cast<const uint64_t*>(vt)[i])));
                    w += b;
                }
                Log("gfx   native vtable:%s", w.c_str());
                words = 0; // once
            }
        });
    } else if (cmd == "find") {
        // find <text>: objects whose name contains `text` (case-insensitive), up to 60.
        std::string text;
        ss >> text;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        auto lower = [](std::string v) {
            for (char& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return v;
        };
        const std::string needle = lower(text);
        int shown = 0;
        ue4::ForEachObject([&](const void* object) {
            if (shown >= 60 || lower(ue4::ObjectName(object)).find(needle) == std::string::npos) return;
            shown++;
            Log("find %s (%p) in %s", ue4::Describe(object).c_str(), object, ue4::ObjectName(ue4::OuterOf(object)).c_str());
        });
        Log("find %s: %d shown", text.c_str(), shown);
    } else if (cmd == "classinfo") {
        // classinfo <class name>: its fields (UField chain from +0x38, next at
        // +0x28) with the raw words that hold a property's offset and size.
        std::string name;
        ss >> name;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        const auto* cls = static_cast<const uint8_t*>(ue4::FindObject(name, "Class"));
        if (!cls) cls = static_cast<const uint8_t*>(ue4::FindObject(name, "BlueprintGeneratedClass"));
        Log("classinfo %s: %p, super %s, size 0x%zx", name.c_str(), cls, cls ? ue4::ObjectName(ue4::SuperOf(cls)).c_str() : "",
            cls ? static_cast<size_t>(*reinterpret_cast<const int32_t*>(cls + 0x40)) : 0);
        // Fields of a struct: name, then the raw words after UField (property
        // offset at +0x4C).
        auto fields = [](const uint8_t* owner, const char* indent) {
            const uint8_t* f = owner ? *reinterpret_cast<const uint8_t* const*>(owner + 0x38) : nullptr;
            for (int i = 0; f && i < 200 && IsReadable(f, 0x80); i++) {
                const auto* w = reinterpret_cast<const int32_t*>(f + 0x30);
                Log("%s%s: %x %x %x %x %x %x %x %x", indent, ue4::Describe(f).c_str(), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                    w[7]);
                // An array of structs: ArrayProperty +0x70 inner property,
                // StructProperty +0x70 its struct (PropertiesSize at +0x40).
                // A struct field: the same, from the field itself.
                const std::string fieldClass = ue4::ObjectName(ue4::ClassOf(f));
                if (fieldClass == "ArrayProperty" || fieldClass == "StructProperty") {
                    const auto* inner = fieldClass == "StructProperty" ? f : *reinterpret_cast<const uint8_t* const*>(f + 0x70);
                    if (inner && fieldClass == "ArrayProperty" && ue4::ObjectName(ue4::ClassOf(inner)) != "StructProperty") {
                        Log("%s  of %s", indent, ue4::ObjectName(ue4::ClassOf(inner)).c_str());
                    }
                    if (inner && ue4::ObjectName(ue4::ClassOf(inner)) == "StructProperty") {
                        const auto* st = *reinterpret_cast<const uint8_t* const*>(inner + 0x70);
                        if (st && IsReadable(st, 0x48)) {
                            Log("%s  of %s, size 0x%x:", indent, ue4::Describe(st).c_str(), *reinterpret_cast<const int32_t*>(st + 0x40));
                            const uint8_t* g = *reinterpret_cast<const uint8_t* const*>(st + 0x38);
                            for (int j = 0; g && j < 100 && IsReadable(g, 0x50); j++) {
                                Log("%s    %s at +0x%x", indent, ue4::Describe(g).c_str(), *reinterpret_cast<const int32_t*>(g + 0x4C));
                                g = *reinterpret_cast<const uint8_t* const*>(g + 0x28);
                            }
                        }
                    }
                }
                f = *reinterpret_cast<const uint8_t* const*>(f + 0x28);
            }
        };
        fields(cls, "  ");
    } else if (cmd == "poke") {
        // poke <hex address> <hex value> [bytes 1|4|8]: testing only.
        unsigned long long address = 0, value = 0;
        int bytes = 4;
        ss >> std::hex >> address >> value >> std::dec >> bytes;
        SIZE_T done = 0;
        const bool ok = (bytes == 1 || bytes == 4 || bytes == 8) &&
                        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, bytes, &done);
        Log("poke %llx = %llx (%d bytes): %s", address, value, bytes, ok ? "ok" : "failed");
    } else if (cmd == "findbytes") {
        // findbytes <hex bytes, e.g. 0a0b0c>: where they are in writable memory (research).
        std::string hex;
        ss >> hex;
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        FindBytes(bytes);
    } else if (cmd == "layout") {
        // layout [16 hex values]: pad 0's live Button Settings table (kPadKeyTable),
        // logged, or replaced for this run (testing: different layouts per PC).
        auto* pads = *reinterpret_cast<uint8_t* const*>(g_imageBase + kInputManager + 8);
        if (!pads || !IsReadable(pads, kPadStride)) return;
        auto* table = reinterpret_cast<uint32_t*>(pads + kPadKeyTable);
        std::string word;
        uint32_t values[16];
        int count = 0;
        while (count < 16 && ss >> word) values[count++] = static_cast<uint32_t>(std::stoul(word, nullptr, 16));
        if (count == 16) std::memcpy(table, values, sizeof(values));
        std::string text;
        for (int k = 0; k < 16; k++) {
            char hex[12];
            std::snprintf(hex, sizeof(hex), " %x", table[k]);
            text += hex;
        }
        Log("layout: pad 0 keys up..select give%s%s", text.c_str(), count == 16 ? " (set)" : "");
    } else if (cmd == "keys") {
        // keys <key names> [hold ms]: presses each key in turn through the
        // system (to the game when its window is in front; testing a PC's
        // keyboard layout from a script). Names: letters, digits, up/down/left/right, enter.
        std::vector<WORD> vks;
        int holdMs = 150;
        std::string word;
        while (ss >> word) {
            if (std::isdigit(static_cast<unsigned char>(word[0])) && word.size() > 1) holdMs = std::atoi(word.c_str());
            else if (word == "up") vks.push_back(VK_UP);
            else if (word == "down") vks.push_back(VK_DOWN);
            else if (word == "left") vks.push_back(VK_LEFT);
            else if (word == "right") vks.push_back(VK_RIGHT);
            else if (word == "enter") vks.push_back(VK_RETURN);
            else if (word.size() == 1) vks.push_back(static_cast<WORD>(std::toupper(static_cast<unsigned char>(word[0]))));
        }
        // The game's window to the front first (an Alt tap lets a process that
        // is not in front take it, as win.ps1 does; joining the front window's
        // input thread too). The real foreground: always_active's hook says
        // the game's window.
        HWND w = g_gameWindow;
        auto realFront = [] { return g_origForeground ? g_origForeground() : GetForegroundWindow(); };
        for (int attempt = 0; w && attempt < 3 && realFront() != w; attempt++) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            const DWORD front = GetWindowThreadProcessId(realFront(), nullptr);
            const DWORD self = GetCurrentThreadId();
            const bool attached = front && front != self && AttachThreadInput(self, front, TRUE);
            INPUT alt[2] = {};
            alt[0].type = alt[1].type = INPUT_KEYBOARD;
            alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
            alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, alt, sizeof(INPUT));
            BringWindowToTop(w);
            SetForegroundWindow(w);
            if (attached) AttachThreadInput(self, front, FALSE);
            Sleep(300);
        }
        const bool inFront = w && realFront() == w;
        UINT accepted = 0;
        for (WORD vk : vks) {
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = in[1].ki.wVk = vk;
            in[0].ki.wScan = in[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            // 0 when the desktop takes no input (locked, or a disconnected
            // remote desktop session).
            accepted += SendInput(1, &in[0], sizeof(INPUT));
            Sleep(holdMs);
            SendInput(1, &in[1], sizeof(INPUT));
            Sleep(150);
        }
        Log("keys: %u of %zu pressed (game window %s)", accepted, vks.size(), inFront ? "in front" : "not in front");
    } else if (cmd == "input2") {
        // input2 <key> <value> | input2 restore: a key of [AssignKeyboard] in
        // the game's Input2.ini (KeyboardKeys_N = a key name), from the next
        // start; the first change keeps Input2.ini.sfvr-backup (testing).
        wchar_t local[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
        const std::wstring path = std::wstring(local) + L"\\StreetFighterV\\Saved\\Config\\WindowsNoEditor\\Input2.ini";
        const std::wstring backup = path + L".sfvr-backup";
        std::string key, value;
        ss >> key >> value;
        if (key == "restore") {
            const bool ok = CopyFileW(backup.c_str(), path.c_str(), FALSE) && DeleteFileW(backup.c_str());
            Log("input2: %s", ok ? "restored from the backup" : "no backup to restore");
            return;
        }
        if (key.empty() || value.empty()) return;
        CopyFileW(path.c_str(), backup.c_str(), TRUE); // only the first time
        const bool ok = WritePrivateProfileStringW(L"AssignKeyboard", std::wstring(key.begin(), key.end()).c_str(),
                                                   std::wstring(value.begin(), value.end()).c_str(), path.c_str());
        Log("input2: [AssignKeyboard] %s=%s %s (from the next start)", key.c_str(), value.c_str(), ok ? "set" : "failed");
    } else if (cmd == "owner") {
        // owner <hex address>: the UObject whose memory holds it, and the offset (research).
        unsigned long long address = 0;
        ss >> std::hex >> address;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        int found = 0;
        ue4::ForEachObject([&](const void* object) {
            const auto start = reinterpret_cast<uintptr_t>(object);
            const size_t size = ue4::ObjectSize(object);
            if (address < start || address >= start + size || size == 0) return;
            Log("owner %llx: %s +0x%llx (size 0x%zx)", address, ue4::Describe(object).c_str(),
                static_cast<unsigned long long>(address - start), size);
            found++;
        });
        if (!found) Log("owner %llx: no UObject holds it", address);
    } else if (cmd == "enuminfo") {
        // enuminfo <enum name>: its names and values (UEnum::Names at +0x40,
        // FNames in value order (this engine version); research).
        std::string name;
        ss >> name;
        if (!ue4::EnsureObjects(g_imageBase)) return;
        const auto* e = static_cast<const uint8_t*>(ue4::FindObject(name, "Enum"));
        if (!e || !IsReadable(e, 0x50)) {
            Log("enuminfo: no enum %s", name.c_str());
            return;
        }
        const auto* pairs = *reinterpret_cast<const uint8_t* const*>(e + 0x40);
        const int32_t count = *reinterpret_cast<const int32_t*>(e + 0x48);
        if (!pairs || count <= 0 || count > 512 || !IsReadable(pairs, static_cast<size_t>(count) * 8)) {
            Log("enuminfo %s: unexpected layout (%p, %d)", name.c_str(), pairs, count);
            return;
        }
        for (int32_t i = 0; i < count; i++) {
            const int32_t index = *reinterpret_cast<const int32_t*>(pairs + i * 8);
            Log("enuminfo %s: %d %s", name.c_str(), i, ue4::NameText(index).c_str());
        }
    } else if (cmd == "keyconfigs") {
        // keyconfigs: the button settings (KWKeyConfigs, KWUserProfileDetails
        // +0xB0: 15 bytes Data, then TypeData) of every user profile (research).
        if (!ue4::EnsureObjects(g_imageBase)) return;
        ue4::ForEachObject([](const void* object) {
            if (ue4::ObjectName(ue4::ClassOf(object)) != "KWUserProfileDetails") return;
            const auto* p = static_cast<const uint8_t*>(object);
            if (!IsReadable(p, 0xC0)) return;
            char hex[64] = {};
            for (int i = 0; i < 16; i++) std::snprintf(hex + i * 2, 3, "%02x", p[0xB0 + i]);
            Log("keyconfigs: %s (%p) main %d: %s", ue4::ObjectName(object).c_str(), object, p[0x28] & 1, hex);
        });
    } else if (cmd == "findptr") {
        unsigned long long v = 0;
        ss >> std::hex >> v;
        FindPointer(v);
    } else if (cmd == "replay") {
        // replay <file in Polarity\matches (or reports\) or a full path> [stress <distance>]
        // ... [watch <from> <to> <region>]
        // ... [keep <from> <to>] (a straight run kept as a full recording with
        // state dumps of those frames) [cold] (no preload: the characters
        // load as the battle is created, which then has its objects sooner)
        // [trap <frame> <region> <hex offset> <bytes>]... (write watches)
        // [hold <frame> <seconds>] (stands still after that frame)
        // [mispredict <delay> [remote 1|2]] (one player's inputs late: real rollbacks)
        // [latch tick|game] (the pad latch: after every tick, or the game's own)
        std::string file, word, region, traps;
        int stress = 0, from = -1, to = -1, keepFrom = -1, keepTo = -1, holdAt = -1, holdSeconds = 0, mispredict = 0,
            remote = 0;
        bool cold = false;
        ss >> file;
        if (file == "view") {
            // replay view <file> [hold <frame> <seconds>]: the Battle Log's playback (rview controls it)
            ss >> file;
            if (ss >> word && word == "hold") ss >> holdAt >> holdSeconds;
            RequestReplayView(file, file, nullptr, holdAt, holdSeconds);
            return;
        }
        while (ss >> word) {
            if (word == "stress") ss >> stress;
            if (word == "watch") ss >> from >> to >> region;
            if (word == "keep") ss >> keepFrom >> keepTo;
            if (word == "cold") cold = true;
            if (word == "hold") ss >> holdAt >> holdSeconds;
            if (word == "mispredict") ss >> mispredict;
            if (word == "remote") ss >> remote;
            if (word == "latch") {
                std::string how;
                ss >> how;
                SetReplayLatch(how == "tick" ? 1 : how == "game" ? 0
                                                                 : -1);
            }
            if (word == "trap") {
                std::string f, r, off, b;
                ss >> f >> r >> off >> b;
                traps += f + " " + r + " " + off + " " + b + " ";
            }
        }
        RequestReplay(file, stress, from, to, region, keepFrom, keepTo, !cold, traps, holdAt, holdSeconds, mispredict, remote - 1);
    } else if (cmd == "roundinfo") {
        RunOnGameThread([] { LogRoundInfo(); });
    } else if (cmd == "ufunc") {
        // ufunc <name>: research (a UFunction's owner and native code)
        std::string name;
        ss >> name;
        RunOnGameThread([name] { LogFunction(name); });
    } else if (cmd == "functrace") {
        // functrace <name> <owner|-> <offset hex>: research (log a UFunction's native calls)
        std::string name, owner;
        uint32_t offset = 0;
        ss >> name >> owner >> std::hex >> offset;
        if (owner == "-") owner.clear();
        RunOnGameThread([name, owner, offset] { TraceFunction(name, owner, offset); });
    } else if (cmd == "rview") {
        // rview pause|play|toggle|step <n>|seek <frame>|round <1|-1>|fast <n>|leave|status:
        // the viewed replay's controls (tests)
        std::string what;
        int arg = 0;
        ss >> what >> arg;
        if (what == "callkeys" || what == "crossrounds" || what == "anykeys" || what == "keysanywhere" || what == "intro") {
            SetReplayExperiment(what, arg != 0);
            return;
        }
        if (what == "callpace" || what == "callafter") {
            SetReplayExperimentValue(what, arg);
            return;
        }
        if (what == "status") {
            const ReplayView v = ReplayViewNow();
            Log("rview: %s frame %d of %d, round %d, %s%s, fast %d, %d keyframe loads, %d frames differ, back to %d, "
                "round frame %d of %d, forward %d, progress %.3f, opening %d, next round %d",
                v.active ? "active" : "none", v.frame, v.frames, v.round, v.paused ? "paused" : "playing", v.seeking ? " (seeking)" : "",
                v.fast, v.seeks, v.mismatches, v.backLimit, v.roundFrame, v.roundFrames, v.canStepForward ? 1 : 0,
                static_cast<double>(v.roundProgress), v.opening ? 1 : 0, v.canRoundForward ? 1 : 0);
            Log("rview: takeover %d, countdown %.1f, ending %d", v.takeover, static_cast<double>(v.takeoverCountdown), v.takeoverEnding ? 1 : 0);
            return;
        }
        const ReplayControl c = what == "pause" ? ReplayControl::Pause : what == "play"       ? ReplayControl::Play
                                                                     : what == "step"         ? ReplayControl::Step
                                                                     : what == "seek"         ? ReplayControl::Seek
                                                                     : what == "round"        ? ReplayControl::Round
                                                                     : what == "fast"         ? ReplayControl::Fast
                                                                     : what == "takeover"     ? ReplayControl::Takeover
                                                                     : what == "takeoverstop" ? ReplayControl::TakeoverStop
                                                                     : what == "leave"        ? ReplayControl::Leave
                                                                     : what == "outofsync"    ? ReplayControl::OutOfSync
                                                                     : what == "holdat"       ? ReplayControl::HoldAt
                                                                                              : ReplayControl::Toggle;
        ControlReplay(c, arg);
    } else if (cmd == "regions") {
        size_t size = 0;
        ss >> size;
        LogRegionsOfSize(size);
    } else if (cmd == "phases") {
        LogPhaseMachine();
    } else if (cmd == "tasklist") {
        int calls = 3000;
        ss >> calls;
        TaskCensus(calls);
    } else if (cmd == "outsidetest") {
        int ticks = 8;
        std::string mode;
        ss >> ticks >> mode;
        if (ue4::EnsureObjects(g_imageBase)) RequestOutsideProbe(nullptr, ticks, mode == "all");
        else Log("outside: object array not found");
    } else if (cmd == "recdiff") {
        // The int32 fields that differ between the P1 and P2 battle records.
        uint8_t* data = BattleData();
        if (!data) {
            Log("recdiff: no battle data");
        } else {
            for (uint32_t off = 0; off < battledata::kRecordStride; off += 4) {
                const int32_t a = *reinterpret_cast<int32_t*>(data + off);
                const int32_t b = *reinterpret_cast<int32_t*>(data + battledata::kRecordStride + off);
                if (a != b) Log("recdiff +0x%03x: P1 %d (%08x) P2 %d (%08x)", off, a, a, b, b);
            }
        }
    } else if (cmd == "nextstage") {
        std::string code;
        ss >> code;
        SetPendingStage(code);
        Log("next stage: %s", code.empty() ? "(game's choice)" : code.c_str());
    } else if (cmd == "stages") {
        LogStageTable();
    } else if (cmd == "randomoff") {
        // randomoff [CHN,S25]: the Random stages list's stages left out, for
        // the sessions hosted from now on (testing; not saved: the panel
        // file keeps the player's own list).
        std::string list;
        ss >> list;
        o.randomOff = ParseStageMask(list);
        Log("random stages: leaving out '%s'", FormatStageMask(o.randomOff).c_str());
    } else if (cmd == "randomstages") {
        // randomstages [n]: n random stage draws (as the host's: owned, from
        // the Random stages list; and from any stage), counted; each draw
        // walks the stage table (about 12 ms).
        int n = 200;
        ss >> n;
        std::mt19937_64 rng{std::random_device{}()};
        std::map<std::string, int> counts;
        for (int i = 0; i < n; i++) {
            const int owned = RandomStage(rng(), true, o.randomOff), any = RandomStage(rng(), false);
            counts["owned " + (owned < 0 ? std::string("none") : StageCode(owned))]++;
            counts["any " + (any < 0 ? std::string("none") : StageCode(any))]++;
        }
        for (const auto& [code, count] : counts) Log("randomstages: %s %d", code.c_str(), count);
    } else if (cmd == "worldctx") {
        LogWorldContext();
    } else if (cmd == "battlenow") {
        JumpToBattle();
    } else if (cmd == "transitions") {
        // transitions 0|1: research ([netplay] lockstep_round_transitions for the next sessions)
        int on = 1;
        ss >> on;
        o.lockstepTransitions = on != 0;
        Log("netplay: round transitions %s for the next sessions", on ? "in lockstep" : "rolled back");
    } else if (cmd == "netsim") {
        double loss = 0;
        ss >> o.conditions.latencyMs >> o.conditions.jitterMs >> loss;
        o.conditions.loss = loss / 100.0;
        NetSetConditions(o.conditions);
    } else if (cmd == "mash") {
        // mash <seed> [from frame] | mash loop <buttons>:<frames>... (buttons as `pad`, "none")
        o.mashFrom = 0;
        o.inputLoop.clear();
        std::string first;
        ss >> first;
        if (first == "loop") {
            o.mashSeed = 0;
            std::string word;
            while (ss >> word) {
                const size_t colon = word.find(':');
                Step step;
                if (colon == std::string::npos || !ParseButtons(word.substr(0, colon), step)) {
                    Log("netplay: mash loop: cannot read '%s'", word.c_str());
                    continue;
                }
                o.inputLoop.push_back({step.buttons, static_cast<uint16_t>(std::max(1, std::atoi(word.c_str() + colon + 1)))});
            }
            Log("netplay: local player plays a loop of %zu inputs", o.inputLoop.size());
        } else {
            o.mashSeed = std::strtoull(first.c_str(), nullptr, 10);
            ss >> o.mashFrom;
            Log("netplay: local player mashes with seed %llu", static_cast<unsigned long long>(o.mashSeed));
        }
    } else if (cmd == "dumps") {
        ss >> o.dumpEvery >> o.maxDumps;
        Log("netplay: recordings dump the state every %d frames, at most %d times", o.dumpEvery, o.maxDumps);
    } else if (cmd == "skipcallbacks") {
        // skipcallbacks <thunk rva>/<target rva> ... | off: game callbacks
        // loads leave out (research: load cost).
        std::vector<uint64_t> keys;
        std::string word;
        while (ss >> word) {
            const size_t slash = word.find('/');
            if (slash == std::string::npos) continue;
            keys.push_back(std::stoull(word.substr(0, slash), nullptr, 16) << 32 | std::stoull(word.substr(slash + 1), nullptr, 16));
        }
        SetSkippedLoadCallbacks(std::move(keys));
    } else if (cmd == "recordings") {
        // recordings full|compact: which frames' region hashes the next saved recordings keep.
        std::string what;
        ss >> what;
        if (what == "full" || what == "compact") o.fullRecordings = what == "full";
        Log("netplay: recordings are %s", o.fullRecordings ? "full (every frame's region hashes)" : "compact (region hashes around desyncs)");
    } else if (cmd == "pick") {
        std::string code;
        int costume = 0, color = 1, comment = 0, vskill = 1, vtrigger = 1;
        ss >> code >> costume >> color >> comment >> vskill >> vtrigger;
        o.pick.variation = 0;
        if (const size_t colon = code.find(':'); colon != std::string::npos) {
            o.pick.variation = static_cast<uint8_t>(std::atoi(code.c_str() + colon + 1));
            code.resize(colon);
        }
        o.hasPick = !code.empty();
        o.pick.character = PackCode(code.c_str());
        ReloadOverlayChoices();
        o.pick.costume = static_cast<uint16_t>(costume);
        o.pick.color = static_cast<uint8_t>(color - 1);
        o.pick.comment = static_cast<uint8_t>(comment);
        o.pick.vskill = static_cast<uint8_t>(vskill - 1);
        o.pick.vtrigger = static_cast<uint8_t>(vtrigger - 1);
        if (code == "ELV") {
            // Eleven: a random fighter in silver, V-Skill and V-Trigger at random.
            const uint32_t roll = static_cast<uint32_t>(GetTickCount64());
            std::string as;
            int silver = 0;
            if (ElevenStandIn(roll, &as, &silver)) {
                o.pick = PlayerSetup{};
                o.pick.character = PackCode(as.c_str());
                o.pick.costume = static_cast<uint16_t>(silver);
                o.pick.vskill = static_cast<uint8_t>((roll >> 8) & 1);
                o.pick.vtrigger = static_cast<uint8_t>((roll >> 9) & 1);
                Log("netplay: Eleven plays %s in costume %d", as.c_str(), silver);
            }
        }
        NetSetPick(o.hasPick, o.pick);
    } else if (cmd == "stage") {
        // stage <code> | stage <table index> | stage (keep)
        std::string v;
        ss >> v;
        o.stageCode.clear();
        o.stage = kKeepStage;
        if (!v.empty() && std::isdigit(static_cast<unsigned char>(v[0]))) o.stage = static_cast<uint16_t>(std::stoi(v));
        else o.stageCode = v;
        // In a session, the host's stage for the next match (as the panel sets it).
        if (!o.stageCode.empty()) NetSetStage(o.stageCode);
    } else if (cmd == "netstatus") {
        Log("netplay status: %s", NetStatus().c_str());
    } else if (cmd == "battle") {
        Log("battle: %s | rendering %.1f fps", BattleInfo().c_str(), RenderFps());
    } else if (cmd == "peek") {
        // peek <hex address> [qwords]: logs memory, with image addresses as RVAs.
        unsigned long long address = 0;
        int count = 4;
        ss >> std::hex >> address >> std::dec >> count;
        for (int i = 0; i < count && i < 64; i++) {
            uint64_t v = 0;
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + 8 * i), &v, 8, &got)) break;
            const bool inImage = v >= g_imageBase && v < g_imageBase + 0x5000000;
            Log("peek %llx: %016llx%s", address + 8 * i, static_cast<unsigned long long>(v),
                inImage ? (" (rva 0x" + [&] { char b[16]; std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(v - g_imageBase)); return std::string(b); }() + ")").c_str() : "");
        }
    } else if (cmd == "setstage") {
        int stage = 0;
        ss >> stage;
        uint8_t* data = BattleData();
        if (data) *reinterpret_cast<int32_t*>(data + battledata::kStage) = stage;
        Log("setstage %d: %s", stage, data ? "written" : "no battle data now");
    } else if (cmd == "startskew") {
        // startskew: the next netplay start's frozen state hashes differently
        // here (testing the end of a session whose starts differ).
        NetSkewNextStartForTest();
        Log("startskew: the next frozen start hashes differently");
    } else if (cmd == "setrounds") {
        // setrounds <n>: the +0x08 round count match setups write, as a game
        // that kept another one would (testing; 0: the match's own).
        int rounds = 0;
        ss >> rounds;
        SetMatchRoundsForTest(rounds);
        Log("setrounds %d: match setups write it from now on", rounds);
    }
}

constexpr int kBothPads = -2; // Step::user: pads 0 and 1 together

bool ParseButtons(const std::string& spec, Step& step) {
    std::stringstream ss(spec);
    std::string b;
    while (std::getline(ss, b, '+')) {
        if (b == "up") step.buttons |= XINPUT_GAMEPAD_DPAD_UP;
        else if (b == "down") step.buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        else if (b == "left") step.buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
        else if (b == "right") step.buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        else if (b == "start") step.buttons |= XINPUT_GAMEPAD_START;
        else if (b == "back") step.buttons |= XINPUT_GAMEPAD_BACK;
        else if (b == "lb") step.buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        else if (b == "rb") step.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        else if (b == "a") step.buttons |= XINPUT_GAMEPAD_A;
        else if (b == "b") step.buttons |= XINPUT_GAMEPAD_B;
        else if (b == "x") step.buttons |= XINPUT_GAMEPAD_X;
        else if (b == "y") step.buttons |= XINPUT_GAMEPAD_Y;
        else if (b == "lt") step.lt = 255;
        else if (b == "rt") step.rt = 255;
        else if (b == "sup") step.ly = 32767;
        else if (b == "sdown") step.ly = -32768;
        else if (b == "sleft") step.lx = -32768;
        else if (b == "sright") step.lx = 32767;
        else if (b != "none") return false;
    }
    return true;
}

bool Parse(const std::string& line, Step& step) {
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd == "pad") {
        // pad <user|both> <buttons> [hold ms, -1: keep] [gap ms]; both: pads 0 and 1 at once.
        std::string user, buttons;
        step.kind = Step::Press;
        step.gapMs = 50;
        ss >> user >> buttons >> step.holdMs >> step.gapMs;
        step.user = user == "both" ? kBothPads : std::atoi(user.c_str());
        return (step.user == kBothPads || (step.user >= 0 && step.user < XUSER_MAX_COUNT)) && ParseButtons(buttons, step);
    }
    if (cmd == "wait") {
        step.kind = Step::Wait;
        ss >> step.holdMs;
        return true;
    }
    if (cmd == "scan" || cmd == "rescan") {
        step.kind = cmd == "scan" ? Step::Scan : Step::Rescan;
        ss >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "trace") {
        step.kind = Step::Trace;
        uint32_t rva;
        while (ss >> std::hex >> rva) step.rvas.push_back(rva);
        return !step.rvas.empty();
    }
    if (cmd == "tracereport") {
        step.kind = Step::TraceReport;
        return true;
    }
    if (cmd == "scanrecord") {
        step.kind = Step::ScanRecord;
        ss >> std::hex >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "scanshow") {
        step.kind = Step::ScanShow;
        return true;
    }
    if (cmd == "scansmall") {
        step.kind = Step::ScanSmall;
        return true;
    }
    if (cmd == "scandelta") {
        step.kind = Step::ScanDelta;
        ss >> step.holdMs;
        return !ss.fail();
    }
    if (cmd == "setup") {
        // setup <code> <costume> <color> <comment> <vskill> <vtrigger> (x2) <stage>, as the menus show them;
        // <code>:<n> also sets the costume variation
        step.kind = Step::Setup;
        for (auto& p : step.setup.players) {
            std::string code;
            int costume, color, comment, vskill, vtrigger;
            ss >> code >> costume >> color >> comment >> vskill >> vtrigger;
            if (const size_t colon = code.find(':'); colon != std::string::npos) {
                p.variation = static_cast<uint8_t>(std::atoi(code.c_str() + colon + 1));
                code.resize(colon);
            }
            p.character = PackCode(code.c_str());
            p.costume = static_cast<uint16_t>(costume);
            p.color = static_cast<uint8_t>(color - 1);
            p.comment = static_cast<uint8_t>(comment);
            p.vskill = static_cast<uint8_t>(vskill - 1);
            p.vtrigger = static_cast<uint8_t>(vtrigger - 1);
        }
        int stage = 0;
        ss >> stage;
        step.setup.stage = static_cast<uint16_t>(stage);
        return !ss.fail();
    }
    if (cmd == "host" || cmd == "join" || cmd == "matchchat" || cmd == "shot" || cmd == "shotburst" || cmd == "burston" || cmd == "warmupsleep" || cmd == "warmupextra" || cmd == "watchcamera" || cmd == "emptystage" || cmd == "spectate" || cmd == "ticket" || cmd == "presence" || cmd == "rooms" || cmd == "settings" || cmd == "server" || cmd == "packlogs" || cmd == "update" || cmd == "reports" || cmd == "crash" || cmd == "name" || cmd == "delay" || cmd == "transport" || cmd == "ready" || cmd == "choose" || cmd == "leave" || cmd == "netsim" || cmd == "transitions" || cmd == "mash" || cmd == "dumps" || cmd == "recordings" || cmd == "pick" ||
        cmd == "stage" || cmd == "netstatus" || cmd == "battle" || cmd == "canceljump" || cmd == "setstage" || cmd == "setrounds" || cmd == "startskew" || cmd == "park" || cmd == "peek" || cmd == "battlenow" || cmd == "findptr" || cmd == "findbytes" || cmd == "keyconfigs" || cmd == "enuminfo" || cmd == "owner" || cmd == "layout" || cmd == "keys" || cmd == "input2" || cmd == "members" || cmd == "worldctx" || cmd == "poke" || cmd == "stages" || cmd == "randomstages" || cmd == "randomoff" || cmd == "nextstage" || cmd == "recdiff" || cmd == "outsidetest" || cmd == "tasklist" || cmd == "phases" || cmd == "regions" || cmd == "costumes" || cmd == "variants" || cmd == "presets" || cmd == "freeplay" || cmd == "relayonly" || cmd == "wordfilter" || cmd == "countryflag" || cmd == "vsync" || cmd == "uniqueparam" || cmd == "voicelang" || cmd == "datatable" || cmd == "replay" || cmd == "rview" || cmd == "roundinfo" || cmd == "ufunc" || cmd == "uobj" || cmd == "rfind" || cmd == "dumpbac" || cmd == "actorlog" || cmd == "cinelog" || cmd == "camtrace" || cmd == "camrollback" || cmd == "camlog" || cmd == "hudstep" || cmd == "watchphase" || cmd == "watchname" || cmd == "rsnap" || cmd == "ptrscan" || cmd == "rpeek" || cmd == "rcmp" || cmd == "rdump" || cmd == "hitboxes" || cmd == "framemeter" || cmd == "benchmark" || cmd == "battledata" || cmd == "fmlog" || cmd == "freeze" || cmd == "ftrace" || cmd == "winsize" || cmd == "boxlog" || cmd == "rdiff" || cmd == "functrace" || cmd == "classes" || cmd == "classinfo" || cmd == "named" || cmd == "ofclass" || cmd == "gfx" || cmd == "scene" || cmd == "relocate" || cmd == "menu" || cmd == "overlay" || cmd == "charas" || cmd == "find" || cmd == "watchregion" || cmd == "packages" || cmd == "preload" || cmd == "charcache" || cmd == "drawlog" || cmd == "bonelog" || cmd == "soundlog" || cmd == "uisound" || cmd == "soundrollback" || cmd == "kowaitmode" || cmd == "forcerollback" || cmd == "ptrtest" || cmd == "mapat" || cmd == "frameids" || cmd == "leanresim" || cmd == "startprobe" || cmd == "phasecost" || cmd == "serialjobs" || cmd == "joborder" || cmd == "resimskip" || cmd == "resimtasks" || cmd == "inputlog" || cmd == "mappedlog" || cmd == "versions" || cmd == "uipads" || cmd == "uistart" || cmd == "watchchat" || cmd == "wheel" || cmd == "echo" || cmd == "watchmatch" || cmd == "textures" || cmd == "showtex" || cmd == "savetex" || cmd == "paktex" || cmd == "skipcallbacks") {
        step.kind = Step::Net;
        step.line = line;
        return true;
    }
    if (cmd == "showsetup" || cmd == "clearsetup") {
        step.kind = cmd == "showsetup" ? Step::ShowSetup : Step::ClearSetup;
        return true;
    }
    if (cmd == "connect" || cmd == "disconnect") {
        step.kind = cmd == "connect" ? Step::Connect : Step::Disconnect;
        ss >> step.user;
        return step.user >= 0 && step.user < XUSER_MAX_COUNT;
    }
    step.kind = Step::Game;
    GameCommand& c = step.request.command;
    if (cmd == "save") c = GameCommand::Save;
    else if (cmd == "load") c = GameCommand::Load;
    else if (cmd == "ringsave") c = GameCommand::RingSave;
    else if (cmd == "ringload") c = GameCommand::RingLoad;
    else if (cmd == "layout") c = GameCommand::Layout;
    else if (cmd == "watch") {
        c = GameCommand::Watch;
        step.request.arg2 = 4;
        ss >> step.request.arg0 >> std::hex >> step.request.arg1 >> std::dec >> step.request.arg2;
    } else if (cmd == "watchaddr") {
        c = GameCommand::WatchAddr;
        step.request.arg2 = 4;
        std::string rw;
        ss >> std::hex >> step.request.address >> std::dec >> step.request.arg2 >> rw;
        step.request.flag = rw == "rw";
    } else if (cmd == "watchreport") c = GameCommand::WatchReport;
    else if (cmd == "stress" || cmd == "learnnoise") {
        c = cmd == "stress" ? GameCommand::Stress : GameCommand::LearnNoise;
        step.request.arg0 = 600;
        step.request.arg1 = 7;
        step.request.arg2 = 1;
        ss >> step.request.arg0 >> step.request.arg1 >> step.request.arg2;
    } else if (cmd == "unwatch") c = GameCommand::Unwatch;
    else if (cmd == "synctest") {
        c = GameCommand::SyncTest;
        step.request.arg0 = 300;
        step.request.arg1 = 1;
        ss >> step.request.arg0 >> step.request.arg1;
    } else return false;
    return true;
}

void Run(const Step& s) {
    switch (s.kind) {
        case Step::Press: {
            const int first = s.user == kBothPads ? 0 : s.user;
            const int last = s.user == kBothPads ? 1 : s.user;
            for (int u = first; u <= last; u++) {
                VirtualPad& pad = g_pads[u];
                pad.connected = true;
                pad.buttons = s.buttons;
                pad.lt = s.lt;
                pad.rt = s.rt;
                pad.lx = s.lx;
                pad.ly = s.ly;
                pad.packet++;
            }
            // A negative hold leaves them pressed (until the next pad step).
            if (s.holdMs < 0) break;
            Sleep(s.holdMs);
            for (int u = first; u <= last; u++) {
                VirtualPad& pad = g_pads[u];
                pad.buttons = 0;
                pad.lt = 0;
                pad.rt = 0;
                pad.lx = 0;
                pad.ly = 0;
                pad.packet++;
            }
            Sleep(s.gapMs);
            break;
        }
        case Step::Wait:
            Sleep(s.holdMs);
            break;
        case Step::Connect:
            g_pads[s.user].connected = true;
            break;
        case Step::Setup:
            SetPendingSetup(s.setup);
            Log("setup pending: %s", DescribeSetup(s.setup).c_str());
            break;
        case Step::Net:
            RunNetCommand(s.line);
            break;
        case Step::ShowSetup:
            LogBattleSetup("now");
            break;
        case Step::ClearSetup:
            ClearPendingSetup();
            Log("setup cleared");
            break;
        case Step::Scan:
            ScanStart(s.holdMs);
            break;
        case Step::Rescan:
            ScanNext(s.holdMs);
            break;
        case Step::ScanShow:
            ScanShow();
            break;
        case Step::ScanSmall:
            ScanSmall();
            break;
        case Step::ScanDelta:
            ScanDelta(s.holdMs);
            break;
        case Step::ScanRecord:
            ScanRecordFilter(static_cast<uint32_t>(s.holdMs));
            break;
        case Step::Trace:
            TraceFunctions(g_imageBase, s.rvas);
            break;
        case Step::TraceReport:
            TraceReport(g_imageBase);
            break;
        case Step::Disconnect:
            g_pads[s.user].connected = false;
            break;
        case Step::Game: {
            // Watchpoints by address work on every thread from here, also
            // outside battles (menus have no game-thread hook to run them).
            const GameCommand c = s.request.command;
            if (c == GameCommand::WatchAddr || c == GameCommand::WatchReport || c == GameCommand::Unwatch) {
                if (c == GameCommand::WatchAddr) {
                    ArmWatch(0, reinterpret_cast<const void*>(s.request.address), s.request.arg2, g_imageBase,
                             s.request.flag);
                }
                if (c == GameCommand::WatchReport) WatchReport();
                if (c == GameCommand::Unwatch) DisarmWatches();
                break;
            }
            std::lock_guard lock(g_mutex);
            g_gameRequests.push_back(s.request);
            break;
        }
    }
}

// Steps read from the command file, run in order on their own thread: the
// file is taken at once even while earlier steps (pad presses, waits, key
// sequences) still run. Taken only when it was free before, a file written
// meanwhile used to wait, and a script's next file replaced it (its steps
// were lost).
std::mutex g_stepMutex;
std::condition_variable g_stepReady;
std::deque<Step> g_stepQueue;

void StepRunner() {
    ThreadGuard running;
    for (;;) {
        Step s;
        {
            std::unique_lock lock(g_stepMutex);
            g_stepReady.wait_for(lock, std::chrono::milliseconds(200), [] { return !g_stepQueue.empty() || Stopping(); });
            if (Stopping()) return;
            if (g_stepQueue.empty()) continue;
            s = g_stepQueue.front();
            g_stepQueue.pop_front();
        }
        Run(s);
    }
}

void CommandLoop(std::wstring path) {
    std::thread(StepRunner).detach();
    ThreadGuard running;
    uint64_t lastPolls[XUSER_MAX_COUNT] = {};
    ULONGLONG lastReport = GetTickCount64();
    ULONGLONG lastTitle = 0;
    for (;;) {
        std::ifstream in(path);
        if (Stopping()) return;
        NetPollHotkeys();
        // The window procedure (overlay input, pause blocking) also without
        // always-active, once the window exists.
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
        if (in) {
            std::string line;
            std::deque<Step> steps;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                Step s;
                if (Parse(line, s)) steps.push_back(s);
                else Log("cmd: cannot parse '%s'", line.c_str());
            }
            in.close();
            DeleteFileW(path.c_str());
            size_t ahead = 0;
            {
                std::lock_guard lock(g_stepMutex);
                ahead = g_stepQueue.size();
                for (const Step& s : steps) g_stepQueue.push_back(s);
            }
            g_stepReady.notify_one();
            if (ahead) Log("cmd: running %zu steps (after %zu still queued)", steps.size(), ahead);
            else Log("cmd: running %zu steps", steps.size());
        }
        // The game sets its own title for a while after starting; keep ours.
        if (!g_activePending && GetTickCount64() - lastTitle > 1000) {
            lastTitle = GetTickCount64();
            RefreshStatus();
        }
        if (GetTickCount64() - lastReport > 60000) {
            lastReport = GetTickCount64();
            char buf[128];
            int n = 0;
            for (int u = 0; u < XUSER_MAX_COUNT; u++) {
                const uint64_t p = g_polls[u];
                n += std::snprintf(buf + n, sizeof(buf) - n, " %llu", static_cast<unsigned long long>(p - lastPolls[u]));
                lastPolls[u] = p;
            }
            Log("xinput polls per 60 s by user:%s; %s", buf, PumpStats().c_str());
        }
        Sleep(50);
    }
}

WNDPROC g_origWndProc = nullptr;
std::atomic<bool> g_alwaysActive{false};

// UE4 stops processing pad input once it is told the app went inactive.
LRESULT CALLBACK ActiveWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    HookGuard guard;
    // The overlay first; what it takes, the game does not see (raw input
    // still needs DefWindowProc to be released).
    if (OverlayWndProc(w, msg, wp, lp)) {
        // The game takes the mouse (SetCapture) at start and lets it go when
        // it sees the mouse move; with the overlay up it sees none, so it
        // kept it: a click on the title bar (the close button) came as a
        // click in the window. The overlay's mouse lets it go.
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
            // Keyboard Start (Enter) and Back (Escape) would pause one side only.
            // Start still goes to the match as this player's (MappedLocalInput):
            // the intro skip when both press it.
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
            // The game is closing: tell the others now (Bye) rather than let
            // them wait for the silence timeout. A fault from here on is the
            // game's exit, not a crash to report.
            OnQuitting("window closed");
            NetLeave();
            break;
        default:
            break;
    }
    return CallWindowProcW(g_origWndProc, w, msg, wp, lp);
}

} // namespace

HWND RealForegroundWindow() { return g_origForeground ? g_origForeground() : GetForegroundWindow(); }

void SetPadIsolation(bool isolate) { g_isolatePads = isolate; }

PackedInput LocalPadInput(int user) { return user >= 0 && user < XUSER_MAX_COUNT ? g_padInput[user].load() : 0; }

uint32_t GamePadKeys() { return g_gameKeys; }

PackedInput MappedLocalInput(int) {
    // Start as the player pressed it (the pause filter keeps it from the
    // game): on any pad, or the keyboard's Enter. Players with a keyboard
    // could not skip an intro (2026-10-30), nor a pad in another slot.
    PackedInput start = g_keyboardStart ? kStart : 0;
    for (int u = 0; u < XUSER_MAX_COUNT; u++) start |= g_padInput[u].load() & kStart;
    if (!g_origInputUpdate) return LocalPadInput(0) | start;
    return g_mappedInput.load() | start;
}

void SetBlockPause(bool block) {
    g_blockPause = block;
    if (!block) g_keyboardStart = false;
}

void PressUiButtons(uint16_t buttons, int ms) {
    g_uiButtons = buttons;
    g_uiButtonsUntil = GetTickCount64() + ms;
}

void RestoreWindowProc() {
    if (g_origWndProc && g_gameWindow) {
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_origWndProc = nullptr;
    }
}

namespace {

void SubclassWindow() {
    if (!g_gameWindow || g_origWndProc) return;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ActiveWndProc)));
}

void KeepWindowActive() {
    g_alwaysActive = true;
    SubclassWindow();
    // It may already have been told it is inactive.
    PostMessageW(g_gameWindow, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(g_gameWindow, WM_ACTIVATE, WA_ACTIVE, 0);
    PostMessageW(g_gameWindow, WM_SETFOCUS, 0, 0);
}

} // namespace

// Where the game exe's import of `module`'s function (by ordinal or by name)
// leads, or null.
void* GameImport(const char* module, WORD ordinal, const char* name) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
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

bool InstallControl(const std::wstring& commandFile, int virtualPads, bool alwaysActive) {
    g_gameWindow = FindGameWindow();
    SubclassWindow();
    if (alwaysActive) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto hook = [&](const char* name, void* detour, HwndFn* original) {
            void* target = reinterpret_cast<void*>(GetProcAddress(user32, name));
            return target && MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK &&
                   MH_EnableHook(target) == MH_OK;
        };
        const bool ok = hook("GetForegroundWindow", reinterpret_cast<void*>(&ForegroundDetour), &g_origForeground) &&
                        hook("GetActiveWindow", reinterpret_cast<void*>(&ActiveDetour), &g_origActive) &&
                        hook("GetFocus", reinterpret_cast<void*>(&FocusDetour), &g_origFocus);
        if (ok && g_gameWindow) KeepWindowActive();
        else if (ok) g_activePending = true; // the command loop finishes it once the window exists
        Log("control: always active %s (window %p)", !ok ? "FAILED" : g_gameWindow ? "on"
                                                                                   : "waiting for the window",
            g_gameWindow.load());
    }
    for (int u = 0; u < virtualPads && u < XUSER_MAX_COUNT; u++) g_pads[u].connected = true;
    // Hooked where the game's import leads (the proxy's export), ahead of the
    // Steam overlay's patch there: Steam answers for its pads without calling
    // on, so hooked further in (the real XInput) they never showed. Without
    // the import, the real XInput's (real_xinput.h).
    void* target = GameImport("xinput1_3.dll", 2, "XInputGetState");
    if (!target) {
        HMODULE xinput = sfv::proxy::RealXInput();
        target = xinput ? reinterpret_cast<void*>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    }
    if (!target || MH_CreateHook(target, reinterpret_cast<void*>(&GetStateDetour), reinterpret_cast<void**>(&g_origGetState)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        Log("control: cannot hook XInputGetState");
        return false;
    }
    auto* update = reinterpret_cast<uint8_t*>(g_imageBase + kInputManagerUpdate);
    if (std::memcmp(update, kInputManagerUpdateBytes, sizeof(kInputManagerUpdateBytes)) != 0 ||
        MH_CreateHook(update, reinterpret_cast<void*>(&InputUpdateDetour), reinterpret_cast<void**>(&g_origInputUpdate)) != MH_OK ||
        MH_EnableHook(update) != MH_OK) {
        Log("control: cannot hook the input manager; netplay reads the pads with the default layout");
        g_origInputUpdate = nullptr;
    }
    std::thread(CommandLoop, commandFile).detach();

    // The game only polls pads it has seen connected. After a hot reload it has
    // seen ours disconnect; a device change makes it enumerate them again.
    EnumWindows(
        [](HWND w, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(w)) PostMessageW(w, WM_DEVICECHANGE, 0x0007, 0);
            return TRUE;
        },
        0);
    Log("control: virtual pads ready, commands from sfv_cmd.txt");
    return true;
}

GameRequest TakeGameRequest() {
    std::lock_guard lock(g_mutex);
    if (g_gameRequests.empty()) return {};
    const GameRequest r = g_gameRequests.front();
    g_gameRequests.pop_front();
    return r;
}

void SetScriptedPad(int user, bool enabled, uint16_t buttons, uint8_t rightTrigger) {
    if (user < 0 || user >= XUSER_MAX_COUNT) return;
    VirtualPad& pad = g_pads[user];
    pad.scriptedButtons = buttons;
    pad.scriptedRt = rightTrigger;
    pad.scriptedAt = GetTickCount64();
    pad.scripted = enabled;
    pad.packet++;
}

uint64_t PadPolls(int user) {
    return user >= 0 && user < XUSER_MAX_COUNT ? g_polls[user].load() : 0;
}

} // namespace sfv::mod
