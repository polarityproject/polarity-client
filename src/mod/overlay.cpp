#include "mod/overlay.h"

#include "mod/battle_driver.h"
#include "mod/frame_meter.h"
#include "mod/hitbox_display.h"

#include "core/random_stage.h"
#include "mod/pak_textures.h"
#include "mod/ui_sound.h"
#include "mod/profile.h"
#include "mod/textures.h"

#include "core/keeper_protocol.h"
#include "core/match_code.h"
#include "mod/config.h"
#include "mod/control.h"
#include "mod/local_input.h"
#include "mod/entitlements.h"
#include "mod/files.h"
#include "mod/font_resources.h"
#include "mod/flow.h"
#include "mod/hitch.h"
#include "mod/lifecycle.h"
#include "mod/log.h"
#include "mod/netplay_driver.h"
#include "mod/select_screen.h"
#include "mod/battle_log_screen.h"
#include "mod/battle_setup.h"
#include "mod/benchmark.h"
#include "mod/replay_driver.h"
#include "mod/fight_ui.h"
#include "core/key_names.h"
#include "mod/hub_screen.h"
#include "mod/ui_kit.h" // the screens' fade
#include "mod/rooms_screen.h"
#include "mod/room_screen.h"
#include "mod/watch_chat.h"
#include "mod/watch_panel.h"
#include "mod/settings_screen.h"
#include "core/match_ticket.h"
#include "mod/rooms_client.h"
#include "mod/profile.h"
#include "mod/status.h"
#include "mod/report_upload.h"
#include "mod/updater.h"
#include "mod/weyve_service.h"
#include "core/png.h"

#include <MinHook.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <random>
#include <memory>
#include <mutex>
#include <sstream>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace sfv::mod {

namespace {

// IDXGISwapChain vtable slots.
constexpr int kPresentSlot = 8;
constexpr int kResizeBuffersSlot = 13;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT syncInterval, UINT flags);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT count, UINT width, UINT height,
                                                    DXGI_FORMAT format, UINT flags);
PresentFn g_origPresent = nullptr;
ResizeBuffersFn g_origResizeBuffers = nullptr;
void* g_presentTarget = nullptr;
void* g_resizeTarget = nullptr;
// The game's swap chain's table entry for Present, once it points at
// TablePresentDetour, and what it held before. Capture tools (OBS's game
// capture, Discord's Go Live) hook dxgi's Present in its code, as the hook
// above does, and the one hooked last runs first: hooked after the mod, they
// took the frame before the overlay was drawn into it. The game calls
// Present through the table, so drawing there comes before every such hook.
void** g_tablePresentSlot = nullptr;
PresentFn g_tablePresentNext = nullptr;
thread_local bool t_inTablePresent = false; // the code hook below passes the frame on

// Render thread state (Present and ResizeBuffers run there).
IDXGISwapChain* g_swapChain = nullptr; // the one ImGui was set up for
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_window = nullptr;
bool g_imgui = false;

// In a match: the leave modal is up (Back + Start). The old F1 panel is gone:
// the hub, the rooms and Polarity settings do its jobs.
std::atomic<bool> g_visible{false};
std::atomic<bool> g_hubAllowed{false};    // at the game's menus with no session: Back + Start opens the hub
std::atomic<bool> g_chordKey{false};      // F1, the keyboard's Back + Start (window thread -> FeedPads)
std::atomic<bool> g_reloadChoices{false}; // ReloadOverlayChoices: read the choices again
std::atomic<bool> g_spectating{false};    // a spectator's session: a Start tap opens the watch panel (M5)
std::atomic<bool> g_watchPanelTap{false}; // ... tapped (the frame toggles the panel)
std::atomic<ULONGLONG> g_xinputAt{0};     // when the XInput hook last saw a pad (OverlayPad)
std::atomic<bool> g_voting{false};        // the post-match vote is up and takes input
// A spectator over a watched match's results screen: the game gets no input
// (a press there can open its own Results Menu, which players' votes keep
// shut by taking the pads), but nothing is voted on here.
std::atomic<bool> g_watchHold{false};
std::atomic<HWND> g_inputWindow{nullptr}; // the window ImGui reads input for
std::string g_build;                      // the mod's version (SetOverlayBuild)

// g_build without the product name ("0.4.0" from "Polarity 0.4.0").
std::string BuildVersion() {
    const size_t space = g_build.find(' ');
    return space == std::string::npos ? g_build : g_build.substr(space + 1);
}

std::string g_restartError;              // the update modal's last failed restart, why
std::atomic<float> g_fps{0};             // presented frames per second (RenderFps)
std::atomic<bool> g_inMatch{false};      // a netplay match runs (the frame-drop log, Start held for the panel)
std::string g_packResult;                // render thread: what the last "Pack logs to share" did
std::atomic<bool> g_noVsync{false};      // present without waiting for the display's refresh
std::atomic<UINT> g_gameSyncInterval{0}; // the sync interval and flags the game presents with (VsyncInfo)
std::atomic<UINT> g_gamePresentFlags{0};
std::atomic<int64_t> g_presentMicros{0}; // ... time in Present, summed with the frames (VsyncInfo)
std::atomic<int64_t> g_presentFrames{0};

// Window messages for ImGui, from the window thread to the render thread.
struct Message {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
};

std::mutex g_inputMutex;
std::vector<Message> g_input;

std::wstring g_panelPath;

// The pads' latest real state, for ImGui's gamepad navigation.
struct PadState {
    std::atomic<uint16_t> buttons{0};
    std::atomic<int16_t> lx{0}, ly{0};
    std::atomic<uint8_t> lt{0}, rt{0}; // the triggers (the Battle Log's filters, fast forward)
};

PadState g_pads[XUSER_MAX_COUNT];

// All pads together, as ImGui gamepad keys.
// The keyboard keys the game assigns (Input2.ini [AssignKeyboard]) that are
// down, as the game's pad-key bits (render thread).
uint32_t g_keyPad = 0;

// Input2.ini's [AssignKeyboard] into ui::SetKeyboardMap, again every 2 s (the
// game's own key settings write it).
void LoadKeyboardMap() {
    static ULONGLONG readAt = 0;
    if (readAt && GetTickCount64() - readAt < 2000) return;
    readAt = GetTickCount64();
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    const std::wstring path = std::wstring(local) + L"\\StreetFighterV\\Saved\\Config\\WindowsNoEditor\\Input2.ini";
    // The game's defaults when the file has no such key.
    static const wchar_t* const kDefaults[kKeyboardSlots] = {L"W", L"S", L"D", L"A", L"B", L"N", L"G", L"H",
                                                             L"K", L"J", L"Comma", L"M", L"Period", L"Slash", L"Enter", L"Escape"};
    int vk[kKeyboardSlots] = {};
    std::string names;
    for (int slot = 0; slot < kKeyboardSlots; slot++) {
        wchar_t value[64] = {};
        GetPrivateProfileStringW(L"AssignKeyboard", (L"KeyboardKeys_" + std::to_wstring(slot)).c_str(), kDefaults[slot], value, 64, path.c_str());
        std::string name;
        for (const wchar_t* c = value; *c; c++) name += static_cast<char>(*c < 128 ? *c : '?');
        vk[slot] = VirtualKeyOf(name);
        names += (slot ? " " : "") + name;
    }
    static std::string logged;
    if (names != logged) {
        logged = names;
        Log("overlay: the game's keyboard keys: %s", names.c_str());
    }
    ui::SetKeyboardMap(vk);
}

void FeedPads(ImGuiIO& io) {
    uint16_t buttons = 0;
    int lx = 0, ly = 0, lt = 0, rt = 0;
    for (const PadState& p : g_pads) {
        buttons |= p.buttons.load();
        lt = std::max<int>(lt, p.lt.load());
        rt = std::max<int>(rt, p.rt.load());
        if (std::abs(p.lx.load()) > std::abs(lx)) lx = p.lx;
        if (std::abs(p.ly.load()) > std::abs(ly)) ly = p.ly;
    }

    struct Map {
        uint16_t button;
        ImGuiKey key;
    };

    static const Map kButtons[] = {
        {XINPUT_GAMEPAD_A, ImGuiKey_GamepadFaceDown},
        {XINPUT_GAMEPAD_B, ImGuiKey_GamepadFaceRight},
        {XINPUT_GAMEPAD_X, ImGuiKey_GamepadFaceLeft},
        {XINPUT_GAMEPAD_Y, ImGuiKey_GamepadFaceUp},
        {XINPUT_GAMEPAD_DPAD_UP, ImGuiKey_GamepadDpadUp},
        {XINPUT_GAMEPAD_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
        {XINPUT_GAMEPAD_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
        {XINPUT_GAMEPAD_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, ImGuiKey_GamepadL1},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1},
        {XINPUT_GAMEPAD_START, ImGuiKey_GamepadStart},
        {XINPUT_GAMEPAD_BACK, ImGuiKey_GamepadBack},
    };

    // The game's own pad keys too: a pad it reads past XInput (DirectInput,
    // Steam Input) gave the overlay nothing (control.h InputGamePadKeys).
    struct GameKey {
        uint32_t key;
        uint16_t button;
    };

    static const GameKey kGameKeys[] = {
        {0x1, XINPUT_GAMEPAD_DPAD_UP},
        {0x2, XINPUT_GAMEPAD_DPAD_DOWN},
        {0x4, XINPUT_GAMEPAD_DPAD_LEFT},
        {0x8, XINPUT_GAMEPAD_DPAD_RIGHT},
        {0x10, XINPUT_GAMEPAD_X},
        {0x20, XINPUT_GAMEPAD_Y},
        {0x40, XINPUT_GAMEPAD_A},
        {0x80, XINPUT_GAMEPAD_B},
        {0x100, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {0x200, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {0x4000, XINPUT_GAMEPAD_START},
        {0x8000, XINPUT_GAMEPAD_BACK},
    };
    // And the keyboard's keys as the game assigns them (the screens eat the
    // keyboard, so the game's own reading of it sees nothing then).
    const uint32_t keys = InputGamePadKeys() | g_keyPad;
    uint16_t game = 0;
    for (const GameKey& k : kGameKeys) {
        if (keys & k.key) game |= k.button;
    }
    buttons |= game;
    // LT and RT as the game's keys (a keyboard, a pad read past XInput).
    if (keys & 0x400) lt = 255;
    if (keys & 0x800) rt = 255;
    // Back + Start (or F1): at the game's menus the hub; in a match the
    // leave modal (again: it closes); watching, the watch panel.
    constexpr uint16_t kChord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    static bool chordWas = false, chordInPress = false;
    static ULONGLONG chordAt = 0;
    const bool chord = (buttons & kChord) == kChord;
    // Once per press: not again until both are let go, nor within 300 ms
    // (a pad read that drops a frame of the press would toggle it back).
    const bool fresh = chord && !chordWas && !chordInPress && GetTickCount64() - chordAt > 300;
    if (fresh || g_chordKey.exchange(false)) {
        chordAt = GetTickCount64();
        chordInPress = chord;
        if (g_visible) {
            Log("overlay: Back + Start again: the leave modal closes");
            SetOverlayVisible(false);
        } else if (g_hubAllowed && !HubOpen()) {
            Log("overlay: Back + Start: the hub");
            OpenHub();
        } else if (g_spectating) g_watchPanelTap = true;
        else if (g_inMatch) SetOverlayVisible(true);
    }
    chordWas = chord;
    // Watching: a tap of Start alone (pressed and let go within kTapMs)
    // opens or closes the watch panel (the players' tap skips intros; a
    // spectator's does nothing else).
    constexpr ULONGLONG kTapMs = 450;
    static bool startWas = false;
    static ULONGLONG startDownAt = 0;
    const bool start = buttons & XINPUT_GAMEPAD_START;
    if (start && !startWas) startDownAt = GetTickCount64();
    if (!start && startWas && !chordInPress && g_spectating && !g_visible && GetTickCount64() - startDownAt < kTapMs) g_watchPanelTap = true;
    // Watching: a tap of Back alone shows or hides the room chat; Back + Y
    // starts typing into it (watch_chat.h).
    static bool backWas = false, backWithOther = false, yWas = false;
    static ULONGLONG backDownAt = 0;
    const bool back = buttons & XINPUT_GAMEPAD_BACK, yNow = buttons & XINPUT_GAMEPAD_Y;
    if (back && !backWas) {
        backDownAt = GetTickCount64();
        backWithOther = false;
    }
    if (back && (buttons & static_cast<uint16_t>(~XINPUT_GAMEPAD_BACK))) backWithOther = true;
    if (back && yNow && !yWas && g_spectating && !g_visible) WatchChatStartTyping();
    if (!back && backWas && !backWithOther && !chordInPress && g_spectating && !g_visible && GetTickCount64() - backDownAt < kTapMs) {
        WatchChatToggle();
    }
    backWas = back;
    yWas = yNow;
    if (!start && !(buttons & XINPUT_GAMEPAD_BACK)) chordInPress = false;
    startWas = start;
    // The screen the chord opened does not see its Start (the hub's Start
    // is Polarity settings) until both are let go.
    if (chordInPress) buttons &= static_cast<uint16_t>(~kChord);
    for (const Map& m : kButtons) io.AddKeyEvent(m.key, (buttons & m.button) != 0);
    constexpr float kDead = 7849.0f; // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
    auto axis = [&](ImGuiKey key, float v) {
        const float a = std::clamp((v - kDead) / (32767.0f - kDead), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, a > 0.1f, a);
    };
    axis(ImGuiKey_GamepadLStickLeft, -static_cast<float>(lx));
    axis(ImGuiKey_GamepadLStickRight, static_cast<float>(lx));
    axis(ImGuiKey_GamepadLStickUp, static_cast<float>(ly));
    axis(ImGuiKey_GamepadLStickDown, -static_cast<float>(ly));
    // The triggers: down past XINPUT_GAMEPAD_TRIGGER_THRESHOLD.
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, lt > 30, lt / 255.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, rt > 30, rt / 255.0f);
}

void ReleaseTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateTarget(IDXGISwapChain* swapChain) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
    const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return SUCCEEDED(hr);
}

void ReleaseImGui() {
    if (g_imgui) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui = false;
    }
    ReleaseTarget();
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
    g_swapChain = nullptr;
    g_inputWindow = nullptr;
}

// ----- the look: the game's menus ------------------------------------------
// From SFV's main menu: near-black bars, each entry a light warm gray tile
// with dark gray text and a cream outline, the focused one dark olive with
// gold text over a gold rule.
const ImVec4 kGold(1.00f, 0.89f, 0.55f, 1.0f);     // #FFE38C, focused text and titles
const ImVec4 kGoldLine(0.85f, 0.72f, 0.36f, 1.0f); // #D8B85B, rules
const ImVec4 kCream(0.99f, 0.98f, 0.91f, 1.0f);    // #FCFBE7, tile outlines
const ImVec4 kOlive(0.22f, 0.22f, 0.17f, 1.0f);    // #39392B, the focused tile
const ImVec4 kP1(0.36f, 0.62f, 1.0f, 1.0f);
const ImVec4 kP2(1.0f, 0.36f, 0.30f, 1.0f);
const ImVec4 kRed(1.0f, 0.36f, 0.30f, 1.0f);
const ImVec4 kGreen(0.55f, 0.86f, 0.45f, 1.0f);
const ImVec4 kDim(0.66f, 0.65f, 0.59f, 1.0f);

float g_uiScale = 1.0f;                // sizes: 1 at 720 lines (render thread)
float g_builtScale = 0.0f;             // what the fonts were made for
ImFont* g_fontHeading = nullptr;       // bold italic, as the menus' entries
ImFont* g_fontBig = nullptr;           // match codes
ImFont* g_fontSmall = nullptr;         // the player bars' details
ImFont* g_fontTiny = nullptr;          // ... their frame rate and rollback numbers
ImFont* g_fontBold = nullptr;          // Barlow Bold: the screens' names, buttons, focused rows
ImFont* g_fontHuge = nullptr;          // the select screen's character names
ImFont* g_fontBody = nullptr;          // the default font
std::atomic<int> g_selectRequest{-1};  // RequestSelectPreview: the phase to open at (+4: watching)
std::atomic<bool> g_selectLive{false}; // a session's select screen is up (or held over the jump) and takes input

float UiScaleFor(float height) { return std::max(1.0f, height / 720.0f); }

ImVec4 Rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

void ApplyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    // Square corners and thin outlines, as the game's tiles.
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(9, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.WindowBorderSize = s.FrameBorderSize = s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0;
    s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0;
    s.ScrollbarSize = 12;
    s.GrabMinSize = 12;
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = Rgb(242, 240, 230);
    c[ImGuiCol_TextDisabled] = Rgb(140, 138, 126);
    c[ImGuiCol_WindowBg] = Rgb(20, 20, 18, 0.97f);
    c[ImGuiCol_ChildBg] = Rgb(0, 0, 0, 0.25f);
    c[ImGuiCol_PopupBg] = Rgb(26, 26, 22, 0.98f);
    c[ImGuiCol_Border] = Rgb(216, 212, 178, 0.45f);
    c[ImGuiCol_BorderShadow] = Rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = Rgb(42, 42, 34);
    c[ImGuiCol_FrameBgHovered] = Rgb(57, 57, 43);
    c[ImGuiCol_FrameBgActive] = Rgb(72, 71, 55);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = Rgb(10, 10, 10);
    c[ImGuiCol_MenuBarBg] = Rgb(10, 10, 10);
    c[ImGuiCol_ScrollbarBg] = Rgb(10, 10, 10, 0.5f);
    c[ImGuiCol_ScrollbarGrab] = Rgb(90, 89, 72);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(120, 118, 95);
    c[ImGuiCol_ScrollbarGrabActive] = kGoldLine;
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = kGoldLine;
    c[ImGuiCol_SliderGrabActive] = kGold;
    c[ImGuiCol_Button] = Rgb(46, 46, 37);
    c[ImGuiCol_ButtonHovered] = Rgb(57, 57, 43);
    c[ImGuiCol_ButtonActive] = Rgb(72, 71, 55);
    c[ImGuiCol_Header] = Rgb(57, 57, 43);
    c[ImGuiCol_HeaderHovered] = Rgb(72, 71, 55);
    c[ImGuiCol_HeaderActive] = Rgb(88, 86, 66);
    c[ImGuiCol_Separator] = Rgb(216, 184, 91, 0.45f);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = kGoldLine;
    c[ImGuiCol_ResizeGrip] = Rgb(216, 184, 91, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = Rgb(216, 184, 91, 0.6f);
    c[ImGuiCol_ResizeGripActive] = kGoldLine;
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = Rgb(30, 30, 25);
    c[ImGuiCol_TabHovered] = Rgb(72, 71, 55);
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = kOlive;
    c[ImGuiCol_TabSelectedOverline] = kGoldLine;
    c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(216, 184, 91, 0.5f);
    c[ImGuiCol_TableHeaderBg] = Rgb(30, 30, 25);
    c[ImGuiCol_TableBorderStrong] = Rgb(216, 212, 178, 0.35f);
    c[ImGuiCol_TableBorderLight] = Rgb(216, 212, 178, 0.18f);
    c[ImGuiCol_TableRowBg] = Rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Rgb(255, 255, 255, 0.03f);
    c[ImGuiCol_TextSelectedBg] = Rgb(216, 184, 91, 0.35f);
    c[ImGuiCol_TextLink] = kGold;
    c[ImGuiCol_DragDropTarget] = kGold;
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_NavWindowingHighlight] = kGold;
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = Rgb(0, 0, 0, 0.5f);
}

// The DLL's own copy of an embedded font (fonts.rc), as long as the DLL is
// loaded; empty if missing.
std::pair<void*, int> EmbeddedFont(int id) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&EmbeddedFont), &self);
    HRSRC found = self ? FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)) : nullptr; // RT_RCDATA
    HGLOBAL loaded = found ? LoadResource(self, found) : nullptr;
    void* data = loaded ? LockResource(loaded) : nullptr;
    return {data, data ? static_cast<int>(SizeofResource(self, found)) : 0};
}

// The bundled CJK font (Polarity\fonts\NotoSansCJK-Medium.ttc, read once and
// kept: the atlas only points at it), and its face for Windows' display
// language: the Han characters Chinese, Japanese and Korean share take the
// player's own shapes (a Chinese system drew them in Japanese ones once).
// Faces: 0 JP, 1 KR, 2 SC, 3 TC, 4 HK.
const std::vector<uint8_t>& CjkFont() {
    static const std::vector<uint8_t> data = [] {
        std::vector<uint8_t> out;
        const std::wstring path = DataDir() + L"\\fonts\\NotoSansCJK-Medium.ttc";
        if (FILE* f = _wfopen(path.c_str(), L"rb")) {
            std::fseek(f, 0, SEEK_END);
            out.resize(static_cast<size_t>(std::max(0L, std::ftell(f))));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
            std::fclose(f);
        }
        Log("overlay: CJK font %s", out.empty() ? "missing (names in Chinese, Japanese or Korean show boxes)" : "loaded");
        return out;
    }();
    return data;
}

int CjkFace() {
    const LANGID lang = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(lang) == LANG_KOREAN) return 1;
    if (PRIMARYLANGID(lang) != LANG_CHINESE) return 0;
    const WORD sub = SUBLANGID(lang);
    if (sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU) return 4;
    return sub == SUBLANG_CHINESE_TRADITIONAL ? 3 : 2;
}

// Fonts for the screen's size (built again when it changes): Barlow for
// text, its bold italic for titles and tiles (as the menus' bold italic
// entries), Archivo Black for the select screen's names, all embedded, so
// they look the same on Windows and Proton. Each takes the characters it
// lacks from the bundled Noto Sans CJK; ImGui 1.92 makes glyphs as text
// needs them, so names in any of its scripts draw without a rebuild.
void BuildFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const std::vector<uint8_t>& cjk = CjkFont();
    const int face = CjkFace();
    auto add = [&](int id, float px) -> ImFont* {
        const auto [data, size] = EmbeddedFont(id);
        if (!data) return nullptr;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 2;
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(data, size, px * scale, &cfg);
        if (font && !cjk.empty()) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.FontDataOwnedByAtlas = false;
            merge.FontNo = face;
            io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(cjk.data()), static_cast<int>(cjk.size()), px * scale, &merge);
        }
        return font;
    };
    ImFont* body = add(SFV_FONT_BARLOW_REGULAR, 17);
    if (!body) {
        ImFontConfig cfg;
        cfg.SizePixels = 13 * scale;
        body = io.Fonts->AddFontDefault(&cfg);
    }
    g_fontHeading = add(SFV_FONT_BARLOW_BOLD_ITALIC, 20);
    if (!g_fontHeading) g_fontHeading = body;
    g_fontBig = add(SFV_FONT_BARLOW_SEMIBOLD, 40);
    if (!g_fontBig) g_fontBig = body;
    g_fontSmall = add(SFV_FONT_BARLOW_REGULAR, 14);
    if (!g_fontSmall) g_fontSmall = body;
    g_fontTiny = add(SFV_FONT_BARLOW_SEMIBOLD, 12);
    if (!g_fontTiny) g_fontTiny = g_fontSmall;
    g_fontHuge = add(SFV_FONT_ARCHIVO_BLACK, 64);
    if (!g_fontHuge) g_fontHuge = g_fontBig;
    g_fontBold = add(SFV_FONT_BARLOW_BOLD, 21);
    if (!g_fontBold) g_fontBold = g_fontBig;
    g_fontBody = body;
    io.FontDefault = body;
    ApplyTheme(scale);
    g_builtScale = g_uiScale = scale;
}

std::string Upper(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

// Latency at a glance, by the delay the link suggests (ui::PingColor).
ImVec4 PingColorVec(unsigned ms, unsigned jitterMs) {
    return ImGui::ColorConvertU32ToFloat4(ui::PingColor(ms, jitterMs));
}

// Relayed links add the relay's detour: shown in yellow, a direct one dim.
ImVec4 PathColor(const NetView& v) { return v.linkPath == NetView::Path::Relayed ? Rgb(255, 210, 80) : kDim; }

// A connection's icon in a square of `size` from `at`: Wi-Fi's arcs over a
// dot, or a wired network plug; nothing when unknown.
void DrawLinkIcon(ImDrawList* d, ImVec2 at, float size, lobby::Link link, ImU32 color) {
    const float line = std::max(1.0f, size * 0.11f);
    if (link == lobby::Link::WiFi) {
        const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.86f);
        constexpr float kPi = 3.14159265f;
        for (float r : {0.3f, 0.55f, 0.8f}) {
            d->PathArcTo(c, size * r, -kPi * 0.75f, -kPi * 0.25f, 12);
            d->PathStroke(color, ImDrawFlags_None, line);
        }
        d->AddCircleFilled(c, size * 0.09f, color);
    } else if (link == lobby::Link::Wired) {
        // A plug's head with its contacts, and the cable below it.
        const ImVec2 a(at.x + size * 0.2f, at.y + size * 0.12f), b(at.x + size * 0.8f, at.y + size * 0.62f);
        d->AddRect(a, b, color, 0, 0, line);
        for (float x : {0.38f, 0.5f, 0.62f}) {
            d->AddLine(ImVec2(at.x + size * x, a.y + size * 0.1f), ImVec2(at.x + size * x, a.y + size * 0.26f), color, line * 0.8f);
        }
        d->AddRectFilled(ImVec2(at.x + size * 0.36f, b.y), ImVec2(at.x + size * 0.64f, b.y + size * 0.12f), color);
        d->AddLine(ImVec2(at.x + size * 0.5f, b.y + size * 0.12f), ImVec2(at.x + size * 0.5f, at.y + size * 0.98f), color, line);
    }
}

// A relayed link's icon in a square of `size` from `at`: a cloud, the relay
// (Cloudflare's) the datagrams pass through.
void DrawRelayIcon(ImDrawList* d, ImVec2 at, float size, ImU32 color) {
    auto p = [&](float x, float y) { return ImVec2(at.x + size * x, at.y + size * y); };
    d->AddRectFilled(p(0.12f, 0.56f), p(0.88f, 0.84f), color, size * 0.14f);
    d->AddCircleFilled(p(0.33f, 0.58f), size * 0.2f, color);
    d->AddCircleFilled(p(0.56f, 0.46f), size * 0.27f, color);
    d->AddCircleFilled(p(0.76f, 0.62f), size * 0.19f, color);
}

// "I", "II", "III": how the menus number V-Skills and V-Triggers.
std::string Roman(int n) {
    static const char* kRoman[] = {"I", "II", "III", "IV", "V"};
    return n >= 1 && n <= 5 ? kRoman[n - 1] : std::to_string(n);
}

// The other player's frame rate averaged over the last 3 s: it moves
// around 60 as rollback catches up or the game slows to stay in sync.
int SmoothedFps(int seat, int fps) {
    struct Sample {
        ULONGLONG at;
        int fps;
    };

    static std::vector<Sample> samples[kNumPlayers];
    auto& s = samples[seat];
    const ULONGLONG now = GetTickCount64();
    if (fps > 0) s.push_back({now, fps});
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [now](const Sample& x) { return now - x.at <= 3000; }));
    if (s.empty()) return fps;
    double sum = 0;
    for (const Sample& x : s) sum += x.fps;
    return static_cast<int>(sum / s.size() + 0.5);
}

// First Present of a swap chain: ImGui for its device and window.
bool SetUp(IDXGISwapChain* swapChain) {
    ReleaseImGui();
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return false;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) {
        Log("overlay: the swap chain has no D3D11 device");
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    NoteShaderResourceViewKind(g_device);
    SetPakTextureDevice(g_device);
    g_window = desc.OutputWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing written next to the game
    // NoMouseCursorChange: the backend would set the system cursor from the
    // render thread, which does not own the game's window.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplWin32_Init(g_window);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad; // fed by FeedPads
    ImGui_ImplDX11_Init(g_device, g_context);
    BuildFonts(UiScaleFor(static_cast<float>(desc.BufferDesc.Height)));
    g_imgui = true;
    g_swapChain = swapChain;
    g_inputWindow = g_window;
    Log("overlay: set up on swap chain %p (window %p, %ux%u)", static_cast<void*>(swapChain), static_cast<void*>(g_window),
        desc.BufferDesc.Width, desc.BufferDesc.Height);
    return true;
}

// Drawing the Debug tab's previews of the match windows (render thread):
// their buttons and choices then act on nothing.
bool g_previewing = false;

// Runs a netplay action off the render thread (hosting and joining open
// sockets, leaving waits for threads).
void Act(std::function<void()> action) {
    if (g_previewing) return;
    std::thread([action = std::move(action)] {
        ThreadGuard running;
        action();
    }).detach();
}

// The game's character and stage tables, read once they exist (validating
// memory is too slow to repeat every frame).
struct Tables {
    std::vector<std::string> charas; // playable characters
    std::vector<int> charaIndex;     // each one's index in the game's character table
    std::vector<std::string> labels; // "CHUN-LI"
    // Per character, the costumes the game's menus offer (indices into its
    // costume list, in its order), each one's name and its color ids (the
    // game's CharaSpec table: 0.. its colors, 100.. its EX colors).
    std::vector<std::vector<int>> costumes;
    std::vector<std::vector<std::string>> costumeNames;
    std::vector<std::vector<std::vector<int>>> colors;
    std::vector<std::string> stages;                     // the stage select's stages
    std::vector<std::string> stageLabels;                // "Bustling Side Street"
    std::vector<std::vector<std::string>> settings;      // per stage: its settings' stage codes (The Grid: S25)
    std::vector<std::vector<std::string>> settingLabels; // their names ("The Grid Alternative")
    std::vector<std::vector<std::string>> vskills;       // per character, the game's names
    std::vector<std::vector<std::string>> vtriggers;
    std::vector<std::vector<std::string>> vcomments;    // the V-Comments after Auto
    std::vector<int> elevenCostume;                     // the 0C costume Eleven wears in each fighter (-1: none)
    std::vector<std::vector<CostumeSwatches>> swatches; // per costume (as listed): the colors' swatches
    // What this profile owns (entitlements.h), as the game's own menus offer
    // it: per character, per costume (as listed), per color (as listed); per
    // stage and per setting. The lists stay whole (the other player's pick
    // shows from them); only this player's choices skip what is not owned.
    std::vector<bool> charaOwned;
    std::vector<std::vector<bool>> costumeOwned;
    std::vector<std::vector<std::vector<bool>>> colorOwned;
    std::vector<bool> stageOwned;
    std::vector<std::vector<bool>> settingOwned;
    bool loaded = false;
};

Tables g_tables;

// Reads the tables on a worker thread: the names walk every object (seconds),
// which on the render thread froze the game.
Tables ReadTables() {
    Tables t;
    for (int i = 0; i < CharaCount() && !Stopping(); i++) {
        // A fighter whose name has loaded (listed by name).
        const std::string name = CharaName(i);
        if (!IsFighter(i) || name.empty()) continue;
        std::vector<int> costumes;
        std::vector<std::string> names;
        std::vector<std::vector<int>> colors;
        std::vector<bool> costumeOwned;
        std::vector<std::vector<bool>> colorOwned;
        const std::map<int, CostumeSpec> specs = CharaCostumeSpecs(i);
        std::string charaCode;
        for (const auto& [k, spec] : specs) {
            if (charaCode.empty()) charaCode = spec.charaCode;
        }
        for (int k = 0; k < CharaCostumeCount(i); k++) {
            const int n = CostumeColorCount(i, k);
            if (n <= 0) continue;
            costumes.push_back(k);
            const auto spec = specs.find(k);
            const bool named = spec != specs.end() && !spec->second.name.empty() && spec->second.name.rfind("ID_", 0) != 0;
            names.push_back(named ? spec->second.name : "Costume " + std::to_string(costumes.size()));
            std::vector<int> ids = spec != specs.end() ? spec->second.colors : std::vector<int>{};
            if (ids.empty()) {
                for (int c = 0; c < n; c++) ids.push_back(c);
            }
            costumeOwned.push_back(spec == specs.end() || OwnsEntitlement(spec->second.costumeCode));
            std::vector<bool> owned;
            for (int id : ids) {
                bool has = true;
                if (spec != specs.end()) {
                    const auto code = spec->second.colorCodes.find(id);
                    has = code == spec->second.colorCodes.end() || OwnsEntitlement(code->second);
                }
                owned.push_back(has);
            }
            colorOwned.push_back(std::move(owned));
            colors.push_back(std::move(ids));
        }
        if (costumes.empty()) continue;
        t.charaOwned.push_back(OwnsEntitlement(charaCode));
        t.costumeOwned.push_back(std::move(costumeOwned));
        t.colorOwned.push_back(std::move(colorOwned));
        std::vector<CostumeSwatches> swatches;
        for (int k : costumes) swatches.push_back(CostumeSwatchColors(i, k));
        t.swatches.push_back(std::move(swatches));
        t.vcomments.emplace_back(); // read after the rest (ReadComments)
        t.elevenCostume.push_back(CostumeInFolder(i, "0C"));
        t.charas.push_back(CharaCode(i));
        t.charaIndex.push_back(i);
        t.labels.push_back(name);
        t.costumes.push_back(std::move(costumes));
        t.costumeNames.push_back(std::move(names));
        t.colors.push_back(std::move(colors));
        t.vskills.push_back(SkillNames(CharaCode(i), false));
        t.vtriggers.push_back(SkillNames(CharaCode(i), true));
    }
    // Alphabetical by name (the game's table is in release order).
    std::vector<size_t> order(t.charas.size());
    for (size_t k = 0; k < order.size(); k++) order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return std::lexicographical_compare(t.labels[a].begin(), t.labels[a].end(), t.labels[b].begin(), t.labels[b].end(),
                                            [](char x, char y) {
                                                return std::tolower(static_cast<unsigned char>(x)) <
                                                       std::tolower(static_cast<unsigned char>(y));
                                            });
    });
    auto reorder = [&order](auto& v) {
        auto sorted = v;
        for (size_t k = 0; k < order.size(); k++) sorted[k] = std::move(v[order[k]]);
        v = std::move(sorted);
    };
    reorder(t.charas);
    reorder(t.charaIndex);
    reorder(t.labels);
    reorder(t.costumes);
    reorder(t.costumeNames);
    reorder(t.colors);
    reorder(t.vskills);
    reorder(t.vtriggers);
    reorder(t.vcomments);
    reorder(t.elevenCostume);
    reorder(t.swatches);
    reorder(t.charaOwned);
    reorder(t.costumeOwned);
    reorder(t.colorOwned);
    // Eleven: random select in the silver model (each fighter's 0C costume).
    // The player's own game picks the fighter (ChosenOptions), so the pick
    // others see is an ordinary one; labels show it as Eleven.
    if (const int eleven = CharaIndex("ELV"); eleven >= 0) {
        t.charas.push_back("ELV");
        t.charaIndex.push_back(eleven);
        t.labels.push_back("ELEVEN");
        t.costumes.push_back({0});
        t.costumeNames.push_back({"Silver"});
        t.colors.push_back({{0}});
        t.vskills.emplace_back();
        t.vtriggers.emplace_back();
        t.vcomments.emplace_back();
        t.elevenCostume.push_back(-1);
        t.swatches.push_back({CostumeSwatches{}});
        t.charaOwned.push_back(true); // everyone has Eleven
        t.costumeOwned.push_back({true});
        t.colorOwned.push_back({{true}});
    }
    for (int i = 0; i < StageCount() && !Stopping(); i++) {
        if (!IsMenuStage(i)) continue;
        t.stages.push_back(StageCode(i));
        const std::string name = StageName(i);
        t.stageLabels.push_back(name.empty() ? StageCode(i) : name);
        t.stageOwned.push_back(OwnsEntitlement(StageAvailableCode(i)));
        std::vector<std::string> codes, names;
        std::vector<bool> owned;
        for (int k : StageSettings(i)) {
            if (!IsPlayableStage(k)) continue;
            codes.push_back(StageCode(k));
            names.push_back(StageName(k).empty() ? StageCode(k) : StageName(k));
            owned.push_back(OwnsEntitlement(StageAvailableCode(k)));
        }
        t.settings.push_back(std::move(codes));
        t.settingLabels.push_back(std::move(names));
        t.settingOwned.push_back(std::move(owned));
    }
    t.loaded = !t.charas.empty() && !t.stages.empty();
    if (t.loaded) {
        int charas = 0, stages = 0, costumes = 0, costumesOwned = 0, colors = 0, colorsOwned = 0;
        std::string lockedCharas, lockedStages;
        for (size_t i = 0; i < t.charas.size(); i++) {
            charas += t.charaOwned[i];
            if (!t.charaOwned[i]) lockedCharas += " " + t.charas[i];
            for (bool b : t.costumeOwned[i]) costumes++, costumesOwned += b;
            for (const auto& v : t.colorOwned[i]) {
                for (bool b : v) colors++, colorsOwned += b;
            }
        }
        for (size_t i = 0; i < t.stages.size(); i++) {
            stages += t.stageOwned[i];
            if (!t.stageOwned[i]) lockedStages += " " + t.stages[i];
        }
        Log("overlay: owned (%s): %d of %zu characters, %d of %d costumes, %d of %d colors, %d of %zu stages; locked:%s; stages:%s",
            EntitlementsKnown() ? "the profile's list" : "no list read: all open", charas, t.charas.size(), costumesOwned, costumes,
            colorsOwned, colors, stages, t.stages.size(), lockedCharas.empty() ? " none" : lockedCharas.c_str(),
            lockedStages.empty() ? " none" : lockedStages.c_str());
    }
    return t;
}

std::mutex g_tablesMutex;
std::unique_ptr<Tables> g_readTables; // from the worker, taken by the render thread
std::atomic<bool> g_readingTables{false};
// The V-Comment names by character code, read once the tables are there:
// each loads the character's subtitle asset on the game thread (about 0.4 s
// a character, 18 s for the roster), so the lists, the hub and the rooms do
// not wait for them. Until they come, the V-Comment row says "Comment 2".
std::unique_ptr<std::map<std::string, std::vector<std::string>>> g_readComments;

void ReadComments(std::vector<std::string> codes) {
    Act([codes = std::move(codes)] {
        const ULONGLONG from = GetTickCount64();
        auto out = std::make_unique<std::map<std::string, std::vector<std::string>>>();
        for (const std::string& code : codes) {
            if (Stopping()) return;
            if (code != "ELV") (*out)[code] = VCommentNames(code);
        }
        Log("overlay: V-Comment names read in %llu ms", GetTickCount64() - from);
        std::lock_guard lock(g_tablesMutex);
        g_readComments = std::move(out);
    });
}

// Render thread: starts the read, and takes its result once there.
void LoadTables() {
    if (g_tables.loaded) {
        std::lock_guard lock(g_tablesMutex);
        if (g_readComments) {
            for (size_t i = 0; i < g_tables.charas.size() && i < g_tables.vcomments.size(); i++) {
                const auto it = g_readComments->find(g_tables.charas[i]);
                if (it != g_readComments->end()) g_tables.vcomments[i] = it->second;
            }
            g_readComments.reset();
        }
        return;
    }
    // Not while the game is still starting (boot, title): its character data
    // is only partly there, and a read then came out short but not empty and
    // stayed (a spectator who joined from the title saw "KEN", "Z34",
    // "costume 0", V-Skills without names for the whole session).
    const std::string scene = CurrentScene();
    if (scene.empty() || scene == "BootState" || scene == "InitialState") return;
    // Every stage's name first (Ring of Arcade's data is loaded for it).
    if (!StageNamesReady()) return;
    {
        std::lock_guard lock(g_tablesMutex);
        if (g_readTables) {
            if (g_readTables->loaded) {
                g_tables = std::move(*g_readTables);
                Log("overlay: character lists read: %zu characters, %zu stages (scene %s)", g_tables.charas.size(),
                    g_tables.stages.size(), CurrentScene().c_str());
                ReadComments(g_tables.charas);
                LoungeCommentText(0); // the room titles' table, read on its own thread before the browser asks
            }
            g_readTables.reset();
            g_readingTables = false; // not loaded yet (game still starting): read again later
            return;
        }
    }
    if (g_readingTables.exchange(true)) return;
    Act([] {
        static int tries = 0;
        const ULONGLONG from = GetTickCount64();
        auto t = std::make_unique<Tables>(ReadTables());
        tries++;
        Log("overlay: character lists %s in %llu ms (try %d, scene %s)", t->loaded ? "read" : "not there yet", GetTickCount64() - from, tries,
            CurrentScene().c_str());
        if (!t->loaded) Sleep(1000); // the game has not loaded them yet
        std::lock_guard lock(g_tablesMutex);
        g_readTables = std::move(t);
    });
}

// Where a costume (the game's index) is in the character's offered list; -1
// when it is not offered.
int CostumeSlot(int chara, int costume) {
    const std::vector<int>& costumes = g_tables.costumes[chara];
    const auto it = std::find(costumes.begin(), costumes.end(), costume);
    return it == costumes.end() ? -1 : static_cast<int>(it - costumes.begin());
}

// Ownership (Tables::charaOwned and the rest): what this player may pick.
bool CharaOwned(int chara) {
    return chara >= 0 && chara < static_cast<int>(g_tables.charaOwned.size()) && g_tables.charaOwned[chara];
}

bool CostumeOwned(int chara, int slot) {
    if (chara < 0 || chara >= static_cast<int>(g_tables.costumeOwned.size())) return false;
    const auto& v = g_tables.costumeOwned[chara];
    return slot >= 0 && slot < static_cast<int>(v.size()) && v[slot];
}

// A color by its place in the costume's list.
bool ColorOwnedAt(int chara, int slot, int at) {
    if (chara < 0 || chara >= static_cast<int>(g_tables.colorOwned.size())) return false;
    const auto& costumes = g_tables.colorOwned[chara];
    if (slot < 0 || slot >= static_cast<int>(costumes.size())) return false;
    return at >= 0 && at < static_cast<int>(costumes[slot].size()) && costumes[slot][at];
}

// A stage select stage (into Tables::stages), itself (setting 0) or one of its settings.
bool StageOwned(int stage, int setting) {
    if (stage < 0 || stage >= static_cast<int>(g_tables.stageOwned.size())) return false;
    if (setting <= 0) return g_tables.stageOwned[stage];
    const auto& s = g_tables.settingOwned[stage];
    return setting - 1 < static_cast<int>(s.size()) && s[setting - 1];
}

// What the player chose in the panel; applied to the netplay options when
// hosting or joining (and kept there for F7/F8).
struct Choices {
    bool initialised = false;
    bool pick = false;
    int chara = 0;
    int costume = 0;
    int color = 0;     // the game's color id: 0.. the costume's colors, 100.. its EX colors
    int vskill = 1;    // from 1
    int vtrigger = 1;  // from 1
    int variant = 0;   // the costume's variant (dress state), 0: none
    int comment = 0;   // V-Comment: 0 Auto, n the n-th
    int elevenAs = -1; // Eleven: the fighter it copies (a table index), rolled when picked
    int stage = 1;     // 1 random, 2.. a stage
    int setting = 0;   // 0 the stage's default, n its n-th setting
    int delay = 2;
    char code[64] = "";
    char name[lobby::kMaxName + 1] = "";
    bool allowSpectators = true;
    bool uploadReports = true;
    bool showFps = false;
    bool shareCountry = true;
    bool streamerMode = false;
    bool wordFilter = true;
};

Choices g_choices;
// My character's Random stages: the stage codes the host's random leaves
// out (core/random_stage.h), kept apart from the pick (Keep on the grid
// does not undo them); saved once the player has a list.
std::vector<std::string> g_randomOff;
bool g_randomOffSaved = false;
bool IsEleven(int chara);
void RollEleven(Choices& c);
PlayerSetup ElevenPick(Choices& c);
bool IsElevenSetup(const PlayerSetup& p);
std::string CommentText(const std::string& code, int comment);
// Each character's loadout as last saved with it, and as left in the panel
// this run (by code): picking a character again brings its loadout back.
std::map<std::string, PlayerSetup> g_loadouts;

void InitChoices() {
    if (g_choices.initialised || !g_tables.loaded) return;
    if (!g_panelPath.empty()) g_loadouts = ReadLoadouts(g_panelPath);
    const NetplayOptions o = NetOptions();
    // A session always plays a pick (there is no character select to fall
    // back on): the saved one, else the first character.
    g_choices.pick = true;
    if (o.hasPick) {
        const std::string code = UnpackCode(o.pick.character);
        const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
        if (it != g_tables.charas.end()) g_choices.chara = static_cast<int>(it - g_tables.charas.begin());
        g_choices.costume = o.pick.costume;
        g_choices.color = o.pick.color;
        g_choices.vskill = o.pick.vskill + 1;
        g_choices.vtrigger = o.pick.vtrigger + 1;
        g_choices.variant = o.pick.variation;
        g_choices.comment = o.pick.comment;
    }
    // A saved stage is a stage select stage or one of its settings.
    g_choices.stage = 1;
    for (int i = 0; i < static_cast<int>(g_tables.stages.size()) && !o.stageCode.empty(); i++) {
        if (g_tables.stages[i] == o.stageCode) g_choices.stage = 2 + i;
        const auto& settings = g_tables.settings[i];
        const auto it = std::find(settings.begin(), settings.end(), o.stageCode);
        if (it != settings.end()) {
            g_choices.stage = 2 + i;
            g_choices.setting = 1 + static_cast<int>(it - settings.begin());
        }
    }
    g_choices.delay = o.inputDelay;
    std::snprintf(g_choices.name, sizeof(g_choices.name), "%s", o.name.c_str());
    g_choices.allowSpectators = o.allowSpectators;
    g_choices.uploadReports = o.uploadReports;
    g_choices.showFps = o.showOwnFps;
    g_choices.shareCountry = o.shareCountry;
    g_choices.streamerMode = o.streamerMode;
    g_choices.wordFilter = o.wordFilter;
    if (!g_randomOffSaved) { // a list set here (the seed below) stays
        g_randomOff = o.randomOff;
        g_randomOffSaved = o.randomOffSaved;
    }
    g_choices.initialised = true;
}

// The netplay options with the panel's choices.
NetplayOptions ChosenOptions() {
    NetplayOptions o = NetOptions();
    const Choices& c = g_choices;
    o.hasPick = c.pick && c.chara < static_cast<int>(g_tables.charas.size());
    if (o.hasPick) {
        o.pick.character = PackCode(g_tables.charas[c.chara].c_str());
        o.pick.costume = static_cast<uint16_t>(c.costume);
        o.pick.color = static_cast<uint8_t>(c.color);
        o.pick.vskill = static_cast<uint8_t>(c.vskill - 1);
        o.pick.vtrigger = static_cast<uint8_t>(c.vtrigger - 1);
        o.pick.variation = static_cast<uint8_t>(c.variant);
        o.pick.comment = static_cast<uint8_t>(c.comment);
        if (IsEleven(c.chara)) o.pick = ElevenPick(g_choices); // may roll the copied fighter
    }
    o.stage = kKeepStage;
    o.stageCode = "random"; // the host picks one per match
    if (g_randomOffSaved) {
        o.randomOff = g_randomOff;
        o.randomOffSaved = true;
    }
    if (c.stage >= 2 && c.stage - 2 < static_cast<int>(g_tables.stages.size())) {
        const int i = c.stage - 2;
        const auto& settings = g_tables.settings[i];
        o.stageCode = c.setting > 0 && c.setting <= static_cast<int>(settings.size()) ? settings[c.setting - 1] : g_tables.stages[i];
    }
    o.inputDelay = static_cast<uint8_t>(c.delay);
    o.name = c.name;
    o.allowSpectators = c.allowSpectators;
    o.uploadReports = c.uploadReports;
    o.showOwnFps = c.showFps;
    o.shareCountry = c.shareCountry;
    o.streamerMode = c.streamerMode;
    o.wordFilter = c.wordFilter;
    return o;
}

std::string CharaLabel(const std::string& code) {
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    return it == g_tables.charas.end() ? code : g_tables.labels[it - g_tables.charas.begin()];
}

std::string SkillText(const std::string& code, bool vtrigger, unsigned index) {
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it != g_tables.charas.end()) {
        const auto& names = (vtrigger ? g_tables.vtriggers : g_tables.vskills)[it - g_tables.charas.begin()];
        if (index < names.size() && !names[index].empty()) return Roman(static_cast<int>(index) + 1) + " " + names[index];
    }
    return Roman(static_cast<int>(index) + 1);
}

// A color as the menus name it: "Color 3", "EX Color 2" (EX colors are ids
// from 100).
std::string ColorLabel(int id) {
    return id >= 100 ? "EX Color " + std::to_string(id - 99) : "Color " + std::to_string(id + 1);
}

std::string StageText(uint16_t index) {
    const std::string code = StageCode(index);
    const auto it = std::find(g_tables.stages.begin(), g_tables.stages.end(), code);
    if (code.empty()) return "as prepared";
    return it == g_tables.stages.end() ? code : g_tables.stageLabels[it - g_tables.stages.begin()];
}

// Saves the panel's settings (and keeps them for the next host or join).
void SaveChoices() {
    const NetplayOptions o = ChosenOptions();
    Act([o] {
        NetOptions().name = o.name;
        NetOptions().allowSpectators = o.allowSpectators;
        NetOptions().uploadReports = o.uploadReports;
        SetReportUploads(o.uploadReports);
        NetOptions().showOwnFps = o.showOwnFps;
        NetOptions().shareCountry = o.shareCountry;
        WeyveService::SetHideCountry(!o.shareCountry);
        NetOptions().streamerMode = o.streamerMode;
        SetStreamerMode(o.streamerMode);
        NetOptions().wordFilter = o.wordFilter;
        SetRoomsWordFilter(o.wordFilter);
        NetOptions().inputDelay = o.inputDelay;
        if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, o, o.stageCode);
    });
}

// The costume's variants (flow.h CostumeVariants bits), asked of the game off
// the render thread; -1 while unknown.
std::mutex g_variantMutex;
std::map<std::pair<int, int>, int> g_variants;
std::map<std::pair<int, int>, ULONGLONG> g_variantAsked;

int VariantsOf(int chara, int costume) {
    if (chara < 0 || chara >= static_cast<int>(g_tables.charaIndex.size())) return 0;
    const std::pair<int, int> key{chara, costume};
    {
        std::lock_guard lock(g_variantMutex);
        const auto it = g_variants.find(key);
        const int known = it == g_variants.end() ? -1 : it->second;
        if (known >= 0) return known;
        ULONGLONG& asked = g_variantAsked[key];
        if (GetTickCount64() - asked < 2000) return -1;
        asked = GetTickCount64();
    }
    const int gameIndex = g_tables.charaIndex[chara];
    Act([key, gameIndex] {
        const int n = CostumeVariants(gameIndex, key.second);
        std::lock_guard lock(g_variantMutex);
        g_variants[key] = n;
    });
    return -1;
}

// The pick's parts, one widget each, for the lobby's rows and the side
// windows; `label` as ImGui's ("##..." hides it). PickValid first: it keeps
// the pick to what the game offers and says whether the lists are there.
bool IsEleven(int chara) { return chara >= 0 && chara < static_cast<int>(g_tables.charas.size()) && g_tables.charas[chara] == "ELV"; }

// Eleven copies a fighter picked at random each time it is picked (another
// than last time), with that fighter's V-Skill and V-Trigger at random too.
void RollEleven(Choices& c) {
    static std::mt19937 rng{std::random_device{}()};
    std::vector<int> fighters;
    for (int i = 0; i < static_cast<int>(g_tables.charas.size()); i++) {
        if (g_tables.elevenCostume[i] >= 0 && i != c.elevenAs) fighters.push_back(i);
    }
    c.elevenAs = fighters.empty() ? -1 : fighters[rng() % fighters.size()];
    c.vskill = 1 + static_cast<int>(rng() % 2);
    c.vtrigger = 1 + static_cast<int>(rng() % 2);
}

// The pick Eleven plays: the copied fighter in its 0C costume.
PlayerSetup ElevenPick(Choices& c) {
    if (c.elevenAs < 0 || c.elevenAs >= static_cast<int>(g_tables.charas.size())) RollEleven(c);
    PlayerSetup p;
    if (c.elevenAs < 0) return p;
    p.character = PackCode(g_tables.charas[c.elevenAs].c_str());
    p.costume = static_cast<uint16_t>(g_tables.elevenCostume[c.elevenAs]);
    p.vskill = static_cast<uint8_t>(c.vskill - 1);
    p.vtrigger = static_cast<uint8_t>(c.vtrigger - 1);
    return p;
}

// A pick in a fighter's 0C costume: Eleven's.
bool IsElevenSetup(const PlayerSetup& p) {
    const std::string code = UnpackCode(p.character);
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it == g_tables.charas.end()) return false;
    const int costume = g_tables.elevenCostume[it - g_tables.charas.begin()];
    return costume >= 0 && p.costume == costume;
}

bool PickValid(Choices& c) {
    if (!g_tables.loaded || !c.initialised) return false;
    c.chara = std::clamp(c.chara, 0, static_cast<int>(g_tables.charas.size()) - 1);
    // Only what this profile owns (a saved pick may be something it lacks):
    // else the first owned character, costume, color.
    if (!CharaOwned(c.chara)) {
        for (int i = 0; i < static_cast<int>(g_tables.charas.size()); i++) {
            if (CharaOwned(i) && !IsEleven(i)) {
                c.chara = i;
                break;
            }
        }
    }
    int slot = CostumeSlot(c.chara, c.costume);
    if (slot < 0 || !CostumeOwned(c.chara, slot)) {
        slot = 0;
        for (int k = 0; k < static_cast<int>(g_tables.costumes[c.chara].size()); k++) {
            if (CostumeOwned(c.chara, k)) {
                slot = k;
                break;
            }
        }
        c.costume = g_tables.costumes[c.chara][slot];
    }
    const std::vector<int>& colors = g_tables.colors[c.chara][slot];
    const auto at = std::find(colors.begin(), colors.end(), c.color);
    if (at == colors.end() || !ColorOwnedAt(c.chara, slot, static_cast<int>(at - colors.begin()))) {
        c.color = colors[0];
        for (int k = 0; k < static_cast<int>(colors.size()); k++) {
            if (ColorOwnedAt(c.chara, slot, k)) {
                c.color = colors[k];
                break;
            }
        }
    }
    // The host's stage: one not owned goes to its default setting, or random.
    if (c.stage >= 2 && !StageOwned(c.stage - 2, c.setting)) {
        if (StageOwned(c.stage - 2, 0)) {
            c.setting = 0;
        } else {
            c.stage = 1;
            c.setting = 0;
        }
    }
    return true;
}

// The other player's pick in this session, as last seen (render thread,
// each frame): their color on the same character and costume is not offered
// here (two of the same look alike).
bool g_otherHasPick = false;
PlayerSetup g_otherPick;

// The color the other player wears on this pick's character and costume, or -1.
int TakenColor(const Choices& c) {
    if (!g_otherHasPick || !g_tables.loaded || c.chara < 0 || c.chara >= static_cast<int>(g_tables.charas.size()) || IsEleven(c.chara)) {
        return -1;
    }
    if (g_otherPick.character != PackCode(g_tables.charas[c.chara].c_str()) || g_otherPick.costume != c.costume) return -1;
    return g_otherPick.color;
}

// After a change of character or costume: off the other player's color, to
// the next one free (this player made the change, so this player moves).
void AvoidTakenColor(Choices& c) {
    const int taken = TakenColor(c);
    if (taken < 0 || c.color != taken) return;
    const std::vector<int>& ids = g_tables.colors[c.chara][std::max(0, CostumeSlot(c.chara, c.costume))];
    const int n = static_cast<int>(ids.size());
    const int at = static_cast<int>(std::find(ids.begin(), ids.end(), c.color) - ids.begin());
    const int slot = std::max(0, CostumeSlot(c.chara, c.costume));
    for (int k = 1; n > 1 && at < n && k < n; k++) {
        const int next = (at + k) % n;
        if (ColorOwnedAt(c.chara, slot, next)) {
            c.color = ids[next];
            break;
        }
    }
}

// Another character: the one left keeps its loadout for later, the new one
// gets its own as last saved or left (else the first costume and color, V-Skill
// and V-Trigger I).
void SwitchCharacter(Choices& c, int chara) {
    if (chara == c.chara || !CharaOwned(chara)) return;
    PlayerSetup& left = g_loadouts[g_tables.charas[c.chara]];
    left.costume = static_cast<uint16_t>(c.costume);
    left.color = static_cast<uint8_t>(c.color);
    left.vskill = static_cast<uint8_t>(c.vskill - 1);
    left.vtrigger = static_cast<uint8_t>(c.vtrigger - 1);
    left.variation = static_cast<uint8_t>(c.variant);
    left.comment = static_cast<uint8_t>(c.comment);
    c.chara = chara;
    if (IsEleven(chara)) RollEleven(c);
    const auto it = g_loadouts.find(g_tables.charas[chara]);
    const PlayerSetup saved = it != g_loadouts.end() ? it->second : PlayerSetup{};
    c.costume = saved.costume;
    c.color = saved.color;
    c.vskill = saved.vskill + 1;
    c.vtrigger = saved.vtrigger + 1;
    c.variant = saved.variation;
    c.comment = saved.comment;
    PickValid(c);
    AvoidTakenColor(c);
}

std::string CommentText(const std::string& code, int comment) {
    if (comment == 0) return "Auto";
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it == g_tables.charas.end()) return "Comment " + std::to_string(comment);
    return "Comment " + std::to_string(comment);
}

// This player's input delay for the next match (the vote's delay row).
constexpr int kMaxDelayFrames = 10;

std::string FramesText(int n) { return std::to_string(n) + (n == 1 ? " frame" : " frames"); }

// In a session: the panel's pick, the host's stage and this player's delay
// go to the session as they change, and are saved for the next session.
void SyncChoices(const NetView& v) {
    // Not before the panel knows the characters: it would send "no pick".
    // Not from a preview either: what it marks as sent would not be.
    if (!g_tables.loaded || !g_choices.initialised || g_previewing) return;
    static bool sentHas = false;
    static PlayerSetup sent;
    static std::string sentStage;
    static int sentDelay = -1;
    NetplayOptions o = ChosenOptions();
    // A variant goes only with a costume known to have it.
    if (const int variants = VariantsOf(g_choices.chara, g_choices.costume); variants < 0 || (o.pick.variation & variants) != o.pick.variation) {
        o.pick.variation = 0;
    }
    const bool pickChanged = o.hasPick != sentHas || !(o.pick == sent);
    const bool stageChanged = v.host && o.stageCode != sentStage;
    const bool delayChanged = o.inputDelay != sentDelay;
    if (!pickChanged && !stageChanged && !delayChanged) return;
    sentHas = o.hasPick;
    sent = o.pick;
    if (v.host) sentStage = o.stageCode;
    sentDelay = o.inputDelay;
    Act([o, pickChanged, stageChanged, delayChanged] {
        if (pickChanged) NetSetPick(o.hasPick, o.pick);
        if (stageChanged) NetSetStage(o.stageCode);
        if (delayChanged) NetSetDelay(o.inputDelay);
        if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, o, o.stageCode);
    });
}

// The character's name ("CHUN-LI"), for the short pick lines.
std::string ShortCharaName(const std::string& code) { return CharaLabel(code); }

// The Debug tab's previews: the match windows with made-up data, each on
// its own, to look at without a session (render thread).
struct Preview {
    bool hud = false;                        // the player bars over the health bars
    ULONGLONG fadeAt = 0;                    // their fade at the match's end, played from here
    bool strip = false;                      // the corner line while the panel is closed
    bool desync = false;                     // the red banner
    bool side[kNumPlayers] = {false, false}; // P1's and P2's windows after a match
    int sides = 0;                           // 0 the rematch vote, 1 character select over the results
    int you = 0;                             // whose side is this player's
    int choice[kNumPlayers] = {0, 1};        // each side's vote: none, rematch, character select
    bool ready[kNumPlayers] = {false, true};
    bool relayed = true; // the link through Cloudflare's relay (else peer to peer)
    bool set = false;    // the side windows of a table's set (no vote)

    bool Any() const { return hud || strip || desync || side[0] || side[1]; }
};

Preview g_preview;
bool g_sampleToasts = false; // add a few toasts on the next frame

// Research: a game texture shown in a window (SetOverlayTexture).
std::mutex g_textureMutex;
ID3D11ShaderResourceView* g_texture = nullptr;
uint32_t g_textureW = 0, g_textureH = 0;
std::string g_textureLabel;

void DrawTexturePreview() {
    std::lock_guard lock(g_textureMutex);
    if (!g_texture) return;
    const float scale = std::min(1.0f, 512.0f * g_uiScale / std::max<uint32_t>(1, std::max(g_textureW, g_textureH)));
    ImGui::SetNextWindowPos(ImVec2(20 * g_uiScale, 20 * g_uiScale), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Texture##research", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::Text("%s  %ux%u", g_textureLabel.c_str(), g_textureW, g_textureH);
        ImGui::Image(reinterpret_cast<ImTextureID>(g_texture), ImVec2(g_textureW * scale, g_textureH * scale));
    }
    ImGui::End();
}

// Testing aids: simulated network, mashing, quick scene switches, the log.
// The select screen preview's data: the panel's characters (Eleven included)
// with their costumes, swatches, V-Skills, V-Triggers and V-Comments, and the
// stage select's stages.
SelectData PreviewSelectData() {
    SelectData data;
    for (size_t i = 0; i < g_tables.charas.size(); i++) {
        SelectChara c;
        c.code = g_tables.charas[i];
        c.name = g_tables.labels[i];
        c.costumes = g_tables.costumeNames[i];
        for (const CostumeSwatches& sw : g_tables.swatches[i]) {
            std::vector<std::pair<uint32_t, uint32_t>> both = sw.colors;
            both.insert(both.end(), sw.ex.begin(), sw.ex.end());
            c.swatches.push_back(std::move(both));
        }
        c.vskills = g_tables.vskills[i];
        c.vtriggers = g_tables.vtriggers[i];
        c.vcomments = g_tables.vcomments[i];
        c.locked = !CharaOwned(static_cast<int>(i));
        data.charas.push_back(std::move(c));
    }
    for (size_t i = 0; i < g_tables.stages.size(); i++) {
        SelectStage stage{g_tables.stages[i], g_tables.stageLabels[i], {}};
        stage.locked = !StageOwned(static_cast<int>(i), 0);
        for (size_t k = 0; k < g_tables.settings[i].size(); k++) {
            SelectStage setting{g_tables.settings[i][k], g_tables.settingLabels[i][k], {}};
            setting.locked = !StageOwned(static_cast<int>(i), static_cast<int>(k) + 1);
            stage.settings.push_back(std::move(setting));
        }
        data.stages.push_back(std::move(stage));
    }
    data.you = g_choices.name;
    return data;
}

// ----- a session's select screen -------------------------------------------

int ElevenIndex() {
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), "ELV");
    return it == g_tables.charas.end() ? -1 : static_cast<int>(it - g_tables.charas.begin());
}

std::pair<uint32_t, uint32_t> SwatchOf(int chara, int slot, int color) {
    if (chara < 0 || slot < 0 || chara >= static_cast<int>(g_tables.swatches.size()) || slot >= static_cast<int>(g_tables.swatches[chara].size())) {
        return {0, 0};
    }
    const CostumeSwatches& sw = g_tables.swatches[chara][slot];
    const auto& list = color >= 100 ? sw.ex : sw.colors;
    const int k = color >= 100 ? color - 100 : color;
    return k >= 0 && k < static_cast<int>(list.size()) ? list[k] : std::pair<uint32_t, uint32_t>{0, 0};
}

std::string CommentLine(int chara, int comment) {
    if (comment == 0) return "Changes with your opponent's character.";
    const auto& names = g_tables.vcomments[chara];
    return comment <= static_cast<int>(names.size()) ? "\"" + names[comment - 1] + "\"" : "Comment " + std::to_string(comment);
}

// One seat for the select screen: this player's from the panel's choices (it
// answers at once), the other's from the session.
SelectSide SelectSideFor(const NetView& v, int i) {
    SelectSide s;
    const lobby::Seat& seat = v.seats[i];
    s.present = seat.present;
    s.player = seat.name;
    s.link = seat.link == lobby::Link::Wired ? 1 : seat.link == lobby::Link::WiFi ? 2
                                                                                  : 0;
    s.ready = seat.ready;
    const bool mine = i == v.you && !v.spectator;
    s.screen = seat.screen;
    s.row = seat.row;
    s.hoverRandom = seat.screen == 1 && seat.hover == 0;
    if (seat.hover) {
        const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), UnpackCode(seat.hover));
        s.hover = it == g_tables.charas.end() ? -1 : static_cast<int>(it - g_tables.charas.begin());
    }
    const Choices& c = g_choices;
    int chara = -1;
    PlayerSetup pick;
    if (mine) {
        if (!PickValid(g_choices)) return s;
        chara = c.chara;
        pick.costume = static_cast<uint16_t>(c.costume);
        pick.color = static_cast<uint8_t>(c.color);
        pick.vskill = static_cast<uint8_t>(c.vskill - 1);
        pick.vtrigger = static_cast<uint8_t>(c.vtrigger - 1);
        pick.variation = static_cast<uint8_t>(c.variant);
        pick.comment = static_cast<uint8_t>(c.comment);
    } else if (seat.hasPick && seat.pick.character) {
        pick = seat.pick;
        const std::string code = UnpackCode(pick.character);
        const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
        chara = IsElevenSetup(pick) ? ElevenIndex() : it == g_tables.charas.end() ? -1
                                                                                  : static_cast<int>(it - g_tables.charas.begin());
    }
    s.chara = chara;
    const int delay = mine ? c.delay : seat.inputDelay;
    s.rows[6].value = FramesText(delay);
    const int fixed = NetOptions().delaySetting;
    const int suggested = v.SuggestedDelay();
    s.rows[6].hint = fixed >= 0       ? FramesText(fixed) + " in Polarity settings. Lower feels sharper but rolls back more."
                     : suggested >= 0 ? "Suggested for a " + std::to_string(v.link.pingMs) + " ms ping: " + FramesText(suggested) + ". Lower feels sharper but rolls back more."
                                      : std::string("Lower feels sharper but rolls back more.");
    if (chara < 0) return s;
    if (IsEleven(chara)) {
        for (int r = 0; r < 6; r++) {
            s.rows[r].value = "Random";
            s.rows[r].fixed = true;
            s.rows[r].hint = "Plays a random character in silver, with a random V-Skill and V-Trigger.";
        }
        return s;
    }
    const std::string& code = g_tables.charas[chara];
    const int slot = std::max(0, CostumeSlot(chara, pick.costume));
    s.rows[0].value = g_tables.costumeNames[chara][slot];
    s.rows[0].hint = "";
    s.rows[1].value = ColorLabel(pick.color);
    const auto swatch = SwatchOf(chara, slot, pick.color);
    s.rows[1].swatch1 = swatch.first;
    s.rows[1].swatch2 = swatch.second;
    s.rows[1].hint = "";
    const int variants = mine ? VariantsOf(chara, pick.costume) : 1;
    s.rows[2].value = variants < 0 ? "checking..." : VariantLabel(code, pick.variation);
    s.rows[2].fixed = mine && variants <= 0;
    s.rows[2].hint = variants == 0 ? "This costume has no variants." : "";
    s.rows[3].value = SkillText(code, false, pick.vskill);
    s.rows[3].hint = "";
    s.rows[4].value = SkillText(code, true, pick.vtrigger);
    s.rows[4].hint = "";
    s.rows[5].value = pick.comment == 0 ? "AUTO" : std::to_string(pick.comment);
    s.rows[5].hint = CommentLine(chara, pick.comment);
    return s;
}

// The host's stage as an index into the stage select's stages (their count:
// random), and its setting (SelectLive::setting).
int SelectStageFor(const NetView& v, int* setting) {
    const int count = static_cast<int>(g_tables.stages.size());
    *setting = 0;
    if (v.host) {
        if (g_choices.stage < 2 || g_choices.stage - 2 >= count) return count;
        *setting = g_choices.setting;
        return g_choices.stage - 2;
    }
    if (v.stageRule == lobby::kRandomStage || v.stageRule == lobby::kAnyStage) return count;
    const std::string code = StageCode(v.stageRule);
    for (int i = 0; i < count; i++) {
        const auto& settings = g_tables.settings[i];
        const auto it = std::find(settings.begin(), settings.end(), code);
        if (it != settings.end()) *setting = 1 + static_cast<int>(it - settings.begin());
        if (g_tables.stages[i] == code || it != settings.end()) return i;
    }
    return count;
}

SelectLive SelectLiveFor(const NetView& v) {
    SelectLive live;
    live.you = v.spectator ? -1 : v.you;
    live.host = v.host;
    for (int i = 0; i < kNumPlayers; i++) live.sides[i] = SelectSideFor(v, i);
    live.stage = SelectStageFor(v, &live.setting);
    live.stageLocked = v.stageLocked;
    live.bothReady = v.seats[0].ready && v.seats[1].ready;
    live.loading = live.bothReady && !(v.seats[0].preloaded && v.seats[1].preloaded);
    live.pingMs = v.linkPingMs;
    live.relayed = v.linkPath == NetView::Path::Relayed;
    live.spectators = v.spectators;
    live.set = v.setPlay;
    return live;
}

// A loadout row one step on (or back): what the panel's pickers offer.
void StepRow(Choices& c, int row, int delta) {
    auto cycle = [delta](int at, int n) { return n <= 0 ? 0 : ((at + delta) % n + n) % n; };
    switch (row) {
        case 0: {
            // Over the costumes this profile owns.
            const std::vector<int>& list = g_tables.costumes[c.chara];
            const int n = static_cast<int>(list.size());
            int at = std::max(0, CostumeSlot(c.chara, c.costume));
            for (int tries = 0; tries < n; tries++) {
                at = cycle(at, n);
                if (CostumeOwned(c.chara, at)) break;
            }
            c.costume = list[at];
            c.variant = 0;
            PickValid(c);
            AvoidTakenColor(c);
            break;
        }
        case 1: {
            // The other player's color on this costume is stepped over; at the
            // list's end that way, the step does not happen.
            // Colors this profile does not own are stepped over too.
            const int slot = std::max(0, CostumeSlot(c.chara, c.costume));
            const std::vector<int>& ids = g_tables.colors[c.chara][slot];
            const int n = static_cast<int>(ids.size());
            const int at = static_cast<int>(std::find(ids.begin(), ids.end(), c.color) - ids.begin());
            const int taken = TakenColor(c);
            int next = at < n ? at : 0;
            bool found = false;
            for (int tries = 0; tries < n; tries++) {
                next = cycle(next, n);
                if (!ColorOwnedAt(c.chara, slot, next)) continue;
                if (ids[next] == taken) {
                    if (delta > 0 ? next == n - 1 : next == 0) break;
                    continue;
                }
                found = true;
                break;
            }
            if (found) c.color = ids[next];
            break;
        }
        case 2: {
            const int variants = VariantsOf(c.chara, c.costume);
            if (variants <= 0) break;
            std::vector<int> options;
            for (int n = 0; n <= variants; n++) {
                if ((n & variants) == n) options.push_back(n);
            }
            const int at = static_cast<int>(std::find(options.begin(), options.end(), c.variant) - options.begin());
            c.variant = options[cycle(at < static_cast<int>(options.size()) ? at : 0, static_cast<int>(options.size()))];
            break;
        }
        case 3:
        case 4: {
            int& value = row == 3 ? c.vskill : c.vtrigger;
            const auto& names = row == 3 ? g_tables.vskills[c.chara] : g_tables.vtriggers[c.chara];
            value = 1 + cycle(value - 1, std::max(2, static_cast<int>(names.size())));
            break;
        }
        case 5: c.comment = cycle(c.comment, 1 + static_cast<int>(g_tables.vcomments[c.chara].size())); break;
        default: c.delay = std::clamp(c.delay + delta, 0, kMaxDelayFrames); break;
    }
}

// What the player did on the select screen: into the panel's choices (sent as
// they change, SyncChoices) and the session.
// What NetSetSelectState was last given (a new lobby starts over: the driver
// forgets it there).
struct SentSelect {
    int screen = -1;
    uint8_t row = 0;
    uint32_t hover = 0;
} g_sentSelect;

void ApplySelectInput(const SelectInput& in, const NetView& v) {
    Choices& c = g_choices;
    if (in.leave) {
        Log("overlay: left from the select screen");
        Act([] { NetLeave(); });
        return;
    }
    if (v.spectator) return;
    // Where this player is, for the other screens (sent when it changes).
    const uint32_t hover = in.screen != 1 || in.hoverRandom || in.hover < 0 || in.hover >= static_cast<int>(g_tables.charas.size())
                               ? 0
                               : PackCode(g_tables.charas[in.hover].c_str());
    const uint8_t screen = static_cast<uint8_t>(in.screen), row = static_cast<uint8_t>(std::max(0, in.screenRow));
    if (screen != g_sentSelect.screen || row != g_sentSelect.row || hover != g_sentSelect.hover) {
        g_sentSelect = {screen, row, hover};
        Act([screen, row, hover] { NetSetSelectState(screen, row, hover); });
    }
    const bool ready = v.you >= 0 && v.seats[v.you].ready;
    if (!ready) {
        if (in.chara >= 0 && in.chara < static_cast<int>(g_tables.charas.size())) SwitchCharacter(c, in.chara);
        if (in.randomPick) {
            static std::mt19937 rng{std::random_device{}()};
            std::vector<int> fighters;
            for (int i = 0; i < static_cast<int>(g_tables.charas.size()); i++) {
                if (!IsEleven(i) && CharaOwned(i)) fighters.push_back(i);
            }
            if (!fighters.empty()) SwitchCharacter(c, fighters[rng() % fighters.size()]);
        }
        if (in.row >= 0 && in.delta) StepRow(c, in.row, in.delta);
        if (v.host && in.stage >= 0) {
            const int count = static_cast<int>(g_tables.stages.size());
            c.stage = in.stage < count ? 2 + in.stage : 1;
            c.setting = 0;
        }
        if (v.host && in.setting >= 0) c.setting = in.setting;
        PickValid(c);
        // Ready on Eleven: a new fighter each time (also when kept after a
        // match), sent before the ready.
        if (in.ready && IsEleven(c.chara)) RollEleven(c);
    }
    SyncChoices(v);
    if (v.host && in.lockStage) Act([] { NetSetStageLocked(true); });
    if (v.host && in.unlockStage && !ready) Act([] { NetSetStageLocked(false); });
    if (in.ready && !ready) Act([] { NetSetReady(true); });
    if (in.unready && ready) Act([] { NetSetReady(false); });
}

// While the panel is closed: one line in a corner during a session.
void DrawStrip(const NetView& v) {
    std::string text;
    ImVec4 color = kDim;
    char buf[160];
    switch (v.phase) {
        case NetView::Phase::Hosting: text = "Hosting " + v.code + ": waiting for the other player"; break;
        case NetView::Phase::Connecting:
        case NetView::Phase::Joining:
        case NetView::Phase::Jumping:
        case NetView::Phase::Loading: text = v.status; break;
        case NetView::Phase::Lobby: text = "Lobby"; break;
        case NetView::Phase::PostMatch: text = "Match over"; break;
        case NetView::Phase::Running:
            if (v.syncing) {
                text = "connecting...";
                break;
            }
            if (v.spectator) std::snprintf(buf, sizeof(buf), "watching  frame %d  %d in hand", v.frame, v.behind);
            else std::snprintf(buf, sizeof(buf), "%s  ping %u ms  ahead %.1f  rollbacks %u (max %u)  delay %d", v.host ? "P1" : "P2",
                               v.pingMs, v.framesAhead, v.rollbacks, v.longestRollback, v.inputDelay);
            text = buf;
            if (v.desyncFrame >= 0) {
                text += "  DESYNC at " + std::to_string(v.desyncFrame);
                color = kRed;
            }
            break;
        // How the session ended is a toast (bottom center), not repeated here.
        case NetView::Phase::Ended:
        case NetView::Phase::Idle: return;
    }
    DrawStatusNote(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, text, ImGui::ColorConvertFloat4ToU32(color));
}

// ----- without the panel: HUD, banner, toasts, the vote ---------------------

struct Toast {
    std::string text;
    ImVec4 color;
    ULONGLONG until;
    ULONGLONG at = 0; // when it came (it slides in)
};

std::vector<Toast> g_toasts; // render thread

void AddToast(const std::string& text, ImVec4 color = ImVec4(1, 1, 1, 1), int seconds = 4) {
    // The game's notice sound (the error one for a red toast), out of a match:
    // in a fight the toasts are small notes and the fight has its own sounds.
    if (!g_inMatch) PlayUi(color.x > 0.9f && color.y < 0.5f ? UiSound::NoticeError : UiSound::Notice);
    if (g_toasts.size() >= 5) g_toasts.erase(g_toasts.begin());
    g_toasts.push_back({text, color, GetTickCount64() + seconds * 1000ull, GetTickCount64()});
}

bool OnMenuScreen();

std::atomic<bool> g_migrated{false}; // SetMigratedNotice: not toasted yet

// After the move from SFVR (migrate.h), at the first menu.
void ToastMigrated() {
    if (!g_migrated || !OnMenuScreen()) return;
    g_migrated = false;
    AddToast("SFVR is now Polarity. Your settings, replays and logs are in the Polarity folder.", kGold, 12);
}

// Desync reports: once ever, at the main menu, that they are sent (and where
// that is turned off); then each match's, as it is sent.
void ToastReports() {
    static bool noticed = false;
    if (!noticed && OnMenuScreen()) {
        noticed = true;
        if (!g_panelPath.empty() && NetOptions().uploadReports &&
            !GetPrivateProfileIntW(L"report", L"notice_shown", 0, g_panelPath.c_str())) {
            WritePrivateProfileStringW(L"report", L"notice_shown", L"1", g_panelPath.c_str());
            AddToast("Polarity sends desync and crash reports to help fix bugs. You can turn this off in Polarity settings.", kGold, 15);
        }
    }
    static int toasted = 0;
    const int sent = ReportsSent();
    if (sent == toasted) return;
    toasted = sent;
    AddToast("Bug report sent", kDim, 6);
}

// Notices from what changed in the session since the last frame.
// Once per version the site offers: a toast pointing at Settings.
void ToastUpdate() {
    // At the menus only (never over a match): on the hub its modal says it;
    // in a room the set goes on, and leaving the room brings the modal.
    static std::string toasted;
    const UpdateView u = UpdateViewNow();
    if (u.state != UpdateView::State::Available || u.latest == toasted) return;
    if (HubOpen() && !RoomsScreenOpen()) return;
    toasted = u.latest;
    const bool inRoom = RoomsScreenOpen() && RoomsSnapshot().state == RoomsView::State::Room;
    Log("overlay: Polarity %s is out: a toast (%s)", u.latest.c_str(), inRoom ? "in a room" : "at the game's menus");
    AddToast("Polarity " + u.latest + " is out. " + (inRoom ? "Leave the room to update." : "Press Back + Start to update."), kGold, 10);
}

void ToastChanges(const NetView& now) {
    static NetView before;
    static bool have = false;
    if (!have) {
        before = now;
        have = true;
        return;
    }
    using P = NetView::Phase;
    if (now.phase == P::Hosting && before.phase != P::Hosting && !g_visible && !now.setPlay) {
        AddToast("Hosting. Code " + now.code + " copied", kGreen, 10);
    }
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat &a = before.seats[i], &b = now.seats[i];
        if (i == now.you) continue;
        if (now.host && b.present && !a.present && !b.name.empty() && !now.setPlay) AddToast(b.name + " joined");
        if (now.phase == P::PostMatch && before.phase == P::PostMatch && b.choice != a.choice &&
            b.choice != lobby::Choice::None) {
            AddToast(b.name + (b.choice == lobby::Choice::Rematch ? " wants a rematch" : " wants character select"));
        }
    }
    if (now.spectator && now.spectators != before.spectators && now.phase != P::Idle && now.phase != P::Ended) {
        AddToast(now.spectators > before.spectators ? "A spectator joined (" + std::to_string(now.spectators) + " watching)"
                                                    : std::to_string(now.spectators) + " watching",
                 kDim);
    }
    // A table's set: the room screen says how it went.
    if (now.phase == P::Ended && before.phase != P::Ended && !now.result.empty() && !now.setPlay) AddToast("Session over: " + now.result, kRed, 6);
    before = now;
}

// A desync in this match: a note at the bottom centre of the fight until the
// match is over (the recording is in Polarity\matches, named ..._desync.sfvr).
Notice DesyncNotice(const NetView& v) {
    return {"DESYNC AT FRAME " + std::to_string(v.desyncFrame), v.desyncReport.empty() ? "saving the recording..." : "recording saved",
            IM_COL32(255, 107, 94, 255), 1, 99};
}

// The notices (the design system's "Notices"): out of a fight, toasts down
// from the top right; in one (`fight`), small notes at the bottom centre,
// the desync's with them.
NetView PreviewView(NetView::Phase phase);

void DrawNotices(bool fight, const NetView& v) {
    const ULONGLONG now = GetTickCount64();
    g_toasts.erase(std::remove_if(g_toasts.begin(), g_toasts.end(), [&](const Toast& t) { return t.until <= now; }),
                   g_toasts.end());
    std::vector<Notice> notices;
    for (const Toast& t : g_toasts) {
        ImVec4 c = t.color;
        c.w = 1;
        notices.push_back({"", t.text, ImGui::ColorConvertFloat4ToU32(c), (now - t.at) / 1000.0f, (t.until - now) / 1000.0f});
    }
    if (fight && v.desyncFrame >= 0) notices.push_back(DesyncNotice(v));
    else if (g_preview.desync) notices.push_back(DesyncNotice(PreviewView(NetView::Phase::Running))); // the previews' (`overlay preview desync`)
    if (notices.empty()) return;
    if (fight) DrawFightNotes(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, notices);
    else DrawToasts(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, notices, HubOpen());
}

// During a match: a bar at the very top of the screen over each health bar
// (above the game's win count), P1's on the left and P2's on the right: a
// dark plate fading out toward the timer over a gold rule, as the game's own
// name plates, on one line: the player, their set so far (matches won of
// those played this session), their connection's icon and, on the
// other player's bar, the ping (colored by latency), their frame rate (over
// the last 3 s) and the longest rollback in the last second. They fade out
// once the match is over (its last round's outro).
constexpr ULONGLONG kBarsFadeMs = 800;

void DrawPlayerBars(const NetView& v) {
    static ULONGLONG overAt = 0;
    float alpha = 1.0f;
    if (v.phase == NetView::Phase::PostMatch) {
        if (!overAt) overAt = GetTickCount64();
        alpha = 1.0f - static_cast<float>(GetTickCount64() - overAt) / kBarsFadeMs;
        if (alpha <= 0) return;
    } else {
        overAt = 0;
    }
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    auto color = [alpha](ImVec4 c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a * alpha)); };
    ImFont* big = g_fontHeading;
    ImFont* text = g_fontSmall; // the details: smaller than the names, light enough to read
    const ImVec4 detail = Rgb(214, 211, 200);
    const float bigSize = big->LegacySize, textSize = text->LegacySize;
    const float rule = std::max(1.0f, 1.5f * g_uiScale);
    const float pad = 10 * g_uiScale, gap = 8 * g_uiScale;
    // Across, from the game's HUD measured in screen fractions (P1's health
    // bar 8-46%, P2's 54-94%, the round markers by the timer).
    const float top = 0, bottom = bigSize + 6 * g_uiScale + rule;
    const unsigned ping = v.avgPingMs > 0 ? static_cast<unsigned>(v.avgPingMs + 0.5f) : v.pingMs;
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat& s = v.seats[i];
        const bool left = i == 0;
        const float x0 = left ? screen.x * 0.085f : screen.x * 0.60f;
        const float x1 = left ? screen.x * 0.40f : screen.x * 0.915f;
        // Dark at the screen's edge, clear toward the middle.
        const ImU32 dark = color(Rgb(10, 10, 9), 0.82f), clear = color(Rgb(10, 10, 9), 0.0f);
        d->AddRectFilledMultiColor(ImVec2(x0, top), ImVec2(x1, bottom), left ? dark : clear, left ? clear : dark,
                                   left ? clear : dark, left ? dark : clear);
        const ImU32 gold = color(kGoldLine), goldClear = color(kGoldLine, 0.0f);
        d->AddRectFilledMultiColor(ImVec2(x0, bottom - rule), ImVec2(x1, bottom), left ? gold : goldClear, left ? goldClear : gold,
                                   left ? goldClear : gold, left ? gold : goldClear);
        // The parts in order from the screen's edge: the name (the game
        // shows P1/P2 itself), the set, the connection's icon, then the other
        // player's numbers: a cloud when the link is relayed (nothing when
        // it is peer to peer), the ping, and in smaller type their frame
        // rate and the longest rollback in the last second.
        enum class Kind { Text,
                          Link,
                          Relay };

        struct Part {
            std::string text;
            ImFont* font;
            ImU32 color;
            Kind kind = Kind::Text;
        };

        std::vector<Part> parts;
        std::string name = s.name.empty() ? std::string("Player") : s.name;
        const std::string you = v.you == i ? " (you)" : "";
        parts.push_back({name + you, big, color(Rgb(242, 240, 230))});
        // The set: matches this player won of those played in it (a table's
        // set; outside a table, this session).
        parts.push_back({"(" + std::to_string(s.wins) + "/" + std::to_string(v.played) + ")", big, color(detail)});
        if (s.link != lobby::Link::Unknown) parts.push_back({"", text, color(detail), Kind::Link});
        const bool relayed = v.linkPath == NetView::Path::Relayed;
        if (v.spectator) {
            if (i == 1) {
                if (relayed) parts.push_back({"", text, color(PathColor(v)), Kind::Relay});
                parts.push_back({"watching, " + std::to_string(v.behind) + " in hand", text, color(detail)});
            }
        } else if (v.you != i) {
            char line[48];
            if (v.syncing) {
                parts.push_back({"connecting...", text, color(detail)});
            } else {
                if (relayed) parts.push_back({"", text, color(PathColor(v)), Kind::Relay});
                std::snprintf(line, sizeof(line), "%ums \xC2\xB1%.0f", ping, v.jitterMs);
                parts.push_back({line, text, color(PingColorVec(ping, static_cast<unsigned>(v.jitterMs + 0.5f)))});
            }
            if (s.fps) parts.push_back({std::to_string(SmoothedFps(i, s.fps)) + " fps", g_fontTiny, color(detail)});
            parts.push_back({std::to_string(v.recentRollback) + " RBF", g_fontTiny, color(detail)});
        }
        auto width = [textSize](const Part& part) {
            return part.kind != Kind::Text ? textSize : part.font->CalcTextSizeA(part.font->LegacySize, FLT_MAX, 0, part.text.c_str()).x;
        };
        auto total = [&] {
            float sum = -gap;
            for (const Part& part : parts) sum += width(part) + gap;
            return sum;
        };
        // A long name is shortened so the set and the numbers stay in the bar.
        while (!name.empty() && total() > x1 - x0 - 2 * pad) {
            // The last character: its UTF-8 continuation bytes, then its lead byte.
            while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();
            if (!name.empty()) name.pop_back();
            parts[0].text = name + "..." + you;
        }
        d->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), true);
        const float mid = top + (bottom - rule - top) * 0.5f;
        float x = left ? x0 + pad : x1 - pad;
        for (const Part& part : parts) {
            const float size = part.font->LegacySize;
            const float w = width(part);
            const float at = left ? x : x - w;
            const ImVec2 iconAt(at, mid - textSize * 0.5f);
            if (part.kind == Kind::Link) DrawLinkIcon(d, iconAt, textSize, s.link, part.color);
            else if (part.kind == Kind::Relay) DrawRelayIcon(d, iconAt, textSize, part.color);
            else d->AddText(part.font, size, ImVec2(at, mid - size * 0.5f), part.color, part.text.c_str());
            x += left ? w + gap : -(w + gap);
        }
        d->PopClipRect();
    }
}

const char* VoteText(lobby::Choice c) {
    switch (c) {
        case lobby::Choice::Rematch: return "Rematch";
        case lobby::Choice::CharacterSelect: return "Character select";
        case lobby::Choice::None: break;
    }
    return "Choosing...";
}

// After a match, over its results screen: a glass pane docked at each side's
// bottom (fight_ui.h DrawVote): the score, each player's choice of what
// follows (Rematch, Character select, Leave) and this player's input delay
// for the next match. Character select is the select screen. The game's own
// results menu is kept from opening meanwhile (flow.h SetHoldResults).
void DrawSides(const NetView& v, const bool (&show)[kNumPlayers] = {true, true}) {
    static ULONGLONG shownAt = 0;
    static int shownFor = -1;
    const int key = v.played * 2 + (v.phase == NetView::Phase::Lobby ? 1 : 0);
    if (shownFor != key) {
        shownFor = key;
        shownAt = GetTickCount64();
        ResetVote();
    }
    // Buttons held from the match must not choose by accident: presses in
    // the first 750 ms do nothing.
    const bool armed = GetTickCount64() - shownAt >= 750;
    const int setWinner = !v.setPlay ? -1 : v.seats[0].wins >= v.firstTo ? 0
                                        : v.seats[1].wins >= v.firstTo   ? 1
                                                                         : -1;
    // Flags from the room, for a table's set (the session's seats have none).
    const RoomsView rooms = v.setPlay && !g_previewing ? RoomsSnapshot() : RoomsView{};
    VoteSide sides[kNumPlayers];
    for (int i = 0; i < kNumPlayers; i++) {
        const lobby::Seat& s = v.seats[i];
        VoteSide& o = sides[i];
        o.shown = show[i];
        o.mine = v.you == i;
        o.slot = i;
        o.name = s.name;
        for (const RoomMember& m : rooms.members) {
            if (m.name == s.name) o.country = m.country;
        }
        o.wins = s.wins;
        o.firstTo = v.setPlay ? v.firstTo : 0;
        // A table's set won: what follows for each player (rooms_client.h
        // RoomsAfterSet); with nobody else waiting for the table, Rematch is
        // the next set (keeper again).
        const bool voting = setWinner < 0 || v.againVote;
        if (setWinner >= 0) {
            o.status = setWinner == i ? "Wins the set" : "Set over";
            o.statusKind = setWinner == i ? 1 : 2;
            const std::string next = v.againVote ? std::string() : RoomsAfterSet(i, setWinner);
            if (!next.empty()) o.lines.push_back(next);
        }
        if (voting) {
            if (setWinner < 0) {
                o.status = VoteText(s.choice);
                o.statusKind = s.choice == lobby::Choice::None ? 0 : 1;
            }
            if (v.againVote && o.mine) o.lines.push_back("No one's waiting, so you can keep playing.");
            o.choosing = o.mine;
            o.chosen = s.choice == lobby::Choice::Rematch ? 0 : s.choice == lobby::Choice::CharacterSelect ? 1
                                                                                                           : -1;
        }
        char line[160];
        if (o.mine) {
            o.delay = g_choices.delay;
            const int fixed = NetOptions().delaySetting, suggested = v.SuggestedDelay();
            if (v.spectator) {
            } else if (fixed >= 0) std::snprintf(line, sizeof(line), "Delay %d, from Polarity settings", fixed), o.suggestion = line;
            else if (suggested >= 0) std::snprintf(line, sizeof(line), "Suggested delay %d (ping %u ms)", suggested, v.link.pingMs), o.suggestion = line;
            if (voting && v.voteSecondsLeft >= 0 && s.choice == lobby::Choice::None) {
                // Playing on: no choice from either is back to the room (both
                // leave); the other's choice carries it.
                const bool neither = v.seats[1 - i].choice == lobby::Choice::None;
                std::snprintf(line, sizeof(line),
                              !v.setPlay || !v.againVote ? "Rematch in %d s"
                              : neither                  ? "Back to the room in %d s"
                                                         : "Next set in %d s",
                              v.voteSecondsLeft);
                o.hint = line;
            } else o.hint = !v.setPlay ? "Leave ends the session." : v.againVote ? "Leave returns to the room."
                                                                                 : "Leave ends the set here.";
        } else {
            o.delay = s.inputDelay;
            o.hint = "Input delay " + std::to_string(s.inputDelay) + (s.inputDelay == 1 ? " frame" : " frames");
        }
    }
    VoteInput in;
    DrawVote(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, sides, armed, !g_previewing, &in);
    if (in.choose == 0) Act([] { NetChoose(lobby::Choice::Rematch); });
    else if (in.choose == 1) Act([] { NetChoose(lobby::Choice::CharacterSelect); });
    else if (in.choose == 2) Act([] { NetLeave(); });
    if (in.delayStep) g_choices.delay = std::clamp(g_choices.delay + in.delayStep, 0, kMaxDelayFrames);
    if (v.you >= 0 && v.you < kNumPlayers && sides[v.you].choosing) SyncChoices(v);
}

// A made-up session for the previews: this player's pick from the panel,
// the other's another character's second costume; numbers that look like a
// match on a good connection.
NetView PreviewView(NetView::Phase phase) {
    const Preview& p = g_preview;
    NetView v;
    v.phase = phase;
    v.you = p.you;
    v.host = p.you == 0;
    v.code = "K7Q2MX";
    v.played = 3;
    v.spectators = 1;
    v.linkPingMs = 38;
    v.link = {38, 4, core::kLinkSamples};
    v.stageRule = lobby::kRandomStage;
    v.overResults = true;
    v.setPlay = p.set;
    v.firstTo = 3;
    const NetplayOptions mine = ChosenOptions();
    for (int i = 0; i < kNumPlayers; i++) {
        lobby::Seat& s = v.seats[i];
        s.present = true;
        s.name = i == p.you && !mine.name.empty() ? mine.name : i == 0 ? "Player One"
                                                                       : "Player Two";
        s.hasPick = true;
        if (i == p.you && mine.hasPick) {
            s.pick = mine.pick;
        } else if (g_tables.charas.size() > 1) {
            const size_t other = g_choices.chara == 1 ? 0 : 1;
            const auto& costumes = g_tables.costumes[other];
            s.pick.character = PackCode(g_tables.charas[other].c_str());
            s.pick.costume = static_cast<uint16_t>(costumes.size() > 1 ? costumes[1] : costumes[0]);
            s.pick.color = 2;
        }
        s.ready = p.ready[i];
        s.preloaded = true;
        s.choice = p.choice[i] == 1 ? lobby::Choice::Rematch : p.choice[i] == 2 ? lobby::Choice::CharacterSelect
                                                                                : lobby::Choice::None;
        s.wins = static_cast<uint16_t>(i == 0 ? 2 : 1);
        s.inputDelay = static_cast<uint8_t>(i == p.you ? g_choices.delay : 1);
        s.fps = 60;
        s.link = i == 0 ? lobby::Link::Wired : lobby::Link::WiFi;
    }
    v.frame = 4210;
    v.pingMs = 41;
    v.avgPingMs = 40.6f;
    v.jitterMs = 3;
    v.framesAhead = 0.4f;
    v.rollbacks = 57;
    v.longestRollback = 4;
    v.inputDelay = g_choices.delay;
    v.predictionWindow = 8;
    v.linkPath = p.relayed ? NetView::Path::Relayed : NetView::Path::Direct;
    v.relayHost = "turn.cloudflare.com";
    if (p.desync) {
        v.desyncFrame = 3812;
        v.desyncReport = "desync-preview.txt";
    }
    return v;
}

std::mutex g_previewMutex;
std::string g_previewWords; // from PreviewOverlay, applied on the render thread

// Polarity settings' rows from the live state (settings_screen.h), and what
// changing them does.
void UseServer(const std::string& chosen) {
    if (chosen.empty()) return;
    Act([chosen] {
        NetplayOptions& o = NetOptions();
        o.server = o.savedServer = chosen;
        auto& recent = o.recentServers;
        recent.erase(std::remove(recent.begin(), recent.end(), chosen), recent.end());
        recent.insert(recent.begin(), chosen);
        if (recent.size() > 6) recent.resize(6);
        if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, o, o.stageCode);
        Log("overlay: server %s for the next sessions", chosen.c_str());
    });
}

// The servers Polarity settings offers: the public one first, then the ones used before.
std::vector<std::string> ServerChoices() {
    std::vector<std::string> list{kDefaultServer};
    for (const std::string& s : NetOptions().recentServers) {
        if (std::find(list.begin(), list.end(), s) == list.end()) list.push_back(s);
    }
    if (std::find(list.begin(), list.end(), NetOptions().server) == list.end()) list.push_back(NetOptions().server);
    return list;
}

std::string ServerLabel(const std::string& server) { return server == kDefaultServer ? "Public server" : server; }

std::vector<SettingsRow> SettingsRows() {
    std::vector<SettingsRow> rows;
    auto row = [&](const char* id, const char* section, const std::string& label) -> SettingsRow& {
        SettingsRow& r = rows.emplace_back();
        r.id = id;
        r.section = section;
        r.label = label;
        return r;
    };
    const UpdateView u = UpdateViewNow();
    using U = UpdateView::State;
    const std::string current = u.current.empty() ? g_build : "Polarity " + u.current;
    // Updates.
    if (u.state == U::Available || u.state == U::Downloading || u.state == U::Ready) {
        SettingsRow& r = row("install", "UPDATES", "Install " + u.latest);
        r.highlight = true;
        r.kicker = "NEW VERSION";
        r.title = "Polarity " + u.latest;
        if (!u.notes.empty()) r.lines.push_back((u.published.empty() ? "" : "From " + u.published + ": ") + u.notes);
        r.goldFirst = !r.lines.empty();
        r.lines.push_back("You can only play people on the same version.");
        if (u.state == U::Available) {
            r.value = "Download and install";
            r.action = "Install";
            r.hint = "Installs the next time the game starts.";
        } else if (u.state == U::Downloading) {
            const int pct = u.total ? static_cast<int>(100 * u.done / u.total) : 0;
            r.value = "Downloading " + std::to_string(pct) + "%";
            r.hint = "Keep playing. It installs the next time the game starts.";
        } else {
            r.value = "Restart the game to use it";
            r.action = "Restart";
            r.hint = "";
        }
    }
    {
        SettingsRow& r = row("check", "UPDATES", "Check now");
        r.highlight = true;
        r.value = u.state == U::Checking ? "Checking..." : u.state == U::Off ? "Off"
                                                                             : "Check";
        r.enabled = u.state != U::Off;
        r.action = u.state == U::Checking ? "" : "Check";
        r.kicker = "UPDATES";
        r.title = current;
        switch (u.state) {
            case U::Off: r.lines = {"Updates are off: " + u.message + "."}; break;
            case U::Idle: r.lines = {"Not checked yet."}; break;
            case U::Checking: r.lines = {"Checking for updates..."}; break;
            case U::UpToDate: r.lines = {"You're up to date."}; break;
            case U::Available: r.lines = {"Polarity " + u.latest + " is available."}; break;
            case U::Downloading: r.lines = {"Downloading Polarity " + u.latest + "."}; break;
            case U::Ready: r.lines = {"Polarity " + u.latest + " installs when you restart the game."}; break;
            case U::Failed: r.lines = {"The check failed: " + u.message + "."}; break;
        }
        r.hint = "Nothing installs without asking you.";
    }
    {
        SettingsRow& r = row("checkstart", "UPDATES", "Check at start");
        r.arrows = true;
        r.value = ReadMainIni(L"update", L"check", 1) ? "On" : "Off";
        r.kicker = "UPDATES";
        r.title = "Check at start";
        r.lines = {"Checks for a new version each time the game starts. Nothing installs without asking you."};
    }
    // Online.
    {
        SettingsRow& r = row("name", "ONLINE", "Name");
        r.text = true;
        r.editValue = g_choices.name;
        r.value = g_choices.name[0] ? std::string(g_choices.name) : NetLocalPlayerName() + " (Steam name)";
        r.kicker = "ONLINE";
        r.title = g_choices.name[0] ? std::string(g_choices.name) : NetLocalPlayerName();
        r.lines = {"Shown to other players. Leave it empty to use your Steam name.", "On Steam Deck, press Steam + X for the keyboard."};
    }
    {
        SettingsRow& r = row("fps", "ONLINE", "My frame rate");
        r.arrows = true;
        r.value = g_choices.showFps ? "Shown" : "Hidden";
        r.kicker = "ONLINE";
        r.title = "My frame rate";
        r.lines = {"Shows your own frame rate next to your name during matches."};
    }
    {
        SettingsRow& r = row("country", "ONLINE", "Country flag");
        r.arrows = true;
        r.value = g_choices.shareCountry ? "Shown" : "Hidden";
        r.kicker = "ONLINE";
        r.title = "Country flag";
        r.lines = {"Shows your country's flag next to your name in rooms, based on your connection.",
                   "When hidden, your country isn't shown to anyone."};
    }
    {
        SettingsRow& r = row("wordfilter", "ONLINE", "Word filter");
        r.arrows = true;
        r.value = g_choices.wordFilter ? "On" : "Off";
        r.kicker = "ONLINE";
        r.title = "Word filter";
        r.lines = {"Hides offensive names and words in public rooms. Hidden names show as \"Player 1234\".",
                   "The room list is always filtered. Private rooms never are."};
    }
    {
        SettingsRow& r = row("streamer", "ONLINE", "Streamer mode");
        r.arrows = true;
        r.value = g_choices.streamerMode ? "On" : "Off";
        r.kicker = "ONLINE";
        r.title = "Streamer mode";
        r.lines = {"Hides room codes on screen, for streams and screenshots.", "Copy code still copies the real code."};
    }
    {
        SettingsRow& r = row("delay", "ONLINE", "Input delay");
        const int fixed = NetOptions().delaySetting;
        r.arrows = true;
        r.value = fixed >= 0 ? FramesText(fixed) : "Auto";
        r.kicker = "ONLINE";
        r.title = fixed >= 0 ? "Input delay: " + FramesText(fixed) : "Input delay: Auto";
        r.lines = {fixed == 0  ? std::string("Every match starts with no input delay.")
                   : fixed > 0 ? "Every match starts with " + FramesText(fixed) + " of input delay."
                               : std::string("Picks 0 to 3 frames for each match, based on the connection."),
                   "Lower feels sharper but rolls back more. You can still change it before each match."};
    }
    {
        SettingsRow& r = row("server", "ONLINE", "Server");
        const std::vector<std::string> servers = ServerChoices();
        r.arrows = servers.size() > 1;
        r.value = ServerLabel(NetOptions().server);
        r.kicker = "ONLINE";
        r.title = ServerLabel(NetOptions().server);
        r.lines = {"Custom Rooms and Direct Match use this server.", "Other servers appear here once you've used them."};
    }
    // Training (hitbox_display.h).
    {
        SettingsRow& r = row("hitboxes", "TRAINING", "Hitboxes");
        r.arrows = true;
        r.value = HitboxesInTraining() ? "Shown" : "Hidden";
        r.kicker = "TRAINING";
        r.title = "Hitboxes";
        r.lines = {"Shows hurtboxes, hitboxes, throw and proximity boxes in Training mode."};
    }
    {
        SettingsRow& r = row("framemeter", "TRAINING", "Frame meter");
        r.arrows = true;
        r.value = FrameMeterInTraining() ? "Shown" : "Hidden";
        r.kicker = "TRAINING";
        r.title = "Frame meter";
        r.lines = {"Shows each frame of both characters' actions above the V-Trigger gauges in Training mode, with startup, total and frame advantage."};
    }
    // The benchmark (benchmark.h).
    {
        SettingsRow& r = row("bench", "BENCHMARK", "Run benchmark");
        r.highlight = true;
        r.value = "Run";
        r.action = "Run";
        r.hint = "Plays one round with a rollback on every frame.";
    }
    {
        const BenchmarkResult b = LastBenchmark();
        SettingsRow& r = row("benchlast", "BENCHMARK", "Last result");
        if (b.valid) {
            const std::time_t t = static_cast<std::time_t>(b.when);
            std::tm tm{};
            localtime_s(&tm, &t);
            static const char* const kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            char v[64];
            std::snprintf(v, sizeof(v), "%s \xC2\xB7 %.1f fps \xC2\xB7 %s %d", b.Pass() ? "Pass" : "Fail", b.averageFps, kMon[tm.tm_mon], tm.tm_mday);
            r.value = v;
            r.action = "Show";
            r.hint = "Your last run.";
        } else {
            r.value = "None yet";
            r.enabled = false;
        }
    }
    // Reports.
    {
        SettingsRow& r = row("reports", "REPORTS", "Send reports");
        r.arrows = true;
        r.value = g_choices.uploadReports ? "On" : "Off";
        r.kicker = "REPORTS";
        r.title = "Send reports";
        r.lines = {"Sends desync and crash reports to help fix bugs.",
                   "A desync report has the match's recording and log (names, picks, inputs). A crash report has the end of the "
                   "log and a small crash dump, sent at the next start."};
    }
    {
        SettingsRow& r = row("folder", "REPORTS", "Polarity folder");
        r.highlight = true;
        r.value = "Open";
        r.action = "Open";
        r.kicker = "POLARITY FOLDER";
        std::string path;
        for (wchar_t ch : DataDir()) path += ch < 128 ? static_cast<char>(ch) : '_';
        r.title = path;
        r.lines = {"Holds logs, match recordings and crash reports."};
        r.hint = "Opens in Explorer. The game stays open.";
    }
    {
        SettingsRow& r = row("pack", "REPORTS", "Pack logs");
        r.highlight = true;
        r.value = "Pack to share";
        r.action = "Pack";
        r.kicker = "PACK LOGS";
        r.title = "Pack logs";
        r.lines = {"Zips logs and recent recordings into one file in the Polarity folder, to send with a bug report."};
        if (!g_packResult.empty()) r.lines.push_back(g_packResult);
    }
    // Third-party acknowledgements, all in one place (last).
    {
        SettingsRow& r = row("credits", "ABOUT", "Acknowledgements");
        r.kicker = "ABOUT";
        r.title = "Acknowledgements";
        r.lines = {"Dear ImGui by Omar Cornut, MIT",
                   "GekkoNet and Weyvelength by Jamie Meyer, BSD 2-Clause",
                   "MinHook by Tsuda Kageyu, BSD 2-Clause",
                   "libjuice by Paul-Louis Ageneau, MPL 2.0",
                   "Asio by Christopher M. Kohlhoff, Boost Software License 1.0",
                   "zpp::bits by Eyal Z, MIT",
                   "Barlow, Archivo Black and Noto Sans CJK fonts, SIL Open Font License 1.1",
                   "IP Geolocation by DB-IP (db-ip.com), CC BY 4.0",
                   "List of Dirty, Naughty, Obscene, and Otherwise Bad Words (LDNOOBW), CC BY 4.0"};
    }
    return rows;
}

// The benchmark's modals over Polarity settings: before a run, its loading,
// and a result.
enum class BenchModal { None,
                        Confirm,
                        Loading,
                        Result };
BenchModal g_benchModal = BenchModal::None;
int g_benchChoice = 0;
BenchmarkResult g_benchShown;
bool g_benchReturn = false; // a run is on: settings and its result again at the menus

// Loading a run: settings stays, under a modal that takes no input, while
// the round is read, its characters and stage load and the jump switches to
// the battle scene; then black (held while the game's own screen goes
// black) and settings closes, as the Battle Log's Watch does. Gone at once,
// settings sat still for the read and the main menu showed for the preload.
struct BenchLaunch {
    bool active = false;
    int step = 0;  // 0 reading the round, 1 loading, 2 starting, 3 fading into it
    int drawn = 0; // frames the modal has been shown (the read waits for one)
    uint64_t switchesAt = 0;
    double since = 0, stepSince = 0, blackFrom = -1, revealFrom = -1;
};

BenchLaunch g_benchLaunch;
constexpr double kBenchToBlack = 0.3, kBenchReveal = 0.5, kBenchGiveUp = 60;

void OpenBenchmarkModal(bool result) {
    g_benchChoice = 0;
    if (result) {
        g_benchShown = LastBenchmark();
        g_benchModal = g_benchShown.valid ? BenchModal::Result : BenchModal::None;
    } else {
        g_benchModal = BenchModal::Confirm;
    }
}

void StartBenchmarkRun() {
    // The read itself waits for the modal's first frame (it holds this thread).
    g_benchModal = BenchModal::Loading;
    g_benchLaunch = {};
    g_benchLaunch.active = true;
    g_benchLaunch.since = g_benchLaunch.stepSince = ImGui::GetTime();
}

// The launch's progress, each frame.
void StepBenchmarkLaunch() {
    BenchLaunch& l = g_benchLaunch;
    const double now = ImGui::GetTime();
    if (l.step == 0) {
        if (l.drawn < 1) return;
        if (!StartBenchmark(BuildVersion())) {
            g_benchModal = BenchModal::None;
            l = {};
            AddToast("The benchmark can't start now.", kRed, 4);
            return;
        }
        l.switchesAt = BattleSceneSwitches();
        l.step = 1;
        l.stepSince = now;
    }
    if (l.step == 1 && BattleSceneSwitches() != l.switchesAt) {
        l.step = 2;
        l.stepSince = l.blackFrom = now;
    }
    if (l.step == 1 && now - l.since > kBenchGiveUp) {
        Log("benchmark: no battle scene %.0f s after Run: stopping it", kBenchGiveUp);
        ControlReplay(ReplayControl::Leave);
        g_benchModal = BenchModal::None;
        l = {};
        AddToast("The benchmark didn't start. Try again.", kRed, 5);
        return;
    }
    // Black: settings goes (the game's own screen is black under it).
    if (l.step == 2 && g_benchModal == BenchModal::Loading && now - l.blackFrom >= kBenchToBlack) {
        g_benchModal = BenchModal::None;
        g_benchReturn = true;
        CloseSettingsScreen();
        CloseHub();
    }
    // The round's first frame has run: from black into it.
    const ReplayView v = ReplayViewNow();
    const bool running = v.active && v.benchmark && v.frame > 0;
    if (l.step == 2 && g_benchModal == BenchModal::None && (running || now - l.stepSince > kBenchGiveUp)) {
        if (!running) Log("benchmark: the round has not started %.0f s after its battle scene: showing it", kBenchGiveUp);
        l.step = 3;
        l.revealFrom = now;
    }
    if (l.step == 3 && now - l.revealFrom >= kBenchReveal) l = {};
    // The replay ended before its first frame (it could not load): no black left behind.
    if (l.step == 2 && g_benchModal == BenchModal::None && !v.active) l = {};
}

// The launch's modal and its black, over settings and then over the
// battle's loading, above everything the hub draws.
void DrawBenchmarkLaunch(const HubFonts& f) {
    using namespace ui;
    BenchLaunch& l = g_benchLaunch;
    const double now = ImGui::GetTime();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    // Black: in at the battle scene's switch, out once the round runs; the
    // modal goes first.
    double black = l.blackFrom < 0 ? 0 : std::min(1.0, (now - l.blackFrom) / kBenchToBlack);
    float alpha = 1;
    if (l.step == 3) {
        const double out = std::min(1.0, (now - l.revealFrom) / kBenchReveal);
        black *= 1 - out;
        alpha = static_cast<float>(std::max(0.0, 1 - out * 2));
    }
    if (black > 0) d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(black * 255)));
    if (alpha > 0) {
        const int firstVertex = d->VtxBuffer.Size;
        g_inputOn = false;
        ModalSpec m;
        m.x0 = 480, m.y0 = 270, m.w = 640, m.h = 290;
        m.kicker = "BENCHMARK";
        m.hint = "It starts by itself.";
        int clicked = -1;
        float y = Modal(d, bold, f.body, m, &clicked);
        Text(d, f.display, g_v.L(28), g_v.P(m.x0 + 28, y), look::kCream, "Getting the round ready");
        y += 62;
        // The steps as the room's hand-off shows them: a ring, ticked when
        // done, a dot going round while working.
        const char* const kSteps[] = {"Reading the round", "Loading the characters and the stage", "Starting the round"};
        for (int i = 0; i < 3; i++) {
            const int state = i < l.step ? 2 : i == l.step ? 1
                                                           : 0;
            const ImVec2 c = g_v.P(m.x0 + 40, y + 11);
            const ImU32 ring = state == 2 ? look::kOk : state == 1 ? look::kRing
                                                                   : look::kEdgeControl;
            d->AddCircle(c, g_v.L(12), ring, 24, g_v.L(2));
            if (state == 2) {
                const ImVec2 tick[] = {g_v.P(m.x0 + 34, y + 11), g_v.P(m.x0 + 38.5f, y + 16), g_v.P(m.x0 + 46.5f, y + 6)};
                d->AddPolyline(tick, 3, look::kOk, 0, g_v.L(2.2f));
            } else if (state == 1) {
                const float turn = static_cast<float>(now * 4.0);
                d->AddCircleFilled(g_v.P(m.x0 + 40 + 7 * std::cos(turn), y + 11 + 7 * std::sin(turn)), g_v.L(2.6f), look::kRing);
            }
            std::string text = kSteps[i];
            if (state == 1 && now - l.stepSince >= 3) text += " \xC2\xB7 " + std::to_string(static_cast<int>(now - l.stepSince)) + " s";
            Text(d, f.body, g_v.L(17), g_v.P(m.x0 + 66, y), state == 0 ? look::kMuted : look::kCream, text);
            y += 36;
        }
        // Fading out: everything the modal drew, at the modal's alpha.
        if (alpha < 1) {
            for (int i = firstVertex; i < d->VtxBuffer.Size; i++) {
                ImU32& col = d->VtxBuffer[i].col;
                const ImU32 a = static_cast<ImU32>(((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
                col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
            }
        }
    }
    l.drawn++;
    StepBenchmarkLaunch();
}

// What the result says about the install's battle data (benchmark.h
// mods/data): kind -1 nothing to say (a run from before the check), 0 fine,
// 1 a mod changes some characters' data, 2 this round's data was changed.
struct BenchDataLine {
    int kind = -1;
    std::string head, body;
};

BenchDataLine BenchmarkDataLine(const BenchmarkResult& b) {
    // "JRI,KEN,CMN" -> "Juri, Ken and every character's shared data".
    auto names = [](const std::string& codes) {
        std::vector<std::string> out;
        size_t from = 0;
        while (from < codes.size()) {
            const size_t comma = codes.find(',', from);
            const std::string code = codes.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
            from = comma == std::string::npos ? codes.size() : comma + 1;
            if (code.empty()) continue;
            const char* name = keeper::CharacterName(code);
            out.push_back(code == "CMN" ? "every character's shared data" : code == "?" ? "a mod that can't be read"
                                                                        : *name         ? name
                                                                                        : code);
        }
        std::string s;
        for (size_t i = 0; i < out.size(); i++) s += (i == 0 ? "" : i + 1 == out.size() ? " and "
                                                                                        : ", ") +
                                                     out[i];
        return s;
    };
    BenchDataLine l;
    if (b.DataChanged()) {
        l.kind = 2;
        l.head = "This round's battle data isn't the game's own: " + names(b.data.substr(8)) + ".";
        l.body = "A mod changes it. Matches with these characters fall out of sync against players without that mod.";
        return l;
    }
    if (b.ModsChangeBattle()) {
        const size_t colon = b.mods.find(':');
        l.kind = 1;
        l.head = "Mods change the battle data of " + (colon == std::string::npos ? std::string("some characters") : names(b.mods.substr(colon + 1))) + ".";
        l.body = "Matches with them fall out of sync against players who don't have the same mods.";
        return l;
    }
    if (b.mods.empty()) return l;
    l.kind = 0;
    if (b.mods == "none") l.head = "No mods are installed.";
    else {
        const int count = std::atoi(b.mods.c_str());
        l.head = std::to_string(count) + (count == 1 ? " mod is installed. It doesn't" : " mods are installed. None of them") + " change battle data.";
    }
    return l;
}

void DrawBenchmarkModal(const HubFonts& f, bool takeInput) {
    using namespace ui;
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    g_inputOn = takeInput;
    SetUiSoundSet(UiSoundSet::Menu);
    int clicked = -1;
    if (g_benchModal == BenchModal::Confirm) {
        ModalSpec m;
        m.x0 = 480, m.y0 = 250, m.w = 640, m.h = 330;
        m.kicker = "BENCHMARK";
        m.prompts = {{"button_a", "Choose"}, {"button_b", "Back"}};
        float y = Modal(d, bold, f.body, m, &clicked);
        Text(d, f.display, g_v.L(28), g_v.P(m.x0 + 28, y), look::kCream, "Run the benchmark?");
        y += 50;
        ImGui::PushFont(f.body);
        const float wrap = g_v.L(m.w - 56);
        d->AddText(f.body, TextPx(19), g_v.P(m.x0 + 28, y), look::kCream,
                   "You watch one round played with a rollback on every frame, the most a match can have. It takes about a minute.",
                   nullptr, wrap);
        d->AddText(f.body, TextPx(19), g_v.P(m.x0 + 28, y + 62), look::kSoft, "Close other programs for a fair result.", nullptr, wrap);
        ImGui::PopFont();
        const float by = m.y0 + m.h - 46 - 22 - 48;
        const bool run = Button(d, bold, m.x0 + 28, by, m.x0 + m.w / 2 - 6, by + 48, "Run", g_benchChoice == 0);
        const bool later = Button(d, bold, m.x0 + m.w / 2 + 6, by, m.x0 + m.w - 28, by + 48, "Not now", g_benchChoice == 1);
        if (takeInput) {
            if (Left() || Right()) g_benchChoice = 1 - g_benchChoice;
            if ((Confirm() && g_benchChoice == 0) || run) StartBenchmarkRun();
            else if ((Confirm() && g_benchChoice == 1) || later || Back()) g_benchModal = BenchModal::None;
        }
    } else if (g_benchModal == BenchModal::Result) {
        const BenchmarkResult& b = g_benchShown;
        const bool pass = b.Pass();
        const BenchDataLine data = BenchmarkDataLine(b);
        ModalSpec m;
        m.x0 = 350, m.y0 = 120, m.w = 900, m.h = (b.matched ? (pass ? 470.0f : 510.0f) : 510.0f) + (data.kind < 0 ? 0.0f : data.body.empty() ? 44.0f
                                                                                                                                             : 74.0f);
        const std::time_t t = static_cast<std::time_t>(b.when);
        std::tm tm{};
        localtime_s(&tm, &t);
        static const char* const kMon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
        char kick[64];
        std::snprintf(kick, sizeof(kick), "BENCHMARK \xC2\xB7 %s %d, %02d:%02d", kMon[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min);
        m.kicker = kick;
        m.aside = "Polarity " + b.version;
        m.danger = !b.matched;
        m.prompts = {{"button_a", "Choose"}, {"button_b", "Back"}};
        float y = Modal(d, bold, f.body, m, &clicked);
        const ImU32 verdict = pass ? look::kOk : !b.matched ? look::kDanger
                                                            : look::kGold;
        Text(d, f.display, g_v.L(64), g_v.P(m.x0 + 28, y - 4), verdict, pass ? "PASS" : "FAIL");
        const float vx = m.x0 + 28 + TextW(f.display, 64, pass ? "PASS" : "FAIL") + 28;
        char avg[48];
        std::snprintf(avg, sizeof(avg), "%.1f fps average", b.averageFps);
        Text(d, f.display, g_v.L(28), g_v.P(vx, y + 6), look::kCream, avg);
        Text(d, f.body, g_v.L(19), g_v.P(vx, y + 44), look::kSoft, "A pass needs 59 or more.");
        y += 92;
        // Three figures.
        char a[32], s2[32], c[48];
        std::snprintf(a, sizeof(a), "%.1f fps", b.lowFps);
        std::snprintf(s2, sizeof(s2), "%.1f ms", b.slowestMs);
        std::snprintf(c, sizeof(c), "%d of %d", b.slowFrames, b.frames);
        const std::pair<const char*, std::string> figures[] = {{"1% low", a}, {"Slowest frame", s2}, {"Frames over 20 ms", c}};
        const float fw = (m.w - 56 - 24) / 3;
        for (int i = 0; i < 3; i++) {
            const float x0 = m.x0 + 28 + i * (fw + 12);
            Control(d, x0, y, x0 + fw, y + 80, false);
            Text(d, bold, g_v.L(17), g_v.P(x0 + 18, y + 12), look::kMuted, figures[i].first);
            Text(d, f.display, g_v.L(26), g_v.P(x0 + 18, y + 38), look::kCream, figures[i].second);
        }
        y += 98;
        char times[128];
        std::snprintf(times, sizeof(times), "Each frame: rollbacks %.1f ms \xC2\xB7 presenting %.1f ms \xC2\xB7 %.1f ms in all", b.rollbackMs + b.gameMs,
                      b.drawMs, b.seconds * 1000.0 / std::max(1, b.frames));
        Text(d, bold, g_v.L(19), g_v.P(m.x0 + 28, y), look::kSoft, times);
        y += 40;
        d->AddLine(g_v.P(m.x0 + 28, y), g_v.P(m.x0 + m.w - 28, y), Rgba(236, 228, 206, 36), std::max(1.0f, g_v.L(1)));
        y += 14;
        if (b.matched) {
            d->AddLine(g_v.P(m.x0 + 30, y + 13), g_v.P(m.x0 + 36, y + 19), look::kOk, g_v.L(2.4f));
            d->AddLine(g_v.P(m.x0 + 36, y + 19), g_v.P(m.x0 + 46, y + 7), look::kOk, g_v.L(2.4f));
            Text(d, bold, g_v.L(19), g_v.P(m.x0 + 56, y), look::kCream, "The round played exactly as recorded.");
            if (!pass) {
                const bool rollbacks = b.rollbackMs + b.gameMs >= b.drawMs;
                Text(d, f.body, g_v.L(19), g_v.P(m.x0 + 56, y + 30), look::kSoft,
                     rollbacks ? "Rollbacks took the most time. With 1 or 2 frames of input delay, a match rolls back less."
                               : "Drawing the game took the most time. Lower graphics settings may help.");
            }
        } else {
            d->AddLine(g_v.P(m.x0 + 30, y + 7), g_v.P(m.x0 + 44, y + 21), look::kDanger, g_v.L(2.4f));
            d->AddLine(g_v.P(m.x0 + 44, y + 7), g_v.P(m.x0 + 30, y + 21), look::kDanger, g_v.L(2.4f));
            Text(d, bold, g_v.L(19), g_v.P(m.x0 + 56, y), look::kCream,
                 "The round played differently from frame " + std::to_string(b.firstDiffer) + ".");
            Text(d, f.body, g_v.L(19), g_v.P(m.x0 + 56, y + 30), look::kSoft,
                 data.kind == 2 ? "This PC would fall out of sync online."
                                : "This PC would fall out of sync online. Pack logs in Polarity settings and send them in.");
        }
        // The install's battle data: a tick, or a warning sign with what the
        // mods change.
        if (data.kind >= 0) {
            float dy = y + (b.matched && pass ? 44 : 74);
            const ImU32 mark = data.kind == 0 ? look::kOk : data.kind == 1 ? look::kGold
                                                                           : look::kDanger;
            if (data.kind == 0) {
                d->AddLine(g_v.P(m.x0 + 30, dy + 13), g_v.P(m.x0 + 36, dy + 19), mark, g_v.L(2.4f));
                d->AddLine(g_v.P(m.x0 + 36, dy + 19), g_v.P(m.x0 + 46, dy + 7), mark, g_v.L(2.4f));
            } else {
                d->AddTriangle(g_v.P(m.x0 + 38, dy + 4), g_v.P(m.x0 + 47, dy + 21), g_v.P(m.x0 + 29, dy + 21), mark, g_v.L(2.0f));
                d->AddLine(g_v.P(m.x0 + 38, dy + 10), g_v.P(m.x0 + 38, dy + 15), mark, g_v.L(2.0f));
            }
            Text(d, bold, g_v.L(19), g_v.P(m.x0 + 56, dy), look::kCream, data.head);
            if (!data.body.empty()) Text(d, f.body, g_v.L(19), g_v.P(m.x0 + 56, dy + 30), look::kSoft, data.body);
        }
        const float by = m.y0 + m.h - 46 - 22 - 48;
        const bool done = Button(d, bold, m.x0 + 28, by, m.x0 + m.w / 2 - 6, by + 48, "Done", g_benchChoice == 0);
        const bool again = Button(d, bold, m.x0 + m.w / 2 + 6, by, m.x0 + m.w - 28, by + 48, "Run again", g_benchChoice == 1);
        if (takeInput) {
            if (Left() || Right()) g_benchChoice = 1 - g_benchChoice;
            if ((Confirm() && g_benchChoice == 1) || again) StartBenchmarkRun();
            else if ((Confirm() && g_benchChoice == 0) || done || Back()) g_benchModal = BenchModal::None;
        }
    }
    if (clicked >= 0) ClickPad(PadKeyOfGlyph(clicked == 0 ? "button_a" : "button_b"));
    g_inputOn = false;
}

void ApplySetting(const SettingsInput& in) {
    if (in.row == "install" && in.confirm) {
        if (UpdateViewNow().state == UpdateView::State::Ready) {
            std::string error;
            if (!RestartGame(g_window, &error)) g_packResult = "Couldn't restart: " + error + ". Close the game and start it again.";
        } else UpdateInstall();
    } else if (in.row == "check" && in.confirm) UpdateCheck();
    else if (in.row == "bench" && in.confirm) OpenBenchmarkModal(false);
    else if (in.row == "benchlast" && in.confirm) OpenBenchmarkModal(true);
    else if (in.row == "checkstart" && in.delta) SaveMainIni(L"update", L"check", ReadMainIni(L"update", L"check", 1) ? 0 : 1);
    else if (in.row == "hitboxes" && in.delta) SetHitboxesInTraining(!HitboxesInTraining());
    else if (in.row == "framemeter" && in.delta) SetFrameMeterInTraining(!FrameMeterInTraining());
    else if (in.row == "name" && in.typed) {
        std::snprintf(g_choices.name, sizeof(g_choices.name), "%s", in.text.c_str());
        SaveChoices();
    } else if (in.row == "delay" && in.delta) {
        // Auto, 0, 1, ... 8, around.
        const int n = kMaxDelaySetting + 2;
        const int8_t next = static_cast<int8_t>(((NetOptions().delaySetting + 1 + in.delta) % n + n) % n - 1);
        NetOptions().delaySetting = next; // shown at once; saved on the game thread
        Act([next] {
            NetOptions().delaySetting = next;
            if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, NetOptions(), NetOptions().stageCode);
            Log("overlay: input delay setting %s", next >= 0 ? std::to_string(next).c_str() : "auto");
        });
    } else if (in.row == "fps" && in.delta) {
        g_choices.showFps = !g_choices.showFps;
        SaveChoices();
    } else if (in.row == "country" && in.delta) {
        g_choices.shareCountry = !g_choices.shareCountry;
        SaveChoices();
    } else if (in.row == "wordfilter" && in.delta) {
        g_choices.wordFilter = !g_choices.wordFilter;
        SetRoomsWordFilter(g_choices.wordFilter); // at once
        SaveChoices();
    } else if (in.row == "streamer" && in.delta) {
        g_choices.streamerMode = !g_choices.streamerMode;
        SetStreamerMode(g_choices.streamerMode); // at once, not on the game thread's turn
        SaveChoices();
    } else if (in.row == "reports" && in.delta) {
        g_choices.uploadReports = !g_choices.uploadReports;
        SaveChoices();
    } else if (in.row == "server" && in.delta) {
        const std::vector<std::string> servers = ServerChoices();
        const auto at = std::find(servers.begin(), servers.end(), NetOptions().server);
        const int i = at == servers.end() ? 0 : static_cast<int>(at - servers.begin());
        const int n = static_cast<int>(servers.size());
        const std::string next = servers[((i + in.delta) % n + n) % n];
        if (next != NetOptions().server) {
            UseServer(next);
            RoomsClose(); // the hub opens its connection again, to the new server
        }
    } else if (in.row == "folder" && in.confirm) OpenDataFolder();
    else if (in.row == "pack" && in.confirm) {
        std::string error;
        const std::wstring zip = PackLogsToShare(&error);
        if (zip.empty()) g_packResult = "Could not pack the logs: " + error;
        else {
            std::string name;
            for (wchar_t ch : zip.substr(zip.find_last_of(L'\\') + 1)) name += ch < 128 ? static_cast<char>(ch) : '_';
            g_packResult = "Packed " + name;
            OpenDataFolder(zip);
        }
    }
}

std::mutex g_favoritesMutex;
ProfileFavorites g_favorites; // read at the title's warm up

// A table's set (M4): the keeper's ticket starts a session on the match
// ticket path (M2), ready at once, with the player's pick (My character),
// stage (P1's) and the delay chosen on the your-turn card.
int g_tableDelay = -1;
bool g_myCharacterOpen = false;
void ApplyProfileFavorite(Choices& c);
// A table's set took this player from the rooms: back at the menus the hub
// comes back to them, also when the room is gone (removed mid-set: the
// custom rooms screen says so).
bool g_backToRooms = false;

// Watching a table (M5): its set's match room as a spectator, from the
// set's first game; the room screen comes back after it, and the next set's
// match room is the next watch ticket.
int g_watchTable = -1;

// Training while waiting at a table (the room screen's LB): the game's own
// Training select and Training, until this player's table calls them; then
// the main menu, where the room screen comes back with the your-turn card.
// The game's "Go to Main Menu" leads back to the room screen the same way.
enum class RoomTraining { Off,
                          Asked,
                          In };
RoomTraining g_roomTraining = RoomTraining::Off;
double g_roomTrainingAt = 0;

bool InTrainingScene() { return CurrentScene().find("Training") != std::string::npos; }

void StartRoomTraining(const RoomsView& rooms) {
    const NetView nv = NetViewNow();
    if (g_roomTraining != RoomTraining::Off || (nv.phase != NetView::Phase::Idle && nv.phase != NetView::Phase::Ended)) return;
    // Watching another table would open its matches over Training.
    if (rooms.watching >= 0) {
        RoomsWatch(-1);
        g_watchTable = -1;
    }
    g_roomTraining = RoomTraining::Asked;
    g_roomTrainingAt = ImGui::GetTime();
    Log("overlay: Training while waiting at a table");
    ChangeScene("TrainingBattleSetupState");
}

// Each frame: follows the scene, and ends Training when the table calls.
void StepRoomTraining() {
    if (g_roomTraining == RoomTraining::Off) return;
    const bool training = InTrainingScene();
    if (g_roomTraining == RoomTraining::Asked) {
        if (training) g_roomTraining = RoomTraining::In;
        else if (ImGui::GetTime() - g_roomTrainingAt > 10) {
            Log("overlay: Training did not start");
            g_roomTraining = RoomTraining::Off;
        }
        return;
    }
    if (!training) {
        Log("overlay: back from Training (%s)", CurrentScene().c_str());
        g_roomTraining = RoomTraining::Off;
        return;
    }
    const RoomsView rooms = RoomsSnapshot();
    if (rooms.state != RoomsView::State::Room) return;
    for (size_t t = 0; t < rooms.tableViews.size(); t++) {
        const keeper::TableState& s = rooms.tableViews[t].s;
        if (s.phase != keeper::TablePhase::Call || (s.seat[0] != rooms.self && s.seat[1] != rooms.self)) continue;
        Log("overlay: Table %zu calls (call %u): Training ends", t + 1, s.call);
        g_roomTraining = RoomTraining::Off;
        ChangeScene("MainMenuState");
        return;
    }
}

void StartTableWatch(const WatchTicket& w) {
    g_backToRooms = true;
    g_watchTable = w.table;
    MatchTicket mt;
    mt.host = false;
    mt.spectate = true;
    mt.watchFromFirst = true;
    mt.roomCode = w.code;
    ParseRules(w.rules, &mt.rules);
    mt.after = AfterMatch::Set;
    LoadTables();
    InitChoices();
    const NetplayOptions o = ChosenOptions();
    Log("overlay: watching table %d's set: joining %s", w.table + 1, w.code.c_str());
    Act([mt, o] { NetPlayTicket(mt, o); });
}

void StartTableSession(const keeper::TableTicket& t) {
    g_backToRooms = true;
    MatchTicket mt;
    mt.host = t.host;
    mt.roomCode = t.code;
    mt.seatToken = t.token;
    ParseRules(t.rules, &mt.rules);
    mt.after = AfterMatch::Set;
    // The saved pick (My character), read in if nothing has yet: the first
    // set after a boot played the Fighter Profile's favorite while the room
    // showed the saved pick (2026-11-05, both PCs' game 1).
    LoadTables();
    InitChoices();
    // Nothing saved: the Fighter Profile's favorite with its loadout, as the
    // room shows it. The panel's choices always count as a pick, so they
    // played their defaults instead: the first character in costume 1,
    // color 1 (Abigail for a Seth player, 2026-11-07).
    if (!NetOptions().hasPick) ApplyProfileFavorite(g_choices);
    // Kept to what this profile owns now (entitlements): moved later, in the
    // lobby, the new pick cleared Ready and the set's lobby, which has no
    // select screen, waited for good.
    PickValid(g_choices);
    NetplayOptions o = ChosenOptions();
    o.autoReady = true;
    if (g_tableDelay >= 0) o.inputDelay = static_cast<uint8_t>(g_tableDelay);
    // The delay boxes between games show this set's delay, not one left
    // from an earlier session (2 from a 108 ms opponent while 0 played).
    g_choices.delay = o.inputDelay;
    Log("overlay: table %d's set: %s (delay %u)", t.table + 1, t.host ? "opening the match room" : ("joining " + t.code).c_str(),
        o.inputDelay);
    Act([mt, o] { NetPlayTicket(mt, o); });
    RoomsTableSession(t.table, t.host, t.rules);
}

// My character from the room (Y; mock-up board 5): the select screen for one
// player, with the CURRENT LOADOUT card where the other player would be. The
// grid and loadout change the panel's pick (g_choices) as the session's
// select screen does; Confirm saves it (the panel ini, the netplay options:
// the menus' preload loads it, the room sees it); B on the grid keeps the
// pick that was in effect. X opens the stage select: the preferred stage,
// played when this player hosts as P1. Input delay is the player's usual
// delay (Polarity settings' Input delay): the your-turn card starts from it.
struct MyCharacter {
    Choices saved;                          // the pick in effect: the card's, B's on the grid
    bool fromProfile = false;               // nothing saved yet: the Fighter Profile's favorite
    bool stagePage = false;                 // on the stage select
    int stageBefore = 1, settingBefore = 0; // the stage as the stage select opened (its B)
    bool savedNow = false;                  // SAVED: A goes back to the room, B changes again
    int8_t delaySetting = kDelayAuto;       // the Input delay row (saved with the pick)
    int openedFrame = 0;                    // the Y that opened it is not read again here
    float alpha = 0;
} g_my;

// Nothing saved yet: the Fighter Profile's favorite character with its saved
// loadout, and the profile's stage as the preferred stage.
void ApplyProfileFavorite(Choices& c) {
    ProfileFavorites fav;
    {
        std::lock_guard lock(g_favoritesMutex);
        fav = g_favorites;
    }
    const std::string code = fav.chara.empty() ? "RYU" : fav.chara;
    const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
    if (it == g_tables.charas.end()) return;
    SwitchCharacter(c, static_cast<int>(it - g_tables.charas.begin()));
    for (const FavoriteChara& f : fav.charas) {
        if (f.code != code || IsEleven(c.chara)) continue;
        c.costume = f.costume;
        c.color = f.color;
        c.vskill = std::clamp(f.vskill + 1, 1, 2);
        c.vtrigger = std::clamp(f.vtrigger + 1, 1, 2);
    }
    for (int i = 0; i < static_cast<int>(g_tables.stages.size()) && !fav.stage.empty(); i++) {
        if (g_tables.stages[i] == fav.stage) c.stage = 2 + i, c.setting = 0;
        const auto& settings = g_tables.settings[i];
        if (const auto s = std::find(settings.begin(), settings.end(), fav.stage); s != settings.end()) {
            c.stage = 2 + i;
            c.setting = 1 + static_cast<int>(s - settings.begin());
        }
    }
    PickValid(c);
}

void OpenMyCharacter() {
    LoadTables();
    InitChoices();
    if (!g_choices.initialised) return; // the lists are still being read
    g_my = MyCharacter{};
    g_my.fromProfile = !NetOptions().hasPick;
    if (g_my.fromProfile) ApplyProfileFavorite(g_choices);
    PickValid(g_choices);
    g_my.saved = g_choices;
    g_my.delaySetting = NetOptions().delaySetting;
    g_my.openedFrame = ImGui::GetFrameCount();
    SetSelectData(PreviewSelectData());
    ResetSelectScreen();
    g_myCharacterOpen = true;
    Log("overlay: my character opened (%s%s)", g_tables.charas[g_choices.chara].c_str(), g_my.fromProfile ? ", the profile's" : "");
}

// Closes it; `keep`: back to the pick that was in effect (what changed since
// the last save is dropped).
void CloseMyCharacter(bool keep, const char* why) {
    if (keep) g_choices = g_my.saved;
    g_myCharacterOpen = false;
    Log("overlay: my character closed (%s): %s", why, g_tables.charas[g_choices.chara].c_str());
}

void SaveMyCharacter() {
    Choices& c = g_choices;
    if (IsEleven(c.chara)) RollEleven(c);
    g_my.saved = c;
    g_my.fromProfile = false;
    SaveChoices();
    const NetplayOptions o = ChosenOptions();
    const int8_t delay = g_my.delaySetting;
    // Outside a session the pick goes to the driver too (an ended one keeps
    // its last session: the menus' preload asks it); a session on its way
    // keeps the pick it started with (its next set plays this one).
    const NetView::Phase phase = NetViewNow().phase;
    const bool idle = phase == NetView::Phase::Idle || phase == NetView::Phase::Ended;
    Act([o, delay, idle] {
        NetOptions().delaySetting = delay;
        NetOptions().stageCode = o.stageCode;
        if (idle) NetSetPick(o.hasPick, o.pick);
        else {
            NetOptions().hasPick = o.hasPick;
            NetOptions().pick = o.pick;
        }
        if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, NetOptions(), o.stageCode);
    });
    Log("overlay: my character saved: %s costume %d color %d, stage %s, delay %s", g_tables.charas[c.chara].c_str(), c.costume, c.color,
        o.stageCode.c_str(), delay >= 0 ? std::to_string(delay).c_str() : "auto");
}

// A new Random stages list: in effect and saved at once (the panel file).
void SetRandomOff(std::vector<std::string> off, const char* why) {
    g_randomOff = ParseStageMask(FormatStageMask(off));
    g_randomOffSaved = true;
    const std::vector<std::string> list = g_randomOff;
    Act([list] {
        NetOptions().randomOff = list;
        NetOptions().randomOffSaved = true;
        if (!g_panelPath.empty()) SaveRandomStagesOff(g_panelPath, list);
    });
    Log("overlay: random stages (%s): leaving out '%s'", why, FormatStageMask(g_randomOff).c_str());
}

// Random's stages as the card says it: the owned stages and settings on,
// of all owned ones (never the never-random stages).
std::string RandomStagesText() {
    int on = 0, owned = 0;
    auto count = [&](const std::string& code, bool have) {
        if (!have || IsNeverRandomStage(code)) return;
        owned++;
        if (std::find(g_randomOff.begin(), g_randomOff.end(), code) == g_randomOff.end()) on++;
    };
    for (size_t i = 0; i < g_tables.stages.size(); i++) {
        if (IsNeverRandomStage(g_tables.stages[i])) continue;
        count(g_tables.stages[i], i < g_tables.stageOwned.size() && g_tables.stageOwned[i]);
        for (size_t k = 0; i < g_tables.settings.size() && k < g_tables.settings[i].size(); k++) {
            const bool have = i < g_tables.settingOwned.size() && k < g_tables.settingOwned[i].size() && g_tables.settingOwned[i][k];
            count(g_tables.settings[i][k], have);
        }
    }
    if (on == owned) return "Random";
    return "Random (" + std::to_string(on) + " of " + std::to_string(owned) + " stages)";
}

// The card's rows: the pick in effect.
SelectCard MyCharacterCard(const RoomsView& rooms) {
    const Choices& c = g_my.saved;
    SelectCard card;
    card.player = NetLocalPlayerName();
    if (c.chara < 0 || c.chara >= static_cast<int>(g_tables.charas.size())) return card;
    const std::string& code = g_tables.charas[c.chara];
    const bool eleven = IsEleven(c.chara);
    card.chara = code;
    card.charaName = g_tables.labels[c.chara];
    card.place = rooms.state == RoomsView::State::Room ? RoomsPlace(rooms, rooms.self) : "Custom Rooms";
    if (card.place == "Idle") card.place = "In the room"; // no table or queue
    const int slot = std::max(0, CostumeSlot(c.chara, c.costume));
    if (eleven) {
        for (const char* label : {"Costume", "Color", "V-Skill", "V-Trigger"}) card.rows.push_back({label, "Random"});
    } else {
        card.rows.push_back({"Costume", g_tables.costumeNames[c.chara][slot]});
        SelectCard::Row color{"Color", ColorLabel(c.color)};
        const auto swatch = SwatchOf(c.chara, slot, c.color);
        color.swatch1 = swatch.first;
        color.swatch2 = swatch.second;
        card.rows.push_back(color);
        card.rows.push_back({"V-Skill", SkillText(code, false, c.vskill - 1)});
        card.rows.push_back({"V-Trigger", SkillText(code, true, c.vtrigger - 1)});
    }
    std::string stage = RandomStagesText();
    if (c.stage >= 2 && c.stage - 2 < static_cast<int>(g_tables.stages.size())) {
        const int i = c.stage - 2;
        const auto& labels = g_tables.settingLabels[i];
        stage = c.setting > 0 && c.setting <= static_cast<int>(labels.size()) ? labels[c.setting - 1] : g_tables.stageLabels[i];
    }
    card.rows.push_back({"Stage", stage});
    // Its loading: the menus' preload of the saved pick (Eleven: the fighter
    // it copies this time).
    const NetplayOptions& o = NetOptions();
    if (g_my.fromProfile || !o.hasPick) {
        card.status = card.charaName + ", from your Fighter Profile";
        return card;
    }
    const std::string loads = eleven ? "ELEVEN" : card.charaName;
    double seconds = 0;
    switch (PreloadStatus(UnpackCode(o.pick.character), o.pick.costume, &seconds)) {
        case 2:
            card.status = loads + " is ready";
            card.statusRight = "ready";
            card.progress = 1;
            card.done = true;
            break;
        case 1:
            // No progress to read from the asset queue: a bar on the time a
            // character takes (about 14 s on a test PC, CPU-bound), held short
            // of the end.
            card.status = "Loading " + loads + "...";
            card.statusRight = "loading";
            card.progress = static_cast<float>(std::min(0.9, seconds / 15.0));
            break;
        case 0:
            card.status = loads + " loads shortly";
            card.progress = 0;
            break;
        default: card.status = loads + " loads with the match"; break;
    }
    return card;
}

void ApplyMyCharacterInput(const SelectInput& in) {
    Choices& c = g_choices;
    if (in.cancel) return CloseMyCharacter(true, "kept");
    if (in.done) return CloseMyCharacter(false, "saved");
    if (in.openStage) {
        g_my.stagePage = true;
        g_my.stageBefore = c.stage;
        g_my.settingBefore = c.setting;
    }
    if (in.closeStage) {
        c.stage = g_my.stageBefore;
        c.setting = g_my.settingBefore;
        g_my.stagePage = false;
    }
    if (in.lockStage) g_my.stagePage = false;
    if (in.randomOffChanged) SetRandomOff(in.randomOff, "my character");
    if (in.unready) g_my.savedNow = false;
    if (g_my.savedNow) return;
    if (in.chara >= 0 && in.chara < static_cast<int>(g_tables.charas.size())) SwitchCharacter(c, in.chara);
    if (in.randomPick) {
        static std::mt19937 rng{std::random_device{}()};
        std::vector<int> fighters;
        for (int i = 0; i < static_cast<int>(g_tables.charas.size()); i++) {
            if (!IsEleven(i) && CharaOwned(i)) fighters.push_back(i);
        }
        if (!fighters.empty()) SwitchCharacter(c, fighters[rng() % fighters.size()]);
    }
    if (in.row == 6 && in.delta) {
        // Auto, 0, 1, ... 8, around (as Polarity settings).
        const int n = kMaxDelaySetting + 2;
        g_my.delaySetting = static_cast<int8_t>(((g_my.delaySetting + 1 + in.delta) % n + n) % n - 1);
    } else if (in.row >= 0 && in.delta) StepRow(c, in.row, in.delta);
    if (in.stage >= 0) {
        const int count = static_cast<int>(g_tables.stages.size());
        c.stage = in.stage < count ? 2 + in.stage : 1;
        c.setting = 0;
    }
    if (in.setting >= 0) c.setting = in.setting;
    PickValid(c);
    if (in.ready) {
        // Locked in: saved, and straight back to the room.
        SaveMyCharacter();
        return CloseMyCharacter(false, "saved");
    }
}

void DrawMyCharacter(const RoomsView& rooms, bool takeInput) {
    // A table calling this player (or a set on its way) needs the room
    // screen: what was last confirmed is played.
    bool called = false;
    for (const TableView& t : rooms.tableViews) {
        if (t.s.phase == keeper::TablePhase::Call && (t.s.seat[0] == rooms.self || t.s.seat[1] == rooms.self)) called = true;
    }
    const NetView::Phase phase = NetViewNow().phase;
    if (called || (phase != NetView::Phase::Idle && phase != NetView::Phase::Ended)) return CloseMyCharacter(true, "the table calls");
    NetView v;
    v.you = 0;
    v.host = true;
    v.seats[0].present = true;
    v.seats[0].ready = g_my.savedNow;
    v.stageLocked = !g_my.stagePage;
    SelectLive live = SelectLiveFor(v);
    live.sides[0].player = NetLocalPlayerName();
    live.sides[1] = SelectSide{};
    live.bothReady = false;
    live.loading = false;
    live.myCharacter = true;
    live.randomOff = g_randomOff;
    live.keepLabel = "Keep " + g_tables.labels[std::clamp(g_my.saved.chara, 0, static_cast<int>(g_tables.labels.size()) - 1)];
    SelectRow& delay = live.sides[0].rows[6];
    delay.value = g_my.delaySetting >= 0 ? FramesText(g_my.delaySetting) : "Auto";
    delay.hint = g_my.delaySetting >= 0 ? "Each match starts with this. Lower feels sharper but rolls back more."
                                        : "Each match suggests a delay from your ping to the opponent.";
    live.card = MyCharacterCard(rooms);
    const bool inRoom = rooms.state == RoomsView::State::Room;
    live.note = g_my.savedNow ? (inRoom ? "Saved. The room sees your pick next to your name." : "Saved. Custom Rooms play this pick.")
                : inRoom      ? "You keep your place in the room. If you're called now, your last saved pick plays."
                              : "What you play at Custom Rooms' tables.";
    g_my.alpha = std::min(1.0f, g_my.alpha + ImGui::GetIO().DeltaTime / 0.15f);
    SelectInput in;
    DrawSelectLive(SelectFonts{g_fontBig, g_fontHuge, g_fontBody, g_fontBold}, live, g_my.alpha, takeInput && ImGui::GetFrameCount() > g_my.openedFrame, &in);
    ApplyMyCharacterInput(in);
}

// The hub's live parts, and what its choices do.

bool g_roomsWhenReady = false; // Custom Rooms chosen before the lists were read: opens once they are

void DrawHubScreens(bool takeInput);
int g_hubScreen = -1; // what DrawHubScreens drew: 0 the hub, 1 the room browser, 2 a room, 3 settings, 4 My character

// The hub and the screens it leads to. Where one screen gives way to
// another (the hub, the room browser, a room, settings, My character) the
// new one fades in quickly over the stage, which stays.
void DrawHubFrame(bool takeInput) {
    constexpr double kFadeSeconds = 0.16;
    static int shown = -1;
    static double changedAt = -1;
    ui::g_backdropEnd = -1;
    g_hubScreen = -1;
    DrawHubScreens(takeInput);
    // What was drawn (DrawHubScreens says, from the state it drew).
    const double now = ImGui::GetTime();
    if (g_hubScreen != shown) {
        if (shown >= 0 && g_hubScreen >= 0) changedAt = now;
        shown = g_hubScreen;
    }
    const float alpha = changedAt < 0 ? 1.0f : static_cast<float>(std::clamp((now - changedAt) / kFadeSeconds, 0.0, 1.0));
    if (alpha < 1 && ui::g_backdropEnd >= 0) {
        const float eased = 1 - (1 - alpha) * (1 - alpha);
        ui::FadeFrom(ImGui::GetBackgroundDrawList(), ui::g_backdropEnd, eased);
    }
}

// What the tables play for this player (My character's pick, else the
// profile's favorite, as the room shows): its character, V-Skill and
// V-Trigger (from 1); Eleven's are random.
CardPick TablePick(const NetplayOptions& o, const std::string& favorite) {
    CardPick p;
    if (o.hasPick) {
        const bool eleven = IsElevenSetup(o.pick);
        p.chara = eleven ? "ELV" : UnpackCode(o.pick.character); // the roll stays hidden
        p.vskill = o.pick.vskill + 1;
        p.vtrigger = o.pick.vtrigger + 1;
        p.editable = !eleven;
        return p;
    }
    p.chara = favorite;
    p.editable = favorite != "ELV";
    std::lock_guard lock(g_favoritesMutex);
    for (const FavoriteChara& f : g_favorites.charas) {
        if (f.code != favorite) continue;
        p.vskill = std::clamp(f.vskill + 1, 1, 2);
        p.vtrigger = std::clamp(f.vtrigger + 1, 1, 2);
    }
    return p;
}

// The your-turn card's V-Skill or V-Trigger (from 1; 0 unchanged): into the
// pick the tables play (made from the profile's favorite when there was
// none), saved, and to the driver for the match about to start.
void SetTableSkills(int vskill, int vtrigger) {
    InitChoices();
    if (!g_choices.initialised) return;
    Choices& c = g_choices;
    const NetplayOptions now = NetOptions();
    if (now.hasPick) {
        if (IsElevenSetup(now.pick)) return; // random each time
        const std::string code = UnpackCode(now.pick.character);
        const auto it = std::find(g_tables.charas.begin(), g_tables.charas.end(), code);
        if (it == g_tables.charas.end()) return;
        c.chara = static_cast<int>(it - g_tables.charas.begin());
        c.costume = now.pick.costume;
        c.color = now.pick.color;
        c.vskill = now.pick.vskill + 1;
        c.vtrigger = now.pick.vtrigger + 1;
        c.variant = now.pick.variation;
        c.comment = now.pick.comment;
    } else {
        ApplyProfileFavorite(c);
    }
    if (vskill > 0) c.vskill = vskill;
    if (vtrigger > 0) c.vtrigger = vtrigger;
    c.pick = true;
    PickValid(c);
    const NetplayOptions o = ChosenOptions();
    const NetView::Phase phase = NetViewNow().phase;
    const bool idle = phase == NetView::Phase::Idle || phase == NetView::Phase::Ended;
    Act([o, idle] {
        if (idle) NetSetPick(o.hasPick, o.pick);
        else {
            NetOptions().hasPick = o.hasPick;
            NetOptions().pick = o.pick;
        }
        if (!g_panelPath.empty()) SaveNetplayChoices(g_panelPath, NetOptions(), NetOptions().stageCode);
    });
    Log("overlay: the card's pick: %s V-Skill %d V-Trigger %d", g_tables.charas[c.chara].c_str(), c.vskill, c.vtrigger);
}

// The hub's connection to the rooms server, under the player's name as it
// is now (the server knows a member by the name it connected with).
// g_nameSeen: the name when last looked at; g_nameChanged: changed since
// the connection opened (the console's `rooms open` uses a test name of
// its own, which is left alone).
std::string g_nameSeen;
bool g_nameChanged = false;
// The room creation form's table rules (Customize Tables), for the room it
// asked for: sent once the new room's screen is up (QueueTableRules).
std::vector<std::string> g_createRules;

void OpenHubRooms(const NetplayOptions& o) {
    g_nameSeen = NetLocalPlayerName();
    g_nameChanged = false;
    RoomsOpen(o.server, g_nameSeen, o.server == kDefaultServer ? kDefaultServerAlsoPort : o.serverAlsoPort, o.keeperKey);
}

// Leaving a watched replay: the Battle Log again once the menus are back.
std::atomic<bool> g_reopenLog{false};

// Watching a replay from the Battle Log: the screen stays (no input) while
// the replay preloads and jumps, as a match's select screen does, until the
// jump switches to the battle scene; then black (kLaunchToBlack, held
// kLaunchBlackHeld while the game's own screen goes black) and it closes.
// Gone at once, the main menu showed for the seconds of the preload.
struct LogLaunch {
    bool active = false;
    uint64_t switchesAt = 0;
    double since = 0, blackFrom = -1;
};

LogLaunch g_logLaunch;
constexpr double kLaunchToBlack = 0.3, kLaunchBlackHeld = 0.6, kLaunchGiveUp = 45;

std::string StageLabelOf(uint16_t stage) {
    const std::string code = StageCode(stage);
    for (size_t i = 0; i < g_tables.stages.size(); i++) {
        if (g_tables.stages[i] == code) return i < g_tables.stageLabels.size() ? g_tables.stageLabels[i] : "";
        for (size_t k = 0; i < g_tables.settings.size() && k < g_tables.settings[i].size(); k++) {
            if (g_tables.settings[i][k] == code && i < g_tables.settingLabels.size() && k < g_tables.settingLabels[i].size()) {
                return g_tables.settingLabels[i][k];
            }
        }
    }
    return "";
}

void DrawHubScreens(bool takeInput) {
    NetplayOptions& o = NetOptions();
    if (g_roomsWhenReady && g_tables.loaded) {
        g_roomsWhenReady = false;
        RoomsClearNotice();
        OpenRoomsScreen();
    }
    if (!RoomsOpened()) {
        OpenHubRooms(o);
    }
    // A new name in Polarity settings: connected again under it at once, or,
    // in a room (or on the way into one), once the player is out of it, so
    // the next room has it.
    if (const std::string name = NetLocalPlayerName(); name != g_nameSeen) {
        g_nameSeen = name;
        g_nameChanged = RoomsOpened();
    }
    if (g_nameChanged && RoomsOpened()) {
        using S = RoomsView::State;
        const S now = RoomsSnapshot().state;
        if (now != S::Room && now != S::Creating && now != S::Joining) {
            Log("hub: the name is now %s: the rooms server again under it", NetLocalPlayerName().c_str());
            OpenHubRooms(o);
        }
    }
    const RoomsView rooms = RoomsSnapshot();
    g_hubScreen = SettingsScreenOpen()                    ? 3
                  : g_myCharacterOpen                     ? 4
                  : rooms.state == RoomsView::State::Room ? 2
                  : RoomsScreenOpen()                     ? 1
                                                          : 0;
    // A lost or unreachable server (a restart or an update on the VPS, the
    // network gone a moment): tried again every 10 s while the hub is up, as
    // a fresh start would. The hub is closed during matches, so a match that
    // goes on over its link is left alone.
    static uint64_t failedSince = 0;
    if (rooms.state == RoomsView::State::Failed) {
        const uint64_t now = GetTickCount64();
        if (!failedSince) failedSince = now;
        else if (now - failedSince >= 10000) {
            failedSince = 0;
            Log("hub: the rooms server again (%s)", o.server.c_str());
            OpenHubRooms(o);
        }
    } else failedSince = 0;
    // The browser with its server gone can do nothing: back to the hub,
    // which says why (and opens the rooms again once they answer).
    if (RoomsScreenOpen() && !SettingsScreenOpen() && rooms.state == RoomsView::State::Failed) {
        Log("hub: the rooms server is gone: the rooms screen gives way to the hub's modal");
        CloseRoomsScreen();
        ShowRoomsProblem();
    }
    // An update out: only the latest client plays in Custom Rooms. Out of
    // the browser to the hub's modal (a room's players stay: a toast tells
    // them, and leaving the room brings them here).
    const UpdateView update = UpdateViewNow();
    if (RoomsScreenOpen() && !SettingsScreenOpen() && UpdatePending(update) && rooms.state != RoomsView::State::Room &&
        rooms.state != RoomsView::State::Creating && rooms.state != RoomsView::State::Joining) {
        Log("hub: Polarity %s is out: the rooms screen gives way to the hub's update modal", update.latest.c_str());
        CloseRoomsScreen();
        OpenUpdateModal();
    }
    // Found while on the hub: offered at once, once a version (however it
    // was offered: Later holds).
    static std::string offered;
    if (UpdateModalOpen()) offered = update.latest;
    if (HubOpen() && !RoomsScreenOpen() && !SettingsScreenOpen() && update.state == UpdateView::State::Available &&
        update.latest != offered && !UpdateModalOpen()) {
        offered = update.latest;
        OpenUpdateModal();
    }
    HubLive live;
    live.player = NetLocalPlayerName();
    live.server = o.server == kDefaultServer ? "Public server" : o.server;
    using S = RoomsView::State;
    live.serverState = rooms.state == S::Off ? 0 : rooms.state == S::Connecting ? 1
                                               : rooms.state == S::Failed       ? 3
                                                                                : 2;
    live.version = g_build;
    // The tile's strip: live state only (what it is for is the hint's).
    if (rooms.state == S::Failed) live.roomsState = "Can't connect";
    else if (!rooms.otherBuild.empty() && !rooms.keeper) live.roomsState = "Update needed";
    else if (rooms.state == S::NoLobby || ((rooms.state == S::Lobby || rooms.state == S::Room) && !rooms.keeper)) live.roomsState = "Unavailable right now";
    else if (rooms.keeper && (rooms.state == S::Browsing || rooms.state == S::Lobby || rooms.state == S::Room)) {
        const int n = rooms.summary.publicRooms, p = rooms.summary.players;
        live.roomsState = std::to_string(n) + (n == 1 ? " public room" : " public rooms") + " \xC2\xB7 " + std::to_string(p) + (p == 1 ? " player" : " players");
    }
    if (!g_tables.loaded) live.roomsState = "Getting ready...";
    // What would leave the Custom Rooms screen with nothing to do.
    using RP = HubLive::RoomsProblem;
    live.roomsServer = o.server;
    live.update = update;
    live.restartError = g_restartError;
    if (UpdatePending(update)) live.roomsProblem = RP::UpdatePending;
    else if (update.state == UpdateView::State::Checking) live.roomsProblem = RP::CheckingUpdate;
    else if (rooms.state == S::Failed) live.roomsProblem = RP::Unreachable;
    else if (!rooms.otherBuild.empty() && !rooms.keeper) live.roomsProblem = RP::UpdateNeeded, live.roomsBuild = rooms.otherBuild;
    else if (rooms.state == S::Off || rooms.state == S::Connecting || rooms.state == S::Finding) live.roomsProblem = RP::Connecting;
    else if (rooms.state == S::NoLobby || !rooms.keeper) live.roomsProblem = RP::Offline;
    live.roomsInfo = "Browse, create or join with a code";
    live.logInfo = BattleLogInfo();
    {
        std::lock_guard lock(g_favoritesMutex);
        live.offlineChara = g_favorites.chara.empty() ? "RYU" : g_favorites.chara;
        live.offlineStage = g_favorites.stage;
    }
    // In the rooms: the pick played at the tables (My character), else the
    // profile's favorite.
    // Eleven shows as Eleven: the fighter it rolled stays hidden from
    // everyone (as in the game) until the match loads.
    RoomsSetCharacter(!o.hasPick ? live.offlineChara : IsElevenSetup(o.pick) ? std::string("ELV")
                                                                             : UnpackCode(o.pick.character));
    // Its V-Skill and V-Trigger, for the others' your-turn cards.
    const CardPick tablePick = TablePick(o, live.offlineChara);
    if (tablePick.editable) RoomsSetSkills(tablePick.vskill, tablePick.vtrigger);
    else RoomsSetSkills(0, 0);
    SetCardPick(tablePick);
    // My Character's tile: the pick the tables play, over its preferred
    // stage (else the profile's), with its V-Skill and V-Trigger.
    live.myChara = tablePick.chara.empty() ? live.offlineChara : tablePick.chara;
    live.myStage = live.offlineStage;
    for (size_t i = 0; i < g_tables.stages.size(); i++) {
        if (g_tables.stages[i] == o.stageCode) live.myStage = o.stageCode;
    }
    live.mySubtitle = live.myChara == "ELV" ? std::string("Eleven")
                                            : CharaLabel(live.myChara) + " VS" + std::to_string(tablePick.vskill) + " / VT" + std::to_string(tablePick.vtrigger);
    live.roomsChara = "CNL";
    live.roomsStage = "CHN";
    if (live.offlineStage.empty() && !g_tables.stages.empty()) live.offlineStage = g_tables.stages.front();
    static bool warmed = false;
    if (!warmed) {
        warmed = true;
        WarmHubTextures(live);
    }
    const HubFonts fonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold};
    if (BattleLogOpen()) {
        BattleLogLive bl;
        bl.myName = NetLocalPlayerName();
        bl.buildId = NetGameBuildId();
        bl.charaName = [](uint32_t c) { return CharaLabel(UnpackCode(c)); };
        bl.stageName = [](uint16_t s) { return StageLabelOf(s); };
        bl.launching = g_logLaunch.active;
        BattleLogInput in;
        DrawBattleLog(fonts, bl, takeInput && !g_logLaunch.active, &in);
        if (in.back) CloseBattleLog();
        if (!in.watch.empty() && !g_logLaunch.active) {
            Log("battle log: watching %s", in.watch.c_str());
            g_logLaunch = {true, BattleSceneSwitches(), ImGui::GetTime(), -1};
            RequestReplayView(in.watch, in.watchTitle, [] { g_reopenLog = true; });
            if (!CurrentFrameDriver()) {
                // Not started (the file unreadable, another driver): the log stays.
                g_logLaunch = {};
                AddToast("This replay can't be played.", kRed, 4);
            }
        }
        if (g_logLaunch.active) {
            // Black over it once the battle scene is switched to (LogLaunch).
            const double now = ImGui::GetTime();
            if (g_logLaunch.blackFrom < 0 && BattleSceneSwitches() != g_logLaunch.switchesAt) g_logLaunch.blackFrom = now;
            const double black = g_logLaunch.blackFrom < 0 ? 0 : std::min(1.0, (now - g_logLaunch.blackFrom) / kLaunchToBlack);
            if (black > 0) {
                ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize,
                                                              IM_COL32(0, 0, 0, static_cast<int>(black * 255)));
            }
            const bool done = g_logLaunch.blackFrom >= 0 && now - g_logLaunch.blackFrom >= kLaunchToBlack + kLaunchBlackHeld;
            if (done || now - g_logLaunch.since > kLaunchGiveUp) {
                if (!done) Log("battle log: no battle scene %.0f s after Watch: closing", kLaunchGiveUp);
                g_logLaunch = {};
                CloseBattleLog();
                CloseHub();
            }
        }
        return;
    }
    if (SettingsScreenOpen()) {
        SettingsInput in;
        const bool modal = g_benchModal != BenchModal::None;
        DrawSettingsScreen(fonts, BuildVersion(), SettingsRows(), takeInput && !modal, &in);
        if (!in.row.empty()) ApplySetting(in);
        if (in.back && !modal) CloseSettingsScreen();
        if (modal) DrawBenchmarkModal(fonts, takeInput);
        return;
    }
    // In a room: its screen (leaving comes back to the custom rooms screen).
    static std::string roomShown;
    if (rooms.state == S::Room) {
        if (roomShown != rooms.room) {
            roomShown = rooms.room;
            ResetRoomScreen();
            OpenRoomsScreen();
            // A room this player just made: the rules Customize Tables set.
            if (!g_createRules.empty() && rooms.owner == rooms.self) QueueTableRules(g_createRules);
            g_createRules.clear();
        }
        // The card's starting delay: the setting, or with Auto the
        // suggestion for the ping to the opponent at this player's table.
        unsigned opponentPing = 0;
        for (const TableView& t : rooms.tableViews) {
            for (int i = 0; i < 2; i++) {
                if (t.s.seat[i] != rooms.self || !t.s.seat[1 - i]) continue;
                for (const RoomMember& m : rooms.members) {
                    if (m.id == t.s.seat[1 - i] && m.pingMs > 0) opponentPing = static_cast<unsigned>(m.pingMs);
                }
            }
        }
        SetCardDelay(NetPreferredDelay(opponentPing));
        static bool namesSet = false;
        if (!namesSet && g_tables.loaded) {
            namesSet = true;
            std::vector<std::pair<std::string, std::string>> names;
            for (size_t i = 0; i < g_tables.charas.size() && i < g_tables.labels.size(); i++) names.push_back({g_tables.charas[i], g_tables.labels[i]});
            SetCharaNames(std::move(names));
            std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> skills;
            for (size_t i = 0; i < g_tables.charas.size() && i < g_tables.vskills.size() && i < g_tables.vtriggers.size(); i++) {
                skills[g_tables.charas[i]] = {g_tables.vskills[i], g_tables.vtriggers[i]};
            }
            SetSkillNames(std::move(skills));
        }
        RoomScreenInput in;
        {
            // The table's session, for the connecting modal.
            const NetView nv = NetViewNow();
            using P = NetView::Phase;
            TableHandoff h;
            h.session = nv.setPlay && nv.phase != P::Idle && nv.phase != P::Ended;
            h.roomOpen = h.session && nv.phase != P::Connecting;
            h.linked = h.session && nv.seats[0].present && nv.seats[1].present && nv.phase != P::Hosting && nv.phase != P::Joining &&
                       nv.phase != P::Connecting;
            h.pingMs = nv.link.samples ? nv.link.pingMs : nv.linkPingMs;
            h.path = nv.linkPath == NetView::Path::Direct ? 1 : nv.linkPath == NetView::Path::Relayed ? 2
                                                                                                      : 0;
            h.preloaded[0] = h.linked && nv.seats[0].preloaded;
            h.preloaded[1] = h.linked && nv.seats[1].preloaded;
            h.jumping = h.session && (nv.phase == P::Jumping || nv.phase == P::Loading || nv.phase == P::Running);
            h.played = h.session ? nv.played : 0;
            SetTableHandoff(h);
        }
        if (!g_myCharacterOpen) DrawRoomScreen(fonts, rooms, takeInput, &in);
        if (in.cancelHandoff) {
            RoomsCancelHandoff();
            Log("overlay: the connecting modal's Cancel");
        }
        if (!in.request.empty()) RoomsTable(in.request, in.args);
        if (in.delay >= 0) g_tableDelay = in.delay;
        if (in.vskill > 0 || in.vtrigger > 0) SetTableSkills(in.vskill, in.vtrigger);
        if (in.myCharacter && !g_myCharacterOpen) OpenMyCharacter();
        if (in.training) StartRoomTraining(rooms);
        if (in.watch != -2) {
            // Not while a watched match jumps, loads or plays: the screen has
            // the input until the battle takes it, and an X pressed in the
            // jump (light punch, in the game) stopped watching, which ended
            // the session mid-jump (crash reports 2026-11-07..10).
            const NetView nv = NetViewNow();
            using P = NetView::Phase;
            if (nv.spectator && (nv.phase == P::Jumping || nv.phase == P::Loading || nv.phase == P::Running)) {
                Log("overlay: the room screen's watch toggle ignored while the watched match is on");
            } else {
                RoomsWatch(in.watch);
                if (in.watch < 0) g_watchTable = -1;
            }
        }
        if (in.clearNotice) RoomsClearNotice();
        else if (g_myCharacterOpen) DrawMyCharacter(rooms, takeInput);
        if (!in.chat.empty() && !RoomsChat(in.chat)) {
            Log("rooms: a chat line held back (one a second)");
            AddToast("Slow down. One message per second.", ImVec4(1, 0.8f, 0.4f, 1), 2);
        }
        if (in.copiedCode) AddToast("Room code copied", ImVec4(1, 1, 1, 1), 2);
        if (in.leave) RoomsLeave();
        return;
    }
    roomShown.clear();
    // My Character from the hub (no room): its screen until it closes.
    if (g_myCharacterOpen) {
        DrawMyCharacter(rooms, takeInput);
        return;
    }
    if (!RoomsScreenOpen()) RoomsPeek("");
    if (RoomsScreenOpen()) {
        RoomsScreenInput in;
        DrawRoomsScreen(fonts, rooms, takeInput, &in);
        RoomsPeek(in.focusedRoom);
        if (in.clearNotice) RoomsClearNotice();
        if (in.settings_) OpenSettingsScreen(rooms.otherBuild.empty() ? "server" : "check");
        if (in.create) {
            g_createRules = in.tableRules;
            RoomsCreate(in.settings);
        }
        if (!in.join.empty()) {
            g_createRules.clear();
            RoomsJoin(in.join);
        }
        if (in.back) CloseRoomsScreen();
        return;
    }
    switch (DrawHub(fonts, live, takeInput)) {
        case HubAction::PlayOffline:
            CloseHub();
            RoomsClose();
            break;
        case HubAction::CustomRooms:
            // Only the latest client plays there: a check first unless one ended
            // in the last 5 minutes (the modal waits for it, then opens them).
            if (UpdateCheckIfStale(5 * 60 * 1000)) {
                ShowRoomsProblem();
                break;
            }
            // Before the game's lists are read (a few seconds after launch) it
            // opens once they are.
            if (!g_tables.loaded) {
                g_roomsWhenReady = true;
                break;
            }
            RoomsClearNotice();
            OpenRoomsScreen();
            break;
        case HubAction::Settings:
            OpenSettingsScreen();
            break;
        case HubAction::BattleLog:
            OpenBattleLog();
            break;
        case HubAction::MyCharacter:
            // Before the game's lists are read (a few seconds after launch): not yet.
            if (g_tables.loaded) OpenMyCharacter();
            break;
        case HubAction::UpdateInstall:
            g_restartError.clear();
            UpdateInstall(); // nothing if it is downloading or installed already
            break;
        case HubAction::UpdateRestart: {
            std::string error;
            if (!RestartGame(g_window, &error)) g_restartError = error;
            break;
        }
        case HubAction::UpdateRetry:
            UpdateCheck();
            break;
        case HubAction::RetryRooms:
            Log("hub: the rooms server again, asked for (%s)", o.server.c_str());
            OpenHubRooms(o);
            break;
        case HubAction::None:
            break;
    }
}

// On the game's menus, with the panel closed and no session: a hint at the
// top center of how to open online play.
bool OnMenuScreen() {
    const std::string scene = CurrentScene();
    return !scene.empty() && scene != "BootState" && scene != "InitialState" && scene.find("Battle") == std::string::npos;
}

void DrawMenuHint() {
    DrawMenuNote(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold});
}

// The Debug tab's previews, each window on its own.
void DrawPreviews() {
    std::string words;
    {
        std::lock_guard lock(g_previewMutex);
        words.swap(g_previewWords);
    }
    std::istringstream in(words);
    for (std::string w; in >> w;) {
        Preview& p = g_preview;
        if (w == "hud") p.hud = true;
        else if (w == "strip") p.strip = true;
        else if (w == "desync") p.desync = true;
        else if (w == "p1") p.side[0] = true;
        else if (w == "p2") p.side[1] = true;
        else if (w == "select") p.sides = 1;
        else if (w == "you1") p.you = 0;
        else if (w == "you2") p.you = 1;
        else if (w == "toasts") g_sampleToasts = true;
        else if (w == "relay") p.relayed = true;
        else if (w == "set") p.set = true;
        else if (w == "vote") p.set = false, p.sides = 0;
        else if (w == "p2p") p.relayed = false;
        else if (w == "fade") {
            p.hud = true;
            p.fadeAt = GetTickCount64();
        } else if (w == "off") p.hud = p.strip = p.desync = p.side[0] = p.side[1] = false;
    }
    if (g_sampleToasts) {
        g_sampleToasts = false;
        AddToast("Player Two joined");
        AddToast("Player Two wants a rematch");
        AddToast("A spectator joined (1 watching)", kDim);
        AddToast("Session over: the other player left", kRed, 6);
        AddToast("Polarity sends desync and crash reports to help fix bugs. You can turn this off in Polarity settings.", kGold, 15);
    }
    const Preview& p = g_preview;
    if (!p.Any()) return;
    LoadTables();
    InitChoices();
    if (!g_tables.loaded) return;
    g_previewing = true;
    if (p.hud) {
        // "Fade out" plays the match's end for a moment, then they come back.
        const bool over = p.fadeAt && GetTickCount64() - p.fadeAt < kBarsFadeMs + 1200;
        DrawPlayerBars(PreviewView(over ? NetView::Phase::PostMatch : NetView::Phase::Running));
    }
    if (p.strip) DrawStrip(PreviewView(NetView::Phase::Running));
    if (p.side[0] || p.side[1]) DrawSides(PreviewView(p.sides == 0 ? NetView::Phase::PostMatch : NetView::Phase::Lobby), p.side);
    g_previewing = false;
}

void Draw(IDXGISwapChain* swapChain) {
    if (swapChain != g_swapChain && !SetUp(swapChain)) return;
    std::vector<Message> input;
    {
        std::lock_guard lock(g_inputMutex);
        input.swap(g_input);
    }
    // The keyboard: a key the game assigns is its pad button (FeedPads), not a
    // key, unless a text field took the keyboard last frame.
    LoadKeyboardMap();
    ui::BeginInputFrame();
    const bool textEntry = ui::g_textEntry;
    ui::g_textEntry = false;
    if (textEntry) g_keyPad = 0;
    for (const Message& m : input) {
        const bool down = m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN, up = m.msg == WM_KEYUP || m.msg == WM_SYSKEYUP;
        if (m.msg == WM_KILLFOCUS) g_keyPad = 0;
        if (!textEntry && (down || up)) {
            if (const uint32_t bits = ui::MappedPadKeys(static_cast<int>(m.wp))) {
                if (down) g_keyPad |= bits;
                else g_keyPad &= ~bits;
                continue;
            }
        }
        if (!textEntry && m.msg == WM_CHAR) {
            const SHORT scan = VkKeyScanW(static_cast<WCHAR>(m.wp));
            if (scan != -1 && ui::MappedPadKeys(scan & 0xFF)) continue; // the letter of a mapped key: not typed
        }
        ImGui_ImplWin32_WndProcHandler(g_window, m.msg, m.wp, m.lp);
    }
    ImGuiIO& io = ImGui::GetIO();
    FeedPads(io);
    // A frame every time, also with nothing to show: it consumes the input
    // events queued above, which would otherwise pile up.
    const NetView view = NetViewNow();
    {
        const int other = view.you == 0 ? 1 : 0;
        const bool player = !view.spectator && view.you >= 0 && view.phase != NetView::Phase::Idle && view.phase != NetView::Phase::Ended;
        g_otherHasPick = player && view.seats[other].present && view.seats[other].hasPick;
        g_otherPick = view.seats[other].pick;
    }
    const NetView::Phase phase = view.phase;
    g_inMatch = phase == NetView::Phase::Running;
    // A session's windows pick from the game's lists, also when the panel
    // was never opened. They are read once the game is at a menu, so that the
    // select screen is there as soon as a session starts (the read takes a
    // while: every character's costume names, colors, comments).
    if (g_reloadChoices.exchange(false)) g_choices.initialised = false;
    if (phase != NetView::Phase::Idle) {
        LoadTables();
        InitChoices();
    } else if (OnMenuScreen()) {
        LoadTables();
    }
    // On the way to a session (the panel open at the menus, hosting,
    // joining): the select screen's textures start loading, so nothing pops
    // in when it opens. About 170, 40 ms each on the game thread: a joiner
    // types the code meanwhile. Once a run: they stay loaded.
    // At the menus once the lists are read: the title's warm up (M1), the
    // pictures any screen shows first, the player's favorites among them.
    static bool bootWarmed = false;
    if (!bootWarmed && g_tables.loaded && OnMenuScreen()) {
        bootWarmed = true;
        RunOnGameThread([data = PreviewSelectData()] {
            ProfileFavorites favorites;
            ReadProfileFavorites(&favorites);
            WarmBootTextures(data, favorites.chara, favorites.stage);
            std::lock_guard lock(g_favoritesMutex);
            g_favorites = favorites;
        });
    }
    // No Random stages list yet: the Fighter Profile's random stage setting,
    // once (read only; the game's save is never written).
    static bool randomSeeded = false;
    if (!randomSeeded && g_tables.loaded) {
        bool read = false;
        std::vector<std::string> gameList;
        {
            std::lock_guard lock(g_favoritesMutex);
            read = g_favorites.loaded;
            if (read) gameList = g_favorites.randomOff;
        }
        if (g_randomOffSaved || NetOptions().randomOffSaved) randomSeeded = true;
        else if (read) {
            randomSeeded = true;
            SetRandomOff(gameList, "from the Fighter Profile");
        }
    }
    static bool warmed = false;
    const bool onTheWay = phase != NetView::Phase::Idle && phase != NetView::Phase::Ended;
    if (!warmed && onTheWay && g_tables.loaded) {
        warmed = true;
        SetSelectData(PreviewSelectData());
        WarmSelectTextures();
    }
    // Reaching a session's lobby sets this player's input delay to their
    // setting (Polarity settings), with Auto the suggestion for the ping between
    // them (once a session; it can be changed from there). A session a
    // command started (tests) keeps the delay its commands set, and a
    // table's set the one chosen on its your-turn card: from its lobby
    // between games, Auto put the first opponent's suggestion back in.
    static bool delaySet = false;
    if (phase == NetView::Phase::Idle || phase == NetView::Phase::Ended) {
        delaySet = false;
    } else if (!delaySet && phase == NetView::Phase::Lobby && NetPreferredDelay(view) >= 0 && g_choices.initialised &&
               !view.spectator && !view.scripted && !view.setPlay) {
        delaySet = true;
        const int delay = NetPreferredDelay(view);
        g_choices.delay = delay;
        Act([delay] { NetSetDelay(static_cast<uint8_t>(delay)); });
        if (NetOptions().delaySetting >= 0) Log("overlay: input delay %d, the player's setting", delay);
        else {
            Log("overlay: input delay %d, suggested for a %u ms ping, %u ms jitter (%zu heartbeats)", delay, view.link.pingMs,
                view.link.jitterMs, view.link.samples);
        }
    }
    // After a match, over its results screen (the vote, then character
    // select's lobby): both sides' windows; they take the pads while this
    // player chooses. Also while the panel covers them: else the press that
    // closes the panel reached the game for a frame, and a press on its
    // results screen can open its own Results Menu.
    const bool overResults = phase == NetView::Phase::PostMatch || (phase == NetView::Phase::Lobby && view.overResults);
    // The vote's windows; the lobby after it is the select screen.
    const bool sides = phase == NetView::Phase::PostMatch && !g_visible;
    g_voting = overResults && !view.spectator;
    g_watchHold = overResults && view.spectator;
    // In a match: the leave modal (Back + Start); anywhere else it goes.
    const bool leaveUp = g_visible && (phase == NetView::Phase::Running || phase == NetView::Phase::Loading || phase == NetView::Phase::Jumping);
    if (g_visible && !leaveUp) SetOverlayVisible(false);
    const bool visible = g_visible;
    const bool session = phase != NetView::Phase::Idle;
    // Our own cursor where the game hides the system's, and only there: not
    // when Windows shows its own (a second arrow trailing it), nor with the
    // real cursor off the game's window (it stayed where it left). The
    // messages know no leave (OverlayWndProc), so the real cursor decides.
    {
        POINT pt{};
        RECT client{};
        CURSORINFO cursor{sizeof(cursor)};
        const bool known = GetCursorPos(&pt) && GetClientRect(g_window, &client);
        POINT local = pt;
        const bool inside = known && WindowFromPoint(pt) == g_window && ScreenToClient(g_window, &local) && PtInRect(&client, local);
        const bool systemShown = GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING);
        const bool wanted = visible || g_voting || g_selectLive || SelectPreviewOpen() || HubOpen();
        io.MouseDrawCursor = wanted && inside && !systemShown;
        if (!inside) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // nothing hovered while it is away
    }
    // Fonts for the game's resolution (1.0 at 720 lines): again when it
    // changed (last frame's size; the backend makes the new atlas). Glyphs a
    // name needs are made as it is drawn (ImGui 1.92).
    if (io.DisplaySize.y > 0 && std::abs(UiScaleFor(io.DisplaySize.y) - g_builtScale) > 0.05f) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        BuildFonts(UiScaleFor(io.DisplaySize.y));
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ToastChanges(view);
    // Not over a match: it waits for a menu.
    if (OnMenuScreen()) ToastUpdate();
    ToastMigrated();
    ToastReports();
    // The select screen preview covers everything.
    if (const int wanted = g_selectRequest.load(); wanted >= 0 && g_tables.loaded) {
        g_selectRequest = -1;
        SelectData data = PreviewSelectData();
        data.spectator = wanted >= 4;
        OpenSelectPreview(std::move(data), wanted % 4);
        SetOverlayVisible(false);
    }
    const bool preview = SelectPreviewOpen();
    // A session's lobby is the select screen: it fades in (over the last
    // match's results screen after Character select), the panel closes as it
    // comes (and stays shut: B on the screen leaves), and it fades out as the
    // match loads.
    static float liveAlpha = 0;
    static bool lobbyWas = false;
    // A table's set (M4: setPlay) starts without a select screen: the picks
    // are each player's My character, both ready at once; the room screen
    // stays up (its connecting modal) until the jump. Character select within
    // the set (the vote after a game) is the select screen, over the results.
    const bool setSelect = view.setPlay && phase == NetView::Phase::Lobby && view.overResults;
    const bool tableConnecting = view.setPlay && !setSelect &&
                                 (phase == NetView::Phase::Connecting || phase == NetView::Phase::Hosting ||
                                  phase == NetView::Phase::Joining || phase == NetView::Phase::Lobby ||
                                  phase == NetView::Phase::Jumping);
    const bool lobby = phase == NetView::Phase::Lobby && g_tables.loaded && g_choices.initialised && !preview && (!view.setPlay || setSelect);
    if (lobby && !lobbyWas) {
        SetSelectData(PreviewSelectData());
        ResetSelectScreen();
        g_sentSelect = {};
        SetOverlayVisible(false);
        g_toasts.clear(); // the vote's ("Host wants character select"): the screen says it
    }
    lobbyWas = lobby;
    // Into a match, it stays until the jump switches to the battle scene
    // (gone at the jump, it showed the menus for the seconds the stage
    // loads), then fades to black as the game's screen does for the battle's
    // loading, and goes once the game's is black too (about half a second
    // after the switch: its menus fade out just before it).
    constexpr double kToBlack = 0.3, kBlackHeld = 0.6;
    static bool holding = false;
    static uint64_t switchesAtJump = 0;
    static double blackFrom = -1;
    const double now = ImGui::GetTime();
    if (lobby) {
        holding = true;
        switchesAtJump = BattleSceneSwitches();
        blackFrom = -1;
    } else if (holding && phase == NetView::Phase::Jumping && BattleSceneSwitches() != switchesAtJump) {
        holding = false;
        blackFrom = now;
    } else if (holding && phase != NetView::Phase::Jumping) {
        holding = false; // no switch seen (left, or a jump another way): it fades out
    }
    const bool toBlack = blackFrom >= 0 && now - blackFrom < kToBlack + kBlackHeld;
    const bool shown = lobby || holding || toBlack;
    liveAlpha = std::clamp(liveAlpha + (shown ? 1.0f : -1.0f) * io.DeltaTime / 0.4f, 0.0f, 1.0f);
    if (blackFrom >= 0 && !toBlack) {
        liveAlpha = 0; // the game's screen is black: gone at once
        blackFrom = -1;
    }
    if (preview) liveAlpha = 0;
    // Still drawn while the jump loads the battle, it keeps the input from the
    // game: the main menu behind took presses then (its cursor starts on
    // Arcade, and offline Arcade shows "You are not logged in to the server",
    // which stayed over the fight, 2026-11-01).
    g_selectLive = (lobby || holding) && !g_visible;
    if (liveAlpha > 0) {
        SelectInput in;
        DrawSelectLive(SelectFonts{g_fontBig, g_fontHuge, g_fontBody, g_fontBold}, SelectLiveFor(view), liveAlpha, lobby && !g_visible, &in);
        if (lobby) ApplySelectInput(in, view);
        if (toBlack) {
            const float black = static_cast<float>(std::min(1.0, (now - blackFrom) / kToBlack));
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(black * 255)));
        }
    }
    // The Polarity hub over the game's menus: after the title, and when Start is
    // held there. It goes when a session starts or a battle loads; Play
    // Offline closes it (and the hub's presence connection with it).
    const bool atMenus = OnMenuScreen() && (phase == NetView::Phase::Idle || phase == NetView::Phase::Ended || tableConnecting) && !preview;
    g_hubAllowed = atMenus;
    static bool hubBooted = false;
    if (!hubBooted && atMenus) {
        hubBooted = true;
        OpenHub(true);
    }
    if (HubOpen() && !atMenus && !g_logLaunch.active && !g_benchLaunch.active) { // (a launch closes it itself)
        CloseHub();
        CloseRoomsScreen();
        CloseBattleLog();
    }
    if (g_benchReturn && atMenus && !ReplayViewNow().active) {
        g_benchReturn = false;
        OpenHub();
        OpenSettingsScreen("bench");
        BenchmarkResult r;
        if (TakeBenchmarkResult(&r)) {
            g_benchShown = r;
            g_benchChoice = 0;
            g_benchModal = BenchModal::Result;
        }
    }
    if (g_reopenLog && atMenus && g_tables.loaded && !ReplayViewNow().active) {
        g_reopenLog = false;
        OpenHub();
        OpenBattleLog();
        RefreshBattleLog();
    }
    StepRoomTraining();
    // A table's ticket starts its set; after it, back at the menus, the
    // room screen comes back.
    for (keeper::TableTicket ticket; RoomsTakeTicket(&ticket);) StartTableSession(ticket);
    for (WatchTicket w; RoomsTakeWatch(&w);) StartTableWatch(w);
    if (RoomsTakeAbort()) Act([] { NetLeave(); });
    if (!HubOpen() && atMenus && g_tables.loaded && RoomsOpened()) {
        if (RoomsSnapshot().state == RoomsView::State::Room) {
            OpenHub();
            g_backToRooms = false;
        } else if (g_backToRooms) {
            OpenHub();
            OpenRoomsScreen();
            g_backToRooms = false;
        }
    }
    if (HubOpen()) DrawHubFrame(!g_visible);
    if (ReplayViewNow().active && !g_benchLaunch.active) DrawReplayStripe(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, !g_visible);
    if (g_benchLaunch.active) DrawBenchmarkLaunch(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold});
    if (preview) {
        DrawSelectPreview(SelectFonts{g_fontBig, g_fontHuge, g_fontBody, g_fontBold});
    } else if (session) {
        const bool inMatch = phase == NetView::Phase::Running || phase == NetView::Phase::PostMatch;
        if (inMatch) DrawPlayerBars(view);
        else if (!sides && liveAlpha <= 0 && !HubOpen()) DrawStrip(view);
        if (sides) DrawSides(view);
    }
    static bool leaveWas = false;
    if (leaveUp) {
        if (!leaveWas) ResetLeaveModal();
        bool leave = false, stay = false;
        DrawLeaveModal(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, view.setPlay, true, &leave, &stay);
        if (stay) SetOverlayVisible(false);
        if (leave) {
            SetOverlayVisible(false);
            Log("overlay: left the match (the leave modal)");
            Act([] { NetLeave(); });
        }
    }
    leaveWas = leaveUp;
    // Watching (M5): the pause panel, opened and closed by a Start tap.
    {
        static bool watchPanel = false;
        static float watchAlpha = 0;
        const bool watchingNow = session && view.spectator;
        g_spectating = watchingNow;
        if (g_watchPanelTap.exchange(false) && watchingNow) watchPanel = !watchPanel;
        if (!watchingNow) watchPanel = false;
        // A table's set watched from a room: the room's chat over it, under
        // the watch panel (watch_chat.h).
        if (watchingNow && g_watchTable >= 0) {
            const RoomsView rooms = RoomsSnapshot();
            if (rooms.state == RoomsView::State::Room) {
                WatchChatInput ci;
                DrawWatchChat(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, rooms, !watchPanel && !g_visible, &ci);
                if (!ci.send.empty() && !RoomsChat(ci.send)) {
                    Log("watch chat: a line held back (one a second)");
                    AddToast("Slow down. One message per second.", ImVec4(1, 0.8f, 0.4f, 1), 2);
                }
            }
        }
        watchAlpha = std::clamp(watchAlpha + (watchPanel ? 1.0f : -1.0f) * io.DeltaTime / 0.15f, 0.0f, 1.0f);
        if (watchAlpha > 0 && watchingNow) {
            WatchPanelLive wl;
            wl.table = view.setPlay ? g_watchTable : -1; // a Direct Match watched: no table
            for (size_t i = 0; i < view.matches.size(); i++) {
                const lobby::MatchEntry& e = view.matches[i];
                wl.games.push_back({ShortCharaName(UnpackCode(e.setup.players[0].character)), ShortCharaName(UnpackCode(e.setup.players[1].character)),
                                    e.winner, e.finished});
                if (e.id == view.watchingMatch) wl.at = static_cast<int>(i);
            }
            wl.behindSeconds = view.behind / 60.0f;
            wl.players[0] = view.seats[0].name;
            wl.players[1] = view.seats[1].name;
            if (wl.table >= 0) {
                // Their flags from the room (the session's seats have none).
                const RoomsView rooms = RoomsSnapshot();
                for (int i = 0; i < 2; i++) {
                    for (const RoomMember& m : rooms.members) {
                        if (m.name == wl.players[i]) wl.countries[i] = m.country;
                    }
                }
            }
            WatchPanelInput wi;
            DrawWatchPanel(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, wl, watchAlpha, watchPanel && !g_visible, &wi);
            if (wi.resume) watchPanel = false;
            if (wi.stop) {
                watchPanel = false;
                Log("overlay: stopped watching from the watch panel");
                RoomsWatch(-1);
                g_watchTable = -1;
                Act([] { NetLeave(); });
            }
            if (wi.watch >= 0 && wi.watch < static_cast<int>(view.matches.size())) {
                const uint64_t id = view.matches[wi.watch].id;
                Log("overlay: watch panel: game %d", wi.watch + 1);
                Act([id] { NetWatchMatch(id); });
            } else if (wi.watch == -1) {
                Log("overlay: watch panel: the current game");
                Act([] { NetWatchMatch(0); });
            }
        }
    }
    const bool hint = !visible && !preview && !HubOpen() && (phase == NetView::Phase::Idle || phase == NetView::Phase::Ended) && OnMenuScreen();
    if (hint) DrawMenuHint();
    DrawHitboxes(ImGui::GetBackgroundDrawList(), ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    DrawFrameMeter(ImGui::GetBackgroundDrawList(), g_fontBold);
    DrawPreviews();
    DrawNotices(session && (phase == NetView::Phase::Running || phase == NetView::Phase::PostMatch), view);
    DrawTexturePreview();
    bool texture = false;
    {
        std::lock_guard lock(g_textureMutex);
        texture = g_texture != nullptr;
    }
    // The menu sounds this frame's screens asked for (ui_sound.h).
    FlushUiSounds();
    const bool draw = visible || preview || liveAlpha > 0 || session || hint || HubOpen() || !g_toasts.empty() || g_preview.Any() || texture ||
                      ReplayViewNow().active || HitboxesVisible() || FrameMeterVisible();
    if (!draw || (!g_rtv && !CreateTarget(swapChain))) {
        ImGui::EndFrame();
        return;
    }
    ImGui::Render();

    // Draw on top of the finished frame, leaving the game's targets as they were.
    ID3D11RenderTargetView* saved = nullptr;
    ID3D11DepthStencilView* savedDepth = nullptr;
    g_context->OMGetRenderTargets(1, &saved, &savedDepth);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &saved, savedDepth);
    if (saved) saved->Release();
    if (savedDepth) savedDepth->Release();
}

// Draws the overlay into the back buffer, then presents with `next`.
void TakeScreenshot(IDXGISwapChain* swapChain); // below: the shot command

HRESULT DrawAndPresent(PresentFn next, IDXGISwapChain* self, UINT syncInterval, UINT flags) {
    if (Stopping() || (flags & DXGI_PRESENT_TEST)) return next(self, syncInterval, flags);
    SaveQueuedTextures();
    CreatePakTextures();
    // Frames per second: frames counted over each half second. (Averaging
    // 1/frame time read high whenever frame times were uneven, 61-62 at a
    // true 60: the mean of 1/t is above 1/mean t.)
    static LARGE_INTEGER last{}, freq{}, windowStart{};
    static int windowFrames = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    int64_t frameMicros = 0;
    if (last.QuadPart) {
        const double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
        frameMicros = static_cast<int64_t>(dt * 1e6);
    }
    last = now;
    if (!windowStart.QuadPart) windowStart = now;
    windowFrames++;
    const double window = double(now.QuadPart - windowStart.QuadPart) / double(freq.QuadPart);
    if (window >= 0.5) {
        // Frames presented since the window began, over its length.
        const float rate = static_cast<float>((windowFrames - 1) / window);
        g_fps = g_fps > 0 && window < 2 ? g_fps * 0.5f + rate * 0.5f : rate;
        windowStart = now;
        windowFrames = 1;
    }
    {
        hitch::Scope timed(hitch::Part::Overlay);
        Draw(self);
    }
    TakeScreenshot(self);
    HRESULT result;
    {
        hitch::Scope timed(hitch::Part::Present);
        const auto from = std::chrono::steady_clock::now();
        result = next(self, g_noVsync ? 0 : syncInterval, flags);
        g_presentMicros += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from).count();
        g_presentFrames++;
    }
    g_gameSyncInterval = syncInterval;
    g_gamePresentFlags = flags;
    // The frame-drop log (hitch.h), for the frame that ends here.
    if (frameMicros) hitch::FramePresented(frameMicros, g_inMatch ? "match" : HubOpen() ? "menus"
                                                                                        : nullptr);
    return result;
}

// ----- screenshots (the shot command) ------------------------------------------

std::mutex g_shotMutex;
std::string g_shotPath; // asked for, taken on the next Present
int g_shotScale = 1;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The back buffer as it is about to be presented, as RGB (1/scale size).
bool ReadBack(IDXGISwapChain* swapChain, int scale, int* outW, int* outH, std::vector<uint8_t>* outRgb) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
        Log("overlay: screenshot: no back buffer");
        return false;
    }
    ID3D11Device* device = nullptr;
    back->GetDevice(&device);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    const DXGI_FORMAT f = desc.Format;
    const bool bgr = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM ||
                     f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    const bool rgba = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool rgb10 = f == DXGI_FORMAT_R10G10B10A2_UNORM; // the game's own (10 bits a channel)
    ID3D11Texture2D* resolved = nullptr;
    ID3D11Texture2D* staging = nullptr;
    auto release = [&] {
        if (staging) staging->Release();
        if (resolved) resolved->Release();
        context->Release();
        device->Release();
        back->Release();
    };
    if (!bgr && !rgba && !rgb10) {
        Log("overlay: screenshot: back buffer format %d not handled", static_cast<int>(f));
        release();
        return false;
    }
    ID3D11Texture2D* source = back;
    if (desc.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC rd = desc;
        rd.SampleDesc = {1, 0};
        rd.BindFlags = 0;
        rd.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&rd, nullptr, &resolved))) {
            Log("overlay: screenshot: cannot resolve the back buffer");
            release();
            return false;
        }
        context->ResolveSubresource(resolved, 0, back, 0, f);
        source = resolved;
    }
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) {
        Log("overlay: screenshot: cannot make a staging texture");
        release();
        return false;
    }
    context->CopyResource(staging, source);
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        Log("overlay: screenshot: cannot read the back buffer");
        release();
        return false;
    }
    const int w = static_cast<int>(desc.Width) / scale, h = static_cast<int>(desc.Height) / scale;
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * scale * mapped.RowPitch;
        uint8_t* out = rgb.data() + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; x++) {
            const uint8_t* p = row + static_cast<size_t>(x) * scale * 4;
            if (rgb10) {
                uint32_t v;
                std::memcpy(&v, p, 4);
                out[x * 3 + 0] = static_cast<uint8_t>((v >> 2) & 0xFF);
                out[x * 3 + 1] = static_cast<uint8_t>((v >> 12) & 0xFF);
                out[x * 3 + 2] = static_cast<uint8_t>((v >> 22) & 0xFF);
                continue;
            }
            out[x * 3 + 0] = bgr ? p[2] : p[0];
            out[x * 3 + 1] = p[1];
            out[x * 3 + 2] = bgr ? p[0] : p[2];
        }
    }
    context->Unmap(staging, 0);
    release();
    *outW = w;
    *outH = h;
    *outRgb = std::move(rgb);
    return true;
}

// Writes a PNG aside and renames it into place (a reader never sees half a file).
bool WritePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb) {
    const std::vector<uint8_t> png = EncodePng(w, h, rgb.data());
    std::wstring full = Widen(path);
    const bool absolute = full.size() > 1 && (full[1] == L':' || full[0] == L'\\' || full[0] == L'/');
    if (!absolute) {
        std::wstring win64 = DataDir();
        const size_t cut = win64.find_last_of(L"\\/");
        if (cut != std::wstring::npos) win64.resize(cut);
        full = win64 + L"\\" + full;
    }
    for (auto& c : full) {
        if (c == L'/') c = L'\\';
    }
    const std::wstring tmp = full + L".tmp";
    FILE* file = _wfopen(tmp.c_str(), L"wb");
    if (!file) {
        Log("overlay: screenshot: cannot write %s", path.c_str());
        return false;
    }
    const bool written = fwrite(png.data(), 1, png.size(), file) == png.size();
    fclose(file);
    if (!written || !MoveFileExW(tmp.c_str(), full.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log("overlay: screenshot: cannot write %s", path.c_str());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// Research (`shotburst <dir> <frames> [scale]`): every presented frame for a
// while, kept in memory and written after (a shot a frame stalls the frame,
// and the HUD's movies run on real time), named by the newest driven tick.
struct BurstFrame {
    uint32_t tick;
    int w, h;
    std::vector<uint8_t> rgb;
};

std::string g_burstDir;
int g_burstLeft = 0, g_burstScale = 2, g_burstIndex = 0;
std::vector<BurstFrame> g_burstFrames;

void TakeBurstFrame(IDXGISwapChain* swapChain) {
    std::string dir;
    {
        std::lock_guard lock(g_shotMutex);
        if (g_burstLeft <= 0) return;
        dir = g_burstDir;
    }
    BurstFrame f{DrivenTicks(), 0, 0, {}};
    if (ReadBack(swapChain, g_burstScale, &f.w, &f.h, &f.rgb)) g_burstFrames.push_back(std::move(f));
    std::lock_guard lock(g_shotMutex);
    if (--g_burstLeft > 0) return;
    std::vector<BurstFrame> frames;
    frames.swap(g_burstFrames);
    std::thread([dir, frames = std::move(frames)] {
        ThreadGuard running;
        // The folder, relative to Win64 as WritePng takes it (one level).
        std::wstring folder = Widen(dir);
        if (!(folder.size() > 1 && folder[1] == L':')) {
            std::wstring win64 = DataDir();
            const size_t cut = win64.find_last_of(L"\\/");
            if (cut != std::wstring::npos) win64.resize(cut);
            folder = win64 + L"\\" + folder;
        }
        CreateDirectoryW(folder.c_str(), nullptr);
        int i = 0;
        for (const BurstFrame& b : frames) {
            char name[64];
            std::snprintf(name, sizeof(name), "\\%03d_t%u.png", i++, b.tick);
            WritePng(dir + name, b.w, b.h, b.rgb);
        }
        Log("overlay: burst of %zu frames written to %s", frames.size(), dir.c_str());
    }).detach();
}

void TakeScreenshot(IDXGISwapChain* swapChain) {
    TakeBurstFrame(swapChain);
    std::string path;
    int scale = 1;
    {
        std::lock_guard lock(g_shotMutex);
        if (g_shotPath.empty()) return;
        path.swap(g_shotPath);
        scale = g_shotScale;
    }
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    if (!ReadBack(swapChain, scale, &w, &h, &rgb)) return;
    if (WritePng(path, w, h, rgb)) Log("overlay: screenshot %s (%dx%d)", path.c_str(), w, h);
}

bool WriteTableSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

HRESULT STDMETHODCALLTYPE TablePresentDetour(IDXGISwapChain* self, UINT syncInterval, UINT flags) {
    HookGuard guard;
    t_inTablePresent = true;
    const HRESULT result = DrawAndPresent(g_tablePresentNext, self, syncInterval, flags);
    t_inTablePresent = false;
    return result;
}

// The first Present of the game's swap chain: from then on its table entry
// draws the overlay (the code hook only passes frames on).
void HookPresentTable(IDXGISwapChain* self) {
    void** slot = *reinterpret_cast<void***>(self) + kPresentSlot;
    if (g_tablePresentSlot || *slot == reinterpret_cast<void*>(&TablePresentDetour)) return;
    g_tablePresentNext = static_cast<PresentFn>(*slot);
    if (!WriteTableSlot(slot, reinterpret_cast<void*>(&TablePresentDetour))) {
        Log("overlay: cannot hook the swap chain's table (error %lu); capture tools hooked later miss the overlay", GetLastError());
        return;
    }
    g_tablePresentSlot = slot;
    Log("overlay: drawn from the swap chain's table, ahead of capture tools");
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* self, UINT syncInterval, UINT flags) {
    HookGuard guard;
    if (t_inTablePresent) return g_origPresent(self, syncInterval, flags);
    if (!Stopping()) HookPresentTable(self);
    return DrawAndPresent(g_origPresent, self, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                              UINT flags) {
    HookGuard guard;
    // The back buffer goes away: our view of it must be gone first.
    if (self == g_swapChain) ReleaseTarget();
    return g_origResizeBuffers(self, count, width, height, format, flags);
}

// Present and ResizeBuffers addresses, from a throwaway device and swap chain
// on a hidden window.
bool FindSwapChainFunctions() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PolarityOverlayProbe";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance,
                             nullptr);
    if (!w) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = w;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                     D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context);
    if (SUCCEEDED(hr) && swapChain) {
        void** vtable = *reinterpret_cast<void***>(swapChain);
        g_presentTarget = vtable[kPresentSlot];
        g_resizeTarget = vtable[kResizeBuffersSlot];
    }
    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return g_presentTarget && g_resizeTarget;
}

} // namespace

// The game's main window, once it is visible.
HWND GameWindow() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND w, LPARAM param) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            wchar_t cls[32] = {};
            GetClassNameW(w, cls, 32);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || std::wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
            *reinterpret_cast<HWND*>(param) = w;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

void InstallPresentHooks() {
    if (!FindSwapChainFunctions()) {
        Log("overlay: no D3D11 swap chain functions");
        return;
    }
    const bool ok = MH_CreateHook(g_presentTarget, reinterpret_cast<void*>(&PresentDetour),
                                  reinterpret_cast<void**>(&g_origPresent)) == MH_OK &&
                    MH_CreateHook(g_resizeTarget, reinterpret_cast<void*>(&ResizeBuffersDetour),
                                  reinterpret_cast<void**>(&g_origResizeBuffers)) == MH_OK &&
                    MH_EnableHook(g_presentTarget) == MH_OK && MH_EnableHook(g_resizeTarget) == MH_OK;
    Log("overlay %s (F1 or Back + Start)", ok ? "installed" : "NOT installed");
}

bool InstallOverlay(const std::wstring& panelPath) {
    g_panelPath = panelPath;
    // Finding Present takes a D3D11 device and swap chain of our own. Made
    // while the game was creating its device (the mod starts alongside it),
    // that crashed the game in ntdll now and then (access violation at
    // ntdll+0x1A12D, several per day of test starts, every one just before
    // "overlay installed"): it waits until the game's window has been up for
    // two seconds, by then the game's device exists. ImGui starts on the
    // first Present after that.
    std::thread([] {
        const auto start = std::chrono::steady_clock::now();
        while (!GameWindow()) {
            if (std::chrono::steady_clock::now() - start > std::chrono::minutes(5)) {
                Log("overlay: no game window after 5 minutes; NOT installed");
                return;
            }
            Sleep(100);
        }
        Sleep(2000);
        InstallPresentHooks();
    }).detach();
    return true;
}

void UnhookPresentTable() {
    if (!g_tablePresentSlot) return;
    if (*g_tablePresentSlot == reinterpret_cast<void*>(&TablePresentDetour)) {
        WriteTableSlot(g_tablePresentSlot, reinterpret_cast<void*>(g_tablePresentNext));
    } else {
        Log("overlay: the swap chain's table entry was replaced after ours; left as it is");
    }
    g_tablePresentSlot = nullptr;
}

void ShutdownOverlay() {
    g_visible = false;
    ReleaseImGui();
}

void RequestSelectPreview(int phase, bool spectator) { g_selectRequest = phase + (spectator ? 4 : 0); }

void ReloadOverlayChoices() { g_reloadChoices = true; }

void RequestShotBurst(const std::string& dir, int frames, int scale) {
    std::lock_guard lock(g_shotMutex);
    g_burstDir = dir;
    g_burstScale = std::clamp(scale, 1, 4);
    g_burstLeft = std::clamp(frames, 0, 600);
    g_burstFrames.clear();
}

void RequestScreenshot(const std::string& path, int scale) {
    std::lock_guard lock(g_shotMutex);
    g_shotPath = path;
    g_shotScale = std::clamp(scale, 1, 4);
}

void ShowLastBenchmarkResult() {
    OpenHub();
    OpenSettingsScreen("benchlast");
    OpenBenchmarkModal(true);
}

void SetOverlayVisible(bool visible) {
    if (g_visible.exchange(visible) != visible) Log("overlay %s", visible ? "shown" : "hidden");
}

void ToggleOverlay() {
    // Not over a session's select screen: B there leaves (its own dialog).
    if (!g_visible && g_selectLive) return;
    SetOverlayVisible(!g_visible);
}

void SetOverlayTexture(void* srv, uint32_t width, uint32_t height, const std::string& label) {
    std::lock_guard lock(g_textureMutex);
    if (g_texture) g_texture->Release();
    g_texture = static_cast<ID3D11ShaderResourceView*>(srv);
    g_textureW = width;
    g_textureH = height;
    g_textureLabel = label;
}

bool OverlayVisible() {
    return g_visible;
}

namespace {

// The mod's screens take the input now.
bool ScreensTakeInput() {
    return g_visible || g_voting || g_watchHold || g_selectLive || SelectPreviewOpen() || HubOpen();
}

// Any pad button, key or mouse button down: the pads as the XInput hook last
// saw them, the pads the game reads past XInput (DirectInput, Steam Input),
// and the keyboard and mouse.
bool AnyInputDown() {
    for (const PadState& pad : g_pads) {
        if (pad.buttons) return true;
    }
    if (InputGamePadKeys()) return true;
    for (int vk = 0x01; vk <= 0xFE; vk++) {
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    }
    return false;
}

// After the mod's screens let go, the game still gets nothing until every
// button and key is up: the press that closed them was still held, and the
// game took it as a new press. Confirming Leave on the leave dialog ended the
// session and the held A picked the main menu's Arcade ("You are not logged
// in to the server", 2026-11-04); the vote's and the panel's Leave did the
// same. A key held longer than kReleaseHoldMs (stuck, or a pad unplugged
// while held) lets the game have the input anyway.
constexpr ULONGLONG kReleaseHoldMs = 3000;
std::atomic<bool> g_releaseHold{false};
std::atomic<ULONGLONG> g_releaseHoldFrom{0};

} // namespace

bool OverlayTakesInput() {
    if (ScreensTakeInput()) {
        g_releaseHold = true;
        g_releaseHoldFrom = GetTickCount64();
        return true;
    }
    if (!g_releaseHold) return false;
    if (AnyInputDown() && GetTickCount64() - g_releaseHoldFrom < kReleaseHoldMs) return true;
    if (g_releaseHold.exchange(false)) Log("overlay: input released to the game");
    return false;
}

void SetVsync(bool on) {
    g_noVsync = !on;
    Log("overlay: vsync %s", on ? "on (the game's own)" : "off (present with sync interval 0)");
}

bool VsyncOn() { return !g_noVsync; }

std::string VsyncInfo() {
    // The time in Present since the last call, per frame.
    static int64_t lastMicros = 0, lastFrames = 0;
    const int64_t micros = g_presentMicros, frames = g_presentFrames;
    const int64_t count = frames - lastFrames;
    const double perFrame = count > 0 ? double(micros - lastMicros) / double(count) / 1000.0 : 0;
    lastMicros = micros;
    lastFrames = frames;
    char line[256];
    DXGI_SWAP_CHAIN_DESC desc{};
    const bool haveDesc = g_swapChain && SUCCEEDED(g_swapChain->GetDesc(&desc));
    std::snprintf(line, sizeof(line),
                  "vsync %s, the game's sync interval %u flags %x, swap effect %d, %u buffers, %s, %.2f ms in Present over %lld frames, %.1f fps",
                  g_noVsync ? "off" : "on", g_gameSyncInterval.load(), g_gamePresentFlags.load(), haveDesc ? int(desc.SwapEffect) : -1,
                  haveDesc ? desc.BufferCount : 0, haveDesc ? (desc.Windowed ? "windowed" : "fullscreen") : "?", perFrame,
                  static_cast<long long>(count), RenderFps());
    return line;
}

void PreviewOverlay(const std::string& words) {
    std::lock_guard lock(g_previewMutex);
    g_previewWords += " " + words;
}

void SetOverlayBuild(const std::string& version) {
    g_build = version;
}

void SetMigratedNotice() {
    g_migrated = true;
}

float RenderFps() {
    return g_fps;
}

bool OverlayWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_F1 && !(lp & (1 << 30))) { // not on key repeat: Back + Start
        g_chordKey = true;
        return true;
    }
    if (w != g_inputWindow) return false;
    const bool keyboard = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
    const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
    // Not WM_MOUSELEAVE: ImGui's backend asks for it with TrackMouseEvent,
    // which from the render thread reports a leave at once, and after a
    // leave it takes the position from the real cursor instead of the
    // messages. Positions come from WM_MOUSEMOVE only.
    const bool relevant = keyboard || mouse || msg == WM_SETFOCUS || msg == WM_KILLFOCUS;
    if (relevant && msg != WM_INPUT) {
        std::lock_guard lock(g_inputMutex);
        if (g_input.size() < 1024) g_input.push_back({msg, wp, lp});
    }
    // Open (or until the keys that closed it are up): the game sees no
    // keyboard or mouse (WM_INPUT is its raw mouse).
    return OverlayTakesInput() && (keyboard || mouse);
}

void OverlayPad(unsigned user, const XINPUT_GAMEPAD& pad) {
    if (user >= XUSER_MAX_COUNT) return;
    g_xinputAt = GetTickCount64();
    g_pads[user].buttons = pad.wButtons;
    g_pads[user].lx = pad.sThumbLX;
    g_pads[user].ly = pad.sThumbLY;
    g_pads[user].lt = pad.bLeftTrigger;
    g_pads[user].rt = pad.bRightTrigger;
}

} // namespace sfv::mod
