#include "mod/overlay_internal.h"

#include "core/text.h"

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
        if (!g_panelPath.empty()) ConfigSaveNetplayChoices(g_panelPath, o, o.stageCode);
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

std::string ServerLabel(const std::string& server) { return server == kDefaultServer ? std::string(TR(Str::common_public_server)) : server; }

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
    const std::string current = u.current.empty() ? g_build : TRF(Str::common_version, u.current.c_str());
    if (u.state == U::Available || u.state == U::Downloading || u.state == U::Ready) {
        SettingsRow& r = row("install", TR(Str::settings_section_updates), TRF(Str::settings_install_label, u.latest.c_str()));
        r.highlight = true;
        r.kicker = TR(Str::settings_install_kicker);
        r.title = TRF(Str::common_version, u.latest.c_str());
        if (!u.notes.empty()) r.lines.push_back(u.published.empty() ? u.notes : TRF(Str::settings_install_notes_from, u.published.c_str(), u.notes.c_str()));
        r.goldFirst = !r.lines.empty();
        r.lines.push_back(TR(Str::settings_install_same_version));
        if (u.state == U::Available) {
            r.value = TR(Str::settings_install_value);
            r.action = TR(Str::settings_install_action);
            r.hint = TR(Str::settings_install_hint);
        } else if (u.state == U::Downloading) {
            const int pct = u.total ? static_cast<int>(100 * u.done / u.total) : 0;
            r.value = TRF(Str::settings_install_downloading, pct);
            r.hint = TR(Str::settings_install_downloading_hint);
        } else {
            r.value = TR(Str::settings_install_restart_value);
            r.action = TR(Str::settings_install_restart);
            r.hint = "";
        }
    }
    {
        SettingsRow& r = row("check", TR(Str::settings_section_updates), TR(Str::settings_check_label));
        r.highlight = true;
        r.value = TR(u.state == U::Checking ? Str::settings_check_checking : u.state == U::Off ? Str::common_off
                                                                                               : Str::settings_check_action);
        r.enabled = u.state != U::Off;
        r.action = u.state == U::Checking ? "" : TR(Str::settings_check_action);
        r.kicker = TR(Str::settings_section_updates);
        r.title = current;
        switch (u.state) {
            case U::Off: r.lines = {TRF(Str::settings_check_off_line, u.message.c_str())}; break;
            case U::Idle: r.lines = {TR(Str::settings_check_idle)}; break;
            case U::Checking: r.lines = {TR(Str::settings_check_checking_line)}; break;
            case U::UpToDate: r.lines = {TR(Str::settings_check_up_to_date)}; break;
            case U::Available: r.lines = {TRF(Str::settings_check_available, u.latest.c_str())}; break;
            case U::Downloading: r.lines = {TRF(Str::settings_check_downloading, u.latest.c_str())}; break;
            case U::Ready: r.lines = {TRF(Str::settings_check_ready, u.latest.c_str())}; break;
            case U::Failed: r.lines = {TRF(Str::settings_check_failed, u.message.c_str())}; break;
        }
        r.hint = TR(Str::settings_check_hint);
    }
    {
        SettingsRow& r = row("checkstart", TR(Str::settings_section_updates), TR(Str::settings_check_start_label));
        r.arrows = true;
        r.value = TR(ConfigReadMainIni(L"update", L"check", 1) ? Str::common_on : Str::common_off);
        r.kicker = TR(Str::settings_section_updates);
        r.title = TR(Str::settings_check_start_label);
        r.lines = {TR(Str::settings_check_start_line)};
    }
    {
        SettingsRow& r = row("name", TR(Str::settings_section_online), TR(Str::settings_name_label));
        r.text = true;
        r.editValue = g_choices.name;
        r.value = g_choices.name[0] ? std::string(g_choices.name) : TRF(Str::settings_name_steam, NetLocalPlayerName().c_str());
        r.kicker = TR(Str::settings_section_online);
        r.title = g_choices.name[0] ? std::string(g_choices.name) : NetLocalPlayerName();
        r.lines = {TR(Str::settings_name_line), TR(Str::settings_name_deck)};
    }
    {
        SettingsRow& r = row("fps", TR(Str::settings_section_online), TR(Str::settings_fps_label));
        r.arrows = true;
        r.value = TR(g_choices.showFps ? Str::common_shown : Str::common_hidden);
        r.kicker = TR(Str::settings_section_online);
        r.title = TR(Str::settings_fps_label);
        r.lines = {TR(Str::settings_fps_line)};
    }
    {
        SettingsRow& r = row("country", TR(Str::settings_section_online), TR(Str::settings_country_label));
        r.arrows = true;
        r.value = TR(g_choices.shareCountry ? Str::common_shown : Str::common_hidden);
        r.kicker = TR(Str::settings_section_online);
        r.title = TR(Str::settings_country_label);
        r.lines = {TR(Str::settings_country_line), TR(Str::settings_country_hidden)};
    }
    {
        SettingsRow& r = row("wordfilter", TR(Str::settings_section_online), TR(Str::settings_filter_label));
        r.arrows = true;
        r.value = TR(g_choices.wordFilter ? Str::common_on : Str::common_off);
        r.kicker = TR(Str::settings_section_online);
        r.title = TR(Str::settings_filter_label);
        r.lines = {TR(Str::settings_filter_line), TR(Str::settings_filter_always)};
    }
    {
        SettingsRow& r = row("streamer", TR(Str::settings_section_online), TR(Str::settings_streamer_label));
        r.arrows = true;
        r.value = TR(g_choices.streamerMode ? Str::common_on : Str::common_off);
        r.kicker = TR(Str::settings_section_online);
        r.title = TR(Str::settings_streamer_label);
        r.lines = {TR(Str::settings_streamer_line), TR(Str::settings_streamer_copy)};
    }
    {
        SettingsRow& r = row("delay", TR(Str::settings_section_online), TR(Str::settings_delay_label));
        const int fixed = NetOptions().delaySetting;
        r.arrows = true;
        r.value = fixed >= 0 ? FramesText(fixed) : std::string(TR(Str::common_auto));
        r.kicker = TR(Str::settings_section_online);
        r.title = fixed >= 0 ? TRF(Str::settings_delay_title_frames, FramesText(fixed).c_str()) : std::string(TR(Str::settings_delay_title_auto));
        r.lines = {fixed == 0  ? std::string(TR(Str::settings_delay_none))
                   : fixed > 0 ? TRF(Str::settings_delay_fixed, FramesText(fixed).c_str())
                               : std::string(TR(Str::settings_delay_auto)),
                   TR(Str::settings_delay_tradeoff)};
    }
    {
        SettingsRow& r = row("server", TR(Str::settings_section_online), TR(Str::settings_server_label));
        const std::vector<std::string> servers = ServerChoices();
        r.arrows = servers.size() > 1;
        r.value = ServerLabel(NetOptions().server);
        r.kicker = TR(Str::settings_section_online);
        r.title = ServerLabel(NetOptions().server);
        r.lines = {TR(Str::settings_server_line), TR(Str::settings_server_others)};
    }
    {
        SettingsRow& r = row("hitboxes", TR(Str::settings_section_training), TR(Str::settings_hitboxes_label));
        r.arrows = true;
        r.value = TR(HitboxesInTraining() ? Str::common_shown : Str::common_hidden);
        r.kicker = TR(Str::settings_section_training);
        r.title = TR(Str::settings_hitboxes_label);
        r.lines = {TR(Str::settings_hitboxes_line)};
    }
    {
        SettingsRow& r = row("framemeter", TR(Str::settings_section_training), TR(Str::settings_meter_label));
        r.arrows = true;
        r.value = TR(FrameMeterInTraining() ? Str::common_shown : Str::common_hidden);
        r.kicker = TR(Str::settings_section_training);
        r.title = TR(Str::settings_meter_label);
        r.lines = {TR(Str::settings_meter_line)};
    }
    {
        SettingsRow& r = row("bench", TR(Str::settings_section_benchmark), TR(Str::settings_bench_label));
        r.highlight = true;
        r.value = TR(Str::settings_bench_action);
        r.action = TR(Str::settings_bench_action);
        r.hint = TR(Str::settings_bench_hint);
    }
    {
        const BenchmarkResult b = BenchmarkLast();
        SettingsRow& r = row("benchlast", TR(Str::settings_section_benchmark), TR(Str::settings_bench_last_label));
        if (b.valid) {
            const std::time_t t = static_cast<std::time_t>(b.when);
            std::tm tm{};
            localtime_s(&tm, &t);
            const Str month = static_cast<Str>(static_cast<int>(Str::common_month_1) + std::clamp(tm.tm_mon, 0, 11));
            r.value = TRF(b.Pass() ? Str::settings_bench_last_pass : Str::settings_bench_last_fail, b.averageFps, TR(month), tm.tm_mday);
            r.action = TR(Str::settings_bench_last_action);
            r.hint = TR(Str::settings_bench_last_hint);
        } else {
            r.value = TR(Str::settings_bench_last_none);
            r.enabled = false;
        }
    }
    {
        SettingsRow& r = row("reports", TR(Str::settings_section_reports), TR(Str::settings_reports_label));
        r.arrows = true;
        r.value = TR(g_choices.uploadReports ? Str::common_on : Str::common_off);
        r.kicker = TR(Str::settings_section_reports);
        r.title = TR(Str::settings_reports_label);
        r.lines = {TR(Str::settings_reports_line), TR(Str::settings_reports_what)};
    }
    {
        SettingsRow& r = row("folder", TR(Str::settings_section_reports), TR(Str::settings_folder_label));
        r.highlight = true;
        r.value = TR(Str::settings_folder_action);
        r.action = TR(Str::settings_folder_action);
        r.kicker = TR(Str::settings_folder_kicker);
        std::string path;
        for (wchar_t ch : FilesDataDir()) path += ch < 128 ? static_cast<char>(ch) : '_';
        r.title = path;
        r.lines = {TR(Str::settings_folder_line)};
        r.hint = TR(Str::settings_folder_hint);
    }
    {
        SettingsRow& r = row("pack", TR(Str::settings_section_reports), TR(Str::settings_pack_label));
        r.highlight = true;
        r.value = TR(Str::settings_pack_value);
        r.action = TR(Str::settings_pack_action);
        r.kicker = TR(Str::settings_pack_kicker);
        r.title = TR(Str::settings_pack_label);
        r.lines = {TR(Str::settings_pack_line)};
        if (!g_packResult.empty()) r.lines.push_back(g_packResult);
    }
    {
        SettingsRow& r = row("credits", TR(Str::settings_section_about), TR(Str::settings_credits_label));
        r.kicker = TR(Str::settings_section_about);
        r.title = TR(Str::settings_credits_label);
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
            if (!UpdateRestartGame(g_window, &error)) g_packResult = TRF(Str::settings_restart_failed, error.c_str());
        } else UpdateInstall();
    } else if (in.row == "check" && in.confirm) UpdateCheck();
    else if (in.row == "bench" && in.confirm) OpenBenchmarkModal(false);
    else if (in.row == "benchlast" && in.confirm) OpenBenchmarkModal(true);
    else if (in.row == "checkstart" && in.delta) ConfigSaveMainIni(L"update", L"check", ConfigReadMainIni(L"update", L"check", 1) ? 0 : 1);
    else if (in.row == "hitboxes" && in.delta) HitboxesSetInTraining(!HitboxesInTraining());
    else if (in.row == "framemeter" && in.delta) FrameMeterSetInTraining(!FrameMeterInTraining());
    else if (in.row == "name" && in.typed) {
        std::snprintf(g_choices.name, sizeof(g_choices.name), "%s", in.text.c_str());
        SaveChoices();
    } else if (in.row == "delay" && in.delta) {
        const int n = kMaxDelaySetting + 2;
        const int8_t next = static_cast<int8_t>(((NetOptions().delaySetting + 1 + in.delta) % n + n) % n - 1);
        NetOptions().delaySetting = next;
        Act([next] {
            NetOptions().delaySetting = next;
            if (!g_panelPath.empty()) ConfigSaveNetplayChoices(g_panelPath, NetOptions(), NetOptions().stageCode);
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
        RoomsSetWordFilter(g_choices.wordFilter);
        SaveChoices();
    } else if (in.row == "streamer" && in.delta) {
        g_choices.streamerMode = !g_choices.streamerMode;
        RoomsScreenSetStreamerMode(g_choices.streamerMode);
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
    } else if (in.row == "folder" && in.confirm) FilesOpenDataFolder();
    else if (in.row == "pack" && in.confirm) {
        std::string error;
        const std::wstring zip = FilesPackLogsToShare(&error);
        if (zip.empty()) g_packResult = TRF(Str::settings_pack_failed, error.c_str());
        else {
            std::string name;
            for (wchar_t ch : zip.substr(zip.find_last_of(L'\\') + 1)) name += ch < 128 ? static_cast<char>(ch) : '_';
            g_packResult = TRF(Str::settings_pack_done, name.c_str());
            FilesOpenDataFolder(zip);
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
    static HubScreen shown = HubScreen::None;
    static double changedAt = -1;
    ui::g_backdropEnd = -1;
    g_hubScreen = HubScreen::None;
    DrawHubScreens(takeInput);
    const double now = ImGui::GetTime();
    if (g_hubScreen != shown) {
        if (shown != HubScreen::None && g_hubScreen != HubScreen::None) changedAt = now;
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
        if (!g_panelPath.empty()) ConfigSaveNetplayChoices(g_panelPath, NetOptions(), NetOptions().stageCode);
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
        RoomsScreenShow();
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
    g_hubScreen = SettingsScreenOpen()                    ? HubScreen::Settings
                  : g_myCharacterOpen                     ? HubScreen::MyCharacter
                  : rooms.state == RoomsView::State::Room ? HubScreen::Room
                  : RoomsScreenOpen()                     ? HubScreen::RoomBrowser
                                                          : HubScreen::Hub;
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
        RoomsScreenHide();
        HubShowRoomsProblem();
    }
    const UpdateView update = UpdateViewNow();
    if (RoomsScreenOpen() && !SettingsScreenOpen() && UpdatePending(update) && rooms.state != RoomsView::State::Room &&
        rooms.state != RoomsView::State::Creating && rooms.state != RoomsView::State::Joining) {
        Log("hub: Polarity %s is out: the rooms screen gives way to the hub's update modal", update.latest.c_str());
        RoomsScreenHide();
        HubOpenUpdateModal();
    }
    static std::string offered;
    if (HubUpdateModalOpen()) offered = update.latest;
    if (HubOpen() && !RoomsScreenOpen() && !SettingsScreenOpen() && update.state == UpdateView::State::Available &&
        update.latest != offered && !HubUpdateModalOpen()) {
        offered = update.latest;
        HubOpenUpdateModal();
    }
    HubLive live;
    live.player = NetLocalPlayerName();
    live.server = o.server == kDefaultServer ? std::string(TR(Str::common_public_server)) : o.server;
    using S = RoomsView::State;
    live.serverState = rooms.state == S::Off ? 0 : rooms.state == S::Connecting ? 1
                                               : rooms.state == S::Failed       ? 3
                                                                                : 2;
    live.version = g_build;
    if (rooms.state == S::Failed) live.roomsState = TR(Str::hub_rooms_cant_connect);
    else if (!rooms.otherBuild.empty() && !rooms.keeper) live.roomsState = TR(Str::hub_rooms_update_needed);
    else if (rooms.state == S::NoLobby || ((rooms.state == S::Lobby || rooms.state == S::Room) && !rooms.keeper)) live.roomsState = TR(Str::hub_rooms_unavailable);
    else if (rooms.keeper && (rooms.state == S::Browsing || rooms.state == S::Lobby || rooms.state == S::Room)) {
        const int n = rooms.summary.publicRooms, p = rooms.summary.players;
        live.roomsState = TRF(n == 1 ? (p == 1 ? Str::hub_rooms_count_room_player : Str::hub_rooms_count_room_players)
                                     : (p == 1 ? Str::hub_rooms_count_rooms_player : Str::hub_rooms_count_rooms_players),
                              n, p);
    }
    if (!g_tables.loaded) live.roomsState = TR(Str::hub_rooms_getting_ready);
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
    live.roomsInfo = TR(Str::hub_rooms_info);
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
    RoomSetCardPick(tablePick);
    live.myChara = tablePick.chara.empty() ? live.offlineChara : tablePick.chara;
    live.myStage = live.offlineStage;
    for (size_t i = 0; i < g_tables.stages.size(); i++) {
        if (g_tables.stages[i] == o.stageCode) live.myStage = o.stageCode;
    }
    live.mySubtitle = live.myChara == "ELV" ? std::string(TR(Str::hub_my_eleven))
                                            : TRF(Str::hub_my_skills, CharaLabel(live.myChara).c_str(), tablePick.vskill, tablePick.vtrigger);
    live.roomsChara = "CNL";
    live.roomsStage = "CHN";
    if (live.offlineStage.empty() && !g_tables.stages.empty()) live.offlineStage = g_tables.stages.front();
    static bool warmed = false;
    if (!warmed) {
        warmed = true;
        HubWarmTextures(live);
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
        BattleLogDraw(fonts, bl, takeInput && !g_logLaunch.active, &in);
        if (in.back) BattleLogHide();
        if (!in.watch.empty() && !g_logLaunch.active) {
            Log("battle log: watching %s", in.watch.c_str());
            g_logLaunch = {true, FlowBattleSceneSwitches(), ImGui::GetTime(), -1};
            ReplayRequestView(in.watch, in.watchTitle, [] { g_reopenLog = true; });
            if (!BattleCurrentFrameDriver()) {
                g_logLaunch = {};
                AddToast(TR(Str::hub_toast_bad_replay), kRed, 4);
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
                BattleLogHide();
                HubHide();
            }
        }
        return;
    }
    if (SettingsScreenOpen()) {
        SettingsInput in;
        const bool modal = g_benchModal != BenchModal::None;
        SettingsScreenDraw(fonts, BuildVersion(), SettingsRows(), takeInput && !modal, &in);
        if (!in.row.empty()) ApplySetting(in);
        if (in.back && !modal) SettingsScreenHide();
        if (modal) DrawBenchmarkModal(fonts, takeInput);
        return;
    }
    static std::string roomShown;
    if (rooms.state == S::Room) {
        if (roomShown != rooms.room) {
            roomShown = rooms.room;
            RoomReset();
            RoomsScreenShow();
            if (!g_createRules.empty() && rooms.owner == rooms.self) RoomQueueTableRules(g_createRules);
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
        RoomSetCardDelay(NetPreferredDelay(opponentPing));
        static bool namesSet = false;
        if (!namesSet && g_tables.loaded) {
            namesSet = true;
            std::vector<std::pair<std::string, std::string>> names;
            for (size_t i = 0; i < g_tables.charas.size() && i < g_tables.labels.size(); i++) names.push_back({g_tables.charas[i], g_tables.labels[i]});
            RoomSetCharaNames(std::move(names));
            std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> skills;
            for (size_t i = 0; i < g_tables.charas.size() && i < g_tables.vskills.size() && i < g_tables.vtriggers.size(); i++) {
                skills[g_tables.charas[i]] = {g_tables.vskills[i], g_tables.vtriggers[i]};
            }
            RoomSetSkillNames(std::move(skills));
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
            RoomSetTableHandoff(h);
        }
        if (!g_myCharacterOpen) RoomDrawScreen(fonts, rooms, takeInput, &in);
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
            AddToast(TR(Str::hub_toast_slow_down), ImVec4(1, 0.8f, 0.4f, 1), 2);
        }
        if (in.copiedCode) AddToast(TR(Str::hub_toast_code_copied), ImVec4(1, 1, 1, 1), 2);
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
        RoomsScreenDraw(fonts, rooms, takeInput, &in);
        RoomsPeek(in.focusedRoom);
        if (in.clearNotice) RoomsClearNotice();
        if (in.settings_) SettingsScreenShow(rooms.otherBuild.empty() ? "server" : "check");
        if (in.create) {
            g_createRules = in.tableRules;
            RoomsCreate(in.settings);
        }
        if (!in.join.empty()) {
            g_createRules.clear();
            RoomsJoin(in.join);
        }
        if (in.back) RoomsScreenHide();
        return;
    }
    switch (HubDraw(fonts, live, takeInput)) {
        case HubAction::PlayOffline:
            HubHide();
            RoomsClose();
            break;
        case HubAction::CustomRooms:
            if (UpdateCheckIfStale(5 * 60 * 1000)) {
                HubShowRoomsProblem();
                break;
            }
            if (!g_tables.loaded) {
                g_roomsWhenReady = true;
                break;
            }
            RoomsClearNotice();
            RoomsScreenShow();
            break;
        case HubAction::Settings:
            SettingsScreenShow();
            break;
        case HubAction::BattleLog:
            BattleLogShow();
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
            if (!UpdateRestartGame(g_window, &error)) g_restartError = error;
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
