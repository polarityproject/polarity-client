#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

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
    if (!g_choices.initialised) return;
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
    if (card.place == "Idle") card.place = "In the room";
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
        SaveMyCharacter();
        return CloseMyCharacter(false, "saved");
    }
}

void DrawMyCharacter(const RoomsView& rooms, bool takeInput) {
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

} // namespace sfv::mod::overlay
