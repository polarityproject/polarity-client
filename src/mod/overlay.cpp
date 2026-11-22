#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

std::string BuildVersion() {
    const size_t space = g_build.find(' ');
    return space == std::string::npos ? g_build : g_build.substr(space + 1);
}

void LoadKeyboardMap() {
    static ULONGLONG readAt = 0;
    if (readAt && GetTickCount64() - readAt < 2000) return;
    readAt = GetTickCount64();
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    const std::wstring path = std::wstring(local) + L"\\StreetFighterV\\Saved\\Config\\WindowsNoEditor\\Input2.ini";
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
    for (const PadInput& p : g_pads) {
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

    struct GameKey {
        uint32_t key;
        uint16_t button;
    };

    static const GameKey kGameKeys[] = {
        {padkey::kUp, XINPUT_GAMEPAD_DPAD_UP},
        {padkey::kDown, XINPUT_GAMEPAD_DPAD_DOWN},
        {padkey::kLeft, XINPUT_GAMEPAD_DPAD_LEFT},
        {padkey::kRight, XINPUT_GAMEPAD_DPAD_RIGHT},
        {padkey::kX, XINPUT_GAMEPAD_X},
        {padkey::kY, XINPUT_GAMEPAD_Y},
        {padkey::kA, XINPUT_GAMEPAD_A},
        {padkey::kB, XINPUT_GAMEPAD_B},
        {padkey::kLB, XINPUT_GAMEPAD_LEFT_SHOULDER},
        {padkey::kRB, XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {padkey::kStart, XINPUT_GAMEPAD_START},
        {padkey::kBack, XINPUT_GAMEPAD_BACK},
    };
    const uint32_t keys = InputGamePadKeys() | g_keyPad;
    uint16_t game = 0;
    for (const GameKey& k : kGameKeys) {
        if (keys & k.key) game |= k.button;
    }
    buttons |= game;
    if (keys & padkey::kLT) lt = 255;
    if (keys & padkey::kRT) rt = 255;
    constexpr uint16_t kChord = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    static bool chordWas = false, chordInPress = false;
    static ULONGLONG chordAt = 0;
    const bool chord = (buttons & kChord) == kChord;
    const bool fresh = chord && !chordWas && !chordInPress && GetTickCount64() - chordAt > 300;
    if (fresh || g_chordKey.exchange(false)) {
        chordAt = GetTickCount64();
        chordInPress = chord;
        if (g_visible) {
            Log("overlay: Back + Start again: the leave modal closes");
            OverlaySetVisible(false);
        } else if (g_hubAllowed && !HubOpen()) {
            Log("overlay: Back + Start: the hub");
            HubShow();
        } else if (g_spectating) g_watchPanelTap = true;
        else if (g_inMatch) OverlaySetVisible(true);
    }
    chordWas = chord;
    constexpr ULONGLONG kTapMs = 450;
    static bool startWas = false;
    static ULONGLONG startDownAt = 0;
    const bool start = buttons & XINPUT_GAMEPAD_START;
    if (start && !startWas) startDownAt = GetTickCount64();
    if (!start && startWas && !chordInPress && g_spectating && !g_visible && GetTickCount64() - startDownAt < kTapMs) g_watchPanelTap = true;
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
    if (chordInPress) buttons &= static_cast<uint16_t>(~kChord);
    for (const Map& m : kButtons) io.AddKeyEvent(m.key, (buttons & m.button) != 0);
    constexpr float kDead = 7849.0f;
    auto axis = [&](ImGuiKey key, float v) {
        const float a = std::clamp((v - kDead) / (32767.0f - kDead), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, a > 0.1f, a);
    };
    axis(ImGuiKey_GamepadLStickLeft, -static_cast<float>(lx));
    axis(ImGuiKey_GamepadLStickRight, static_cast<float>(lx));
    axis(ImGuiKey_GamepadLStickUp, static_cast<float>(ly));
    axis(ImGuiKey_GamepadLStickDown, -static_cast<float>(ly));
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

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* self, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    HookGuard guard;
    if (self == g_swapChain) ReleaseTarget();
    return g_origResizeBuffers(self, count, width, height, format, flags);
}

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

} // namespace sfv::mod::overlay

namespace sfv::mod {

using namespace overlay;

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

bool OverlayInstall(const std::wstring& panelPath) {
    g_panelPath = panelPath;
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

void OverlayUnhookPresentTable() {
    if (!g_tablePresentSlot) return;
    if (*g_tablePresentSlot == reinterpret_cast<void*>(&TablePresentDetour)) {
        WriteTableSlot(g_tablePresentSlot, reinterpret_cast<void*>(g_tablePresentNext));
    } else {
        Log("overlay: the swap chain's table entry was replaced after ours; left as it is");
    }
    g_tablePresentSlot = nullptr;
}

void OverlayShutdown() {
    g_visible = false;
    ReleaseImGui();
}

void OverlayRequestSelectPreview(int phase, bool spectator) { g_selectRequest = phase + (spectator ? 4 : 0); }

void OverlayReloadChoices() { g_reloadChoices = true; }

void OverlayRequestShotBurst(const std::string& dir, int frames, int scale) {
    std::lock_guard lock(g_shotMutex);
    g_burstDir = dir;
    g_burstScale = std::clamp(scale, 1, 4);
    g_burstLeft = std::clamp(frames, 0, 600);
    g_burstFrames.clear();
}

void OverlayRequestScreenshot(const std::string& path, int scale) {
    std::lock_guard lock(g_shotMutex);
    g_shotPath = path;
    g_shotScale = std::clamp(scale, 1, 4);
}

void OverlayShowLastBenchmarkResult() {
    HubShow();
    SettingsScreenShow("benchlast");
    OpenBenchmarkModal(true);
}

void OverlaySetVisible(bool visible) {
    if (g_visible.exchange(visible) != visible) Log("overlay %s", visible ? "shown" : "hidden");
}

void OverlayToggle() {
    if (!g_visible && g_selectLive) return;
    OverlaySetVisible(!g_visible);
}

void OverlaySetTexture(void* srv, uint32_t width, uint32_t height, const std::string& label) {
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

bool ScreensTakeInput() {
    return g_visible || g_voting || g_watchHold || g_selectLive || SelectPreviewOpen() || HubOpen();
}

bool AnyInputDown() {
    for (const PadInput& pad : g_pads) {
        if (pad.buttons) return true;
    }
    if (InputGamePadKeys()) return true;
    constexpr int kFirstKey = 0x01, kLastKey = 0xFE; // every virtual-key code
    constexpr SHORT kKeyDown = static_cast<SHORT>(0x8000);
    for (int vk = kFirstKey; vk <= kLastKey; vk++) {
        if (GetAsyncKeyState(vk) & kKeyDown) return true;
    }
    return false;
}

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

void OverlaySetVsync(bool on) {
    g_noVsync = !on;
    Log("overlay: vsync %s", on ? "on (the game's own)" : "off (present with sync interval 0)");
}

bool OverlayVsyncOn() { return !g_noVsync; }

std::string OverlayVsyncInfo() {
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
                  static_cast<long long>(count), OverlayRenderFps());
    return line;
}

void OverlayPreview(const std::string& words) {
    std::lock_guard lock(g_previewMutex);
    g_previewWords += " " + words;
}

void OverlaySetBuild(const std::string& version) {
    g_build = version;
}

void OverlaySetMigratedNotice() {
    g_migrated = true;
}

float OverlayRenderFps() {
    return g_fps;
}

bool OverlayWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_F1 && !(lp & (1 << 30))) {
        g_chordKey = true;
        return true;
    }
    if (w != g_inputWindow) return false;
    const bool keyboard = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
    const bool mouse = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
    const bool relevant = keyboard || mouse || msg == WM_SETFOCUS || msg == WM_KILLFOCUS;
    if (relevant && msg != WM_INPUT) {
        std::lock_guard lock(g_inputMutex);
        if (g_input.size() < 1024) g_input.push_back({msg, wp, lp});
    }
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
