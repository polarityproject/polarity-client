#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

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
        const int n = kMaxDelaySetting + 2;
        const int8_t next = static_cast<int8_t>(((NetOptions().delaySetting + 1 + in.delta) % n + n) % n - 1);
        NetOptions().delaySetting = next;
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
        SetRoomsWordFilter(g_choices.wordFilter);
        SaveChoices();
    } else if (in.row == "streamer" && in.delta) {
        g_choices.streamerMode = !g_choices.streamerMode;
        SetStreamerMode(g_choices.streamerMode);
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
            RoomsClose();
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

bool InTrainingScene() { return FlowCurrentScene().find("Training") != std::string::npos; }

void StartRoomTraining(const RoomsView& rooms) {
    const NetView nv = NetViewNow();
    if (g_roomTraining != RoomTraining::Off || (nv.phase != NetView::Phase::Idle && nv.phase != NetView::Phase::Ended)) return;
    if (rooms.watching >= 0) {
        RoomsWatch(-1);
        g_watchTable = -1;
    }
    g_roomTraining = RoomTraining::Asked;
    g_roomTrainingAt = ImGui::GetTime();
    Log("overlay: Training while waiting at a table");
    FlowChangeScene("TrainingBattleSetupState");
}

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
        Log("overlay: back from Training (%s)", FlowCurrentScene().c_str());
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
        FlowChangeScene("MainMenuState");
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
    LoadTables();
    InitChoices();
    if (!NetOptions().hasPick) ApplyProfileFavorite(g_choices);
    PickValid(g_choices);
    NetplayOptions o = ChosenOptions();
    o.autoReady = true;
    if (g_tableDelay >= 0) o.inputDelay = static_cast<uint8_t>(g_tableDelay);
    g_choices.delay = o.inputDelay;
    Log("overlay: table %d's set: %s (delay %u)", t.table + 1, t.host ? "opening the match room" : ("joining " + t.code).c_str(),
        o.inputDelay);
    Act([mt, o] { NetPlayTicket(mt, o); });
    RoomsTableSession(t.table, t.host, t.rules);
}

void DrawHubFrame(bool takeInput) {
    constexpr double kFadeSeconds = 0.16;
    static int shown = -1;
    static double changedAt = -1;
    ui::g_backdropEnd = -1;
    g_hubScreen = -1;
    DrawHubScreens(takeInput);
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

CardPick TablePick(const NetplayOptions& o, const std::string& favorite) {
    CardPick p;
    if (o.hasPick) {
        const bool eleven = IsElevenSetup(o.pick);
        p.chara = eleven ? "ELV" : UnpackCode(o.pick.character);
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

void SetTableSkills(int vskill, int vtrigger) {
    InitChoices();
    if (!g_choices.initialised) return;
    Choices& c = g_choices;
    const NetplayOptions now = NetOptions();
    if (now.hasPick) {
        if (IsElevenSetup(now.pick)) return;
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

void OpenHubRooms(const NetplayOptions& o) {
    g_nameSeen = NetLocalPlayerName();
    g_nameChanged = false;
    RoomsOpen(o.server, g_nameSeen, o.server == kDefaultServer ? kDefaultServerAlsoPort : o.serverAlsoPort, o.keeperKey);
}

std::string StageLabelOf(uint16_t stage) {
    const std::string code = FlowStageCode(stage);
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
    if (RoomsScreenOpen() && !SettingsScreenOpen() && rooms.state == RoomsView::State::Failed) {
        Log("hub: the rooms server is gone: the rooms screen gives way to the hub's modal");
        CloseRoomsScreen();
        ShowRoomsProblem();
    }
    const UpdateView update = UpdateViewNow();
    if (RoomsScreenOpen() && !SettingsScreenOpen() && UpdatePending(update) && rooms.state != RoomsView::State::Room &&
        rooms.state != RoomsView::State::Creating && rooms.state != RoomsView::State::Joining) {
        Log("hub: Polarity %s is out: the rooms screen gives way to the hub's update modal", update.latest.c_str());
        CloseRoomsScreen();
        OpenUpdateModal();
    }
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
    if (rooms.state == S::Failed) live.roomsState = "Can't connect";
    else if (!rooms.otherBuild.empty() && !rooms.keeper) live.roomsState = "Update needed";
    else if (rooms.state == S::NoLobby || ((rooms.state == S::Lobby || rooms.state == S::Room) && !rooms.keeper)) live.roomsState = "Unavailable right now";
    else if (rooms.keeper && (rooms.state == S::Browsing || rooms.state == S::Lobby || rooms.state == S::Room)) {
        const int n = rooms.summary.publicRooms, p = rooms.summary.players;
        live.roomsState = std::to_string(n) + (n == 1 ? " public room" : " public rooms") + " \xC2\xB7 " + std::to_string(p) + (p == 1 ? " player" : " players");
    }
    if (!g_tables.loaded) live.roomsState = "Getting ready...";
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
    RoomsSetCharacter(!o.hasPick ? live.offlineChara : IsElevenSetup(o.pick) ? std::string("ELV")
                                                                             : UnpackCode(o.pick.character));
    const CardPick tablePick = TablePick(o, live.offlineChara);
    if (tablePick.editable) RoomsSetSkills(tablePick.vskill, tablePick.vtrigger);
    else RoomsSetSkills(0, 0);
    SetCardPick(tablePick);
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
            g_logLaunch = {true, FlowBattleSceneSwitches(), ImGui::GetTime(), -1};
            ReplayRequestView(in.watch, in.watchTitle, [] { g_reopenLog = true; });
            if (!CurrentFrameDriver()) {
                g_logLaunch = {};
                AddToast("This replay can't be played.", kRed, 4);
            }
        }
        if (g_logLaunch.active) {
            const double now = ImGui::GetTime();
            if (g_logLaunch.blackFrom < 0 && FlowBattleSceneSwitches() != g_logLaunch.switchesAt) g_logLaunch.blackFrom = now;
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
    static std::string roomShown;
    if (rooms.state == S::Room) {
        if (roomShown != rooms.room) {
            roomShown = rooms.room;
            ResetRoomScreen();
            OpenRoomsScreen();
            if (!g_createRules.empty() && rooms.owner == rooms.self) QueueTableRules(g_createRules);
            g_createRules.clear();
        }
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
            if (UpdateCheckIfStale(5 * 60 * 1000)) {
                ShowRoomsProblem();
                break;
            }
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
            if (g_tables.loaded) OpenMyCharacter();
            break;
        case HubAction::UpdateInstall:
            g_restartError.clear();
            UpdateInstall();
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

} // namespace sfv::mod::overlay
