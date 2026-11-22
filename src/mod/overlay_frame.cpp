#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

bool OnMenuScreen() {
    const std::string scene = FlowCurrentScene();
    return !scene.empty() && scene != "BootState" && scene != "InitialState" && scene.find("Battle") == std::string::npos;
}

void DrawMenuHint() {
    DrawMenuNote(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold});
}

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
            if (scan != -1 && ui::MappedPadKeys(scan & 0xFF)) continue;
        }
        ImGui_ImplWin32_WndProcHandler(g_window, m.msg, m.wp, m.lp);
    }
    ImGuiIO& io = ImGui::GetIO();
    FeedPads(io);
    const NetView view = NetViewNow();
    {
        const int other = view.you == 0 ? 1 : 0;
        const bool player = !view.spectator && view.you >= 0 && view.phase != NetView::Phase::Idle && view.phase != NetView::Phase::Ended;
        g_otherHasPick = player && view.seats[other].present && view.seats[other].hasPick;
        g_otherPick = view.seats[other].pick;
    }
    const NetView::Phase phase = view.phase;
    g_inMatch = phase == NetView::Phase::Running;
    if (g_reloadChoices.exchange(false)) g_choices.initialised = false;
    if (phase != NetView::Phase::Idle) {
        LoadTables();
        InitChoices();
    } else if (OnMenuScreen()) {
        LoadTables();
    }
    static bool bootWarmed = false;
    if (!bootWarmed && g_tables.loaded && OnMenuScreen()) {
        bootWarmed = true;
        FlowRunOnGameThread([data = PreviewSelectData()] {
            ProfileFavorites favorites;
            ProfileReadFavorites(&favorites);
            SelectWarmBootTextures(data, favorites.chara, favorites.stage);
            std::lock_guard lock(g_favoritesMutex);
            g_favorites = favorites;
        });
    }
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
        SelectSetData(PreviewSelectData());
        SelectWarmTextures();
    }
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
    const bool overResults = phase == NetView::Phase::PostMatch || (phase == NetView::Phase::Lobby && view.overResults);
    const bool sides = phase == NetView::Phase::PostMatch && !g_visible;
    g_voting = overResults && !view.spectator;
    g_watchHold = overResults && view.spectator;
    const bool leaveUp = g_visible && (phase == NetView::Phase::Running || phase == NetView::Phase::Loading || phase == NetView::Phase::Jumping);
    if (g_visible && !leaveUp) OverlaySetVisible(false);
    const bool visible = g_visible;
    const bool session = phase != NetView::Phase::Idle;
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
        if (!inside) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    if (io.DisplaySize.y > 0 && std::abs(UiScaleFor(io.DisplaySize.y) - g_builtScale) > 0.05f) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        BuildFonts(UiScaleFor(io.DisplaySize.y));
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ToastChanges(view);
    if (OnMenuScreen()) ToastUpdate();
    ToastMigrated();
    ToastReports();
    if (const int wanted = g_selectRequest.load(); wanted >= 0 && g_tables.loaded) {
        g_selectRequest = -1;
        SelectData data = PreviewSelectData();
        data.spectator = wanted >= 4;
        SelectOpenPreview(std::move(data), wanted % 4);
        OverlaySetVisible(false);
    }
    const bool preview = SelectPreviewOpen();
    static float liveAlpha = 0;
    static bool lobbyWas = false;
    const bool setSelect = view.setPlay && phase == NetView::Phase::Lobby && view.overResults;
    const bool tableConnecting = view.setPlay && !setSelect &&
                                 (phase == NetView::Phase::Connecting || phase == NetView::Phase::Hosting ||
                                  phase == NetView::Phase::Joining || phase == NetView::Phase::Lobby ||
                                  phase == NetView::Phase::Jumping);
    const bool lobby = phase == NetView::Phase::Lobby && g_tables.loaded && g_choices.initialised && !preview && (!view.setPlay || setSelect);
    if (lobby && !lobbyWas) {
        SelectSetData(PreviewSelectData());
        SelectReset();
        g_sentSelect = {};
        OverlaySetVisible(false);
        g_toasts.clear();
    }
    lobbyWas = lobby;
    constexpr double kToBlack = 0.3, kBlackHeld = 0.6;
    static bool holding = false;
    static uint64_t switchesAtJump = 0;
    static double blackFrom = -1;
    const double now = ImGui::GetTime();
    if (lobby) {
        holding = true;
        switchesAtJump = FlowBattleSceneSwitches();
        blackFrom = -1;
    } else if (holding && phase == NetView::Phase::Jumping && FlowBattleSceneSwitches() != switchesAtJump) {
        holding = false;
        blackFrom = now;
    } else if (holding && phase != NetView::Phase::Jumping) {
        holding = false;
    }
    const bool toBlack = blackFrom >= 0 && now - blackFrom < kToBlack + kBlackHeld;
    const bool shown = lobby || holding || toBlack;
    liveAlpha = std::clamp(liveAlpha + (shown ? 1.0f : -1.0f) * io.DeltaTime / 0.4f, 0.0f, 1.0f);
    if (blackFrom >= 0 && !toBlack) {
        liveAlpha = 0;
        blackFrom = -1;
    }
    if (preview) liveAlpha = 0;
    g_selectLive = (lobby || holding) && !g_visible;
    if (liveAlpha > 0) {
        SelectInput in;
        SelectDrawLive(SelectFonts{g_fontBig, g_fontHuge, g_fontBody, g_fontBold}, SelectLiveFor(view), liveAlpha, lobby && !g_visible, &in);
        if (lobby) ApplySelectInput(in, view);
        if (toBlack) {
            const float black = static_cast<float>(std::min(1.0, (now - blackFrom) / kToBlack));
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(black * 255)));
        }
    }
    const bool atMenus = OnMenuScreen() && (phase == NetView::Phase::Idle || phase == NetView::Phase::Ended || tableConnecting) && !preview;
    g_hubAllowed = atMenus;
    static bool hubBooted = false;
    if (!hubBooted && atMenus) {
        hubBooted = true;
        HubShow(true);
    }
    if (HubOpen() && !atMenus && !g_logLaunch.active && !g_benchLaunch.active) {
        HubHide();
        RoomsScreenHide();
        BattleLogHide();
    }
    if (g_benchReturn && atMenus && !ReplayViewNow().active) {
        g_benchReturn = false;
        HubShow();
        SettingsScreenShow("bench");
        BenchmarkResult r;
        if (TakeBenchmarkResult(&r)) {
            g_benchShown = r;
            g_benchChoice = 0;
            g_benchModal = BenchModal::Result;
        }
    }
    if (g_reopenLog && atMenus && g_tables.loaded && !ReplayViewNow().active) {
        g_reopenLog = false;
        HubShow();
        BattleLogShow();
        BattleLogRefresh();
    }
    StepRoomTraining();
    for (keeper::TableTicket ticket; RoomsTakeTicket(&ticket);) StartTableSession(ticket);
    for (WatchTicket w; RoomsTakeWatch(&w);) StartTableWatch(w);
    if (RoomsTakeAbort()) Act([] { NetLeave(); });
    if (!HubOpen() && atMenus && g_tables.loaded && RoomsOpened()) {
        if (RoomsSnapshot().state == RoomsView::State::Room) {
            HubShow();
            g_backToRooms = false;
        } else if (g_backToRooms) {
            HubShow();
            RoomsScreenShow();
            g_backToRooms = false;
        }
    }
    if (HubOpen()) DrawHubFrame(!g_visible);
    if (ReplayViewNow().active && !g_benchLaunch.active) BattleLogDrawStripe(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, !g_visible);
    if (g_benchLaunch.active) DrawBenchmarkLaunch(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold});
    if (preview) {
        SelectDrawPreview(SelectFonts{g_fontBig, g_fontHuge, g_fontBody, g_fontBold});
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
        if (stay) OverlaySetVisible(false);
        if (leave) {
            OverlaySetVisible(false);
            Log("overlay: left the match (the leave modal)");
            Act([] { NetLeave(); });
        }
    }
    leaveWas = leaveUp;
    {
        static bool watchPanel = false;
        static float watchAlpha = 0;
        const bool watchingNow = session && view.spectator;
        g_spectating = watchingNow;
        if (g_watchPanelTap.exchange(false) && watchingNow) watchPanel = !watchPanel;
        if (!watchingNow) watchPanel = false;
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
            wl.table = view.setPlay ? g_watchTable : -1;
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
    HitboxesDraw(ImGui::GetBackgroundDrawList(), ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    FrameMeterDraw(ImGui::GetBackgroundDrawList(), g_fontBold);
    DrawPreviews();
    DrawNotices(session && (phase == NetView::Phase::Running || phase == NetView::Phase::PostMatch), view);
    DrawTexturePreview();
    bool texture = false;
    {
        std::lock_guard lock(g_textureMutex);
        texture = g_texture != nullptr;
    }
    UiSoundFlush();
    const bool draw = visible || preview || liveAlpha > 0 || session || hint || HubOpen() || !g_toasts.empty() || g_preview.Any() || texture ||
                      ReplayViewNow().active || HitboxesVisible() || FrameMeterVisible();
    if (!draw || (!g_rtv && !CreateTarget(swapChain))) {
        ImGui::EndFrame();
        return;
    }
    ImGui::Render();

    ID3D11RenderTargetView* saved = nullptr;
    ID3D11DepthStencilView* savedDepth = nullptr;
    g_context->OMGetRenderTargets(1, &saved, &savedDepth);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &saved, savedDepth);
    if (saved) saved->Release();
    if (savedDepth) savedDepth->Release();
}

HRESULT DrawAndPresent(PresentFn next, IDXGISwapChain* self, UINT syncInterval, UINT flags) {
    if (Stopping() || (flags & DXGI_PRESENT_TEST)) return next(self, syncInterval, flags);
    TexturesSaveQueued();
    PakTexturesCreate();
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
    if (frameMicros) hitch::FramePresented(frameMicros, g_inMatch ? "match" : HubOpen() ? "menus"
                                                                                        : nullptr);
    return result;
}

} // namespace sfv::mod::overlay
