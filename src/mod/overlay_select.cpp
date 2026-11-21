#include "mod/overlay_internal.h"

namespace sfv::mod::overlay {

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
    s.rows[2].value = variants < 0 ? "checking..." : FlowVariantLabel(code, pick.variation);
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

int SelectStageFor(const NetView& v, int* setting) {
    const int count = static_cast<int>(g_tables.stages.size());
    *setting = 0;
    if (v.host) {
        if (g_choices.stage < 2 || g_choices.stage - 2 >= count) return count;
        *setting = g_choices.setting;
        return g_choices.stage - 2;
    }
    if (v.stageRule == lobby::kRandomStage || v.stageRule == lobby::kAnyStage) return count;
    const std::string code = FlowStageCode(v.stageRule);
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

void StepRow(Choices& c, int row, int delta) {
    auto cycle = [delta](int at, int n) { return n <= 0 ? 0 : ((at + delta) % n + n) % n; };
    switch (row) {
        case 0: {
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

void ApplySelectInput(const SelectInput& in, const NetView& v) {
    Choices& c = g_choices;
    if (in.leave) {
        Log("overlay: left from the select screen");
        Act([] { NetLeave(); });
        return;
    }
    if (v.spectator) return;
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
        if (in.ready && IsEleven(c.chara)) RollEleven(c);
    }
    SyncChoices(v);
    if (v.host && in.lockStage) Act([] { NetSetStageLocked(true); });
    if (v.host && in.unlockStage && !ready) Act([] { NetSetStageLocked(false); });
    if (in.ready && !ready) Act([] { NetSetReady(true); });
    if (in.unready && ready) Act([] { NetSetReady(false); });
}

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
        case NetView::Phase::Ended:
        case NetView::Phase::Idle: return;
    }
    DrawStatusNote(HubFonts{g_fontHuge, g_fontBig, g_fontBody, g_fontBold}, text, ImGui::ColorConvertFloat4ToU32(color));
}

} // namespace sfv::mod::overlay
