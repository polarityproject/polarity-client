// The room screen: its input, the title line, the tables, players and chat, and the modals over them.

#include "mod/room_internal.h"

#include "core/text.h"
#include "mod/log.h"
#include "mod/ui_sound.h"
#include "mod/weyve_service.h"

#include <imgui_internal.h>

#include <algorithm>

namespace sfv::mod {

using namespace roomui;
using namespace ui;
using keeper::TablePhase;

namespace {

int g_suggestedDelay = 2;

} // namespace

void RoomReset() { g = State{}; }

void RoomSetCardDelay(int frames) { g_suggestedDelay = std::clamp(frames, 0, kMaxCardDelay); }

void RoomSetTableHandoff(const TableHandoff& handoff) { g_handoff = handoff; }

void RoomSetCardPick(const CardPick& pick) { g_cardPick = pick; }

void RoomToggleMuteForTest(uint32_t id) {
    if (!g.muted.erase(id)) g.muted.insert(id);
    Log("room screen: member %u %s (this player's view only)", id, g.muted.count(id) ? "muted" : "unmuted");
}

bool RoomMemberMuted(uint32_t id) { return g.muted.count(id) != 0; }

void RoomSetSkillNames(std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> names) { g_skillNames = std::move(names); }

void RoomSetCharaNames(std::vector<std::pair<std::string, std::string>> codeToName) {
    g_names.clear();
    for (auto& [code, name] : codeToName) g_names[code] = name;
}

void RoomDrawScreen(const HubFonts& f, const RoomsView& v, bool takeInput, RoomScreenInput* in) {
    *in = RoomScreenInput{};
    if (!f.display || !f.strong || !f.body) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    Backdrop(d);
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    const bool paused = !v.keeper;
    const int tables = std::clamp(v.tables, 1, keeper::kMaxTables);
    g.table = std::clamp(g.table, 0, tables - 1);
    const std::vector<RoomMember> players = PlayerOrder(v);
    g.player = std::clamp(g.player, 0, std::max(0, static_cast<int>(players.size()) - 1));

    int callTable = -1, mySeat = -1;
    for (int t = 0; t < static_cast<int>(v.tableViews.size()); t++) {
        const keeper::TableState& s = v.tableViews[t].s;
        const int seat = s.seat[0] == v.self ? 0 : s.seat[1] == v.self ? 1
                                                                       : -1;
        if (seat >= 0 && s.phase == TablePhase::Call && !paused) {
            callTable = t;
            mySeat = seat;
        }
    }
    int handoffTable = -1;
    for (int t = 0; t < static_cast<int>(v.tableViews.size()) && callTable < 0; t++) {
        const keeper::TableState& s = v.tableViews[t].s;
        if ((s.seat[0] == v.self || s.seat[1] == v.self) && (s.phase == TablePhase::Connecting || s.phase == TablePhase::Playing)) {
            handoffTable = t;
        }
    }
    std::vector<HandoffStep> steps;
    double stepSeconds = 0;
    if (handoffTable >= 0) {
        steps = HandoffSteps(f, v, v.tableViews[handoffTable].s, g_handoff);
        int now = static_cast<int>(steps.size()) - 1;
        for (int i = 0; i < static_cast<int>(steps.size()); i++) {
            if (steps[i].state == HandoffStep::InProgress) {
                now = i;
                break;
            }
        }
        if (handoffTable != g.handoffTable || now != g.handoffStep) g.handoffSince = ImGui::GetTime();
        g.handoffStep = now;
        stepSeconds = ImGui::GetTime() - g.handoffSince;
        g.typing = false;
    }
    g.handoffTable = handoffTable;
    const bool canCancel = handoffTable >= 0 && !g_handoff.jumping && g_handoff.played == 0;
    const uint32_t callNumber = callTable >= 0 ? v.tableViews[callTable].s.call : 0;
    if (callTable != g.cardTable || callNumber != g.cardCall) {
        if (callTable != g.cardTable) g.cardDelay = g_suggestedDelay;
        if (callTable >= 0) UiSoundPlay(UiSound::YourTurn);
        g.cardTick = -1;
        g.cardTable = callTable;
        g.cardCall = callNumber;
        g.cardSince = ImGui::GetTime();
        g.cardReady = callTable >= 0 && v.tableViews[callTable].s.ready[mySeat];
        g.cardRow = g_cardPick.editable ? kCardVSkill : kCardDelay;
        g.cardVs = g_cardPick.vskill;
        g.cardVt = g_cardPick.vtrigger;
        g.cardMouseRow = -1;
        g.oppVs = g.oppVt = 0;
        g.oppChangedAt = -10;
        if (callTable >= 0) g.typing = false;
    }

    const TableView* focused = g.table < static_cast<int>(v.tableViews.size()) ? &v.tableViews[g.table] : nullptr;
    const keeper::TableState fs = focused ? focused->s : keeper::TableState{};
    const bool atFocused = fs.seat[0] == v.self || fs.seat[1] == v.self;
    const auto inQueue = focused ? std::find(focused->queue.begin(), focused->queue.end(), v.self) : std::vector<uint32_t>::const_iterator{};
    const bool queuedHere = focused && inQueue != focused->queue.end();
    bool waiting = false, playing = false;
    for (const TableView& t : v.tableViews) {
        const bool seated = t.s.seat[0] == v.self || t.s.seat[1] == v.self;
        if (seated || std::find(t.queue.begin(), t.queue.end(), v.self) != t.queue.end()) waiting = true;
        if (seated && t.s.phase != TablePhase::Open && t.s.phase != TablePhase::After) playing = true;
    }
    waiting = waiting && !playing && handoffTable < 0;

    if (g.typing && takeInput) ui::g_textEntry = true;
    bool send = false;
    if (g.cardTable >= 0) {
        if (!g.cardReady) {
            const auto usable = [](int r) { return r == kCardDelay || g_cardPick.editable; };
            if (!usable(g.cardRow)) g.cardRow = kCardDelay;
            if (Up()) {
                for (int r = g.cardRow - 1; r >= 0; r--) {
                    if (usable(r)) {
                        g.cardRow = r;
                        break;
                    }
                }
            }
            if (Down()) g.cardRow = std::min(kCardRows - 1, g.cardRow + 1);
            int dir = Left() ? -1 : Right() ? 1
                                            : 0;
            if (g.cardMouseRow >= 0) {
                g.cardRow = g.cardMouseRow;
                dir = g.cardMouseDir;
                g.cardMouseRow = -1;
            }
            if (dir && g.cardRow == kCardDelay) g.cardDelay = std::clamp(g.cardDelay + dir, 0, kMaxCardDelay);
            else if (dir) {
                const bool trigger = g.cardRow == kCardVTrigger;
                int& value = trigger ? g.cardVt : g.cardVs;
                const int n = SkillCount(g_cardPick.chara, trigger);
                value = (value - 1 + dir + n) % n + 1;
                (trigger ? in->vtrigger : in->vskill) = value;
            }
            if (Confirm()) {
                g.cardReady = true;
                Request(in, "ready", {std::to_string(g.cardTable)});
                in->delay = g.cardDelay;
                if (!v.tableNotice.empty()) in->clearNotice = true;
            } else if (Back()) {
                Request(in, "decline", {std::to_string(g.cardTable)});
            }
        }
    } else if (handoffTable >= 0) {
        if (canCancel && Back()) in->cancelHandoff = true;
    } else if (g.modal == Modal::Leaving || g.modal == Modal::Remove) {
        if (Left()) g.choice = 0;
        if (Right()) g.choice = 1;
        if (Confirm()) {
            if (g.choice == 0 && g.modal == Modal::Leaving) in->leave = true;
            else if (g.choice == 0) Request(in, "kick", {std::to_string(g.removing)});
            g.modal = Modal::None;
        } else if (Back()) g.modal = Modal::None;
    } else if (g.modal == Modal::Rules) {
        RoomStepTableRules(g.rules);
        if (Confirm() || Pressed({ImGuiKey_GamepadStart}, false)) {
            for (int t = 0; t < tables; t++) {
                Rules now;
                const std::string current = t < static_cast<int>(v.tableRules.size()) ? v.tableRules[t] : "";
                if (!(ParseRules(current, &now) && now == g.rules.rules[t])) g.rulesPending.push_back(t);
            }
            g.modal = Modal::None;
        } else if (Back()) g.modal = Modal::None;
    } else if (g.typing) {
        for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
            if (c < 32 || c == 127) continue;
            char utf8[5] = {};
            ImTextCharToUtf8(utf8, c);
            if (keeper::Utf8Length(g.text) < keeper::kChatMaxChars) g.text += utf8;
        }
        if (Pressed({ImGuiKey_Backspace}) && !g.text.empty()) {
            size_t n = g.text.size() - 1;
            while (n > 0 && (static_cast<unsigned char>(g.text[n]) & 0xC0) == 0x80) n--;
            g.text.erase(n);
        }
        if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false)) send = true;
        if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}, false)) g.typing = false;
    } else {
        static bool backHeld = false, usedY = false;
        const bool back = g_inputOn && ImGui::IsKeyDown(ImGuiKey_GamepadBack);
        const bool y = Pressed({ImGuiKey_GamepadFaceUp}, false);
        if (back && y) {
            g.typing = true;
            usedY = true;
        }
        if (backHeld && !back) {
            if (!usedY) {
                ImGui::SetClipboardText(v.room.c_str());
                in->copiedCode = true;
            }
            usedY = false;
        }
        backHeld = back;
        if (Pressed({ImGuiKey_GamepadR1}, false)) g.onPlayers = !g.onPlayers;
        if (g.onPlayers) {
            if (Up()) g.player = std::max(0, g.player - 1);
            if (Down()) g.player = std::min(static_cast<int>(players.size()) - 1, g.player + 1);
            const uint32_t who = players.empty() ? 0 : players[g.player].id;
            if (Confirm() && who && who != v.self) {
                if (!g.muted.erase(who)) g.muted.insert(who);
                Log("room screen: member %u %s (this player's view only)", who, g.muted.count(who) ? "muted" : "unmuted");
            }
            if (Pressed({ImGuiKey_GamepadFaceLeft}, false) && who && who != v.self && v.owner == v.self && !paused) {
                g.removing = who;
                g.modal = Modal::Remove;
                g.choice = 0;
            }
            if (Back()) g.onPlayers = false;
        } else {
            if (Up()) g.table = std::max(0, g.table - 1);
            if (Down()) g.table = std::min(tables - 1, g.table + 1);
            if (fs.phase == TablePhase::Open && !fs.seat[0] && !fs.seat[1]) {
                if (Left()) g.seatSide = 0;
                if (Right()) g.seatSide = 1;
            }
            const bool enter = Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false);
            const bool confirm = Confirm() && !enter;
            if (!v.tableNotice.empty() && confirm) {
                in->clearNotice = true;
            } else if (!paused && confirm) {
                const std::string t = std::to_string(g.table);
                if (fs.phase == TablePhase::Open && atFocused) Request(in, "stand");
                else if (fs.phase == TablePhase::Open && (!fs.seat[0] || !fs.seat[1])) {
                    const int seat = !fs.seat[0] && !fs.seat[1] ? g.seatSide : !fs.seat[0] ? 0
                                                                                           : 1;
                    Request(in, "seat", {t, std::to_string(seat)});
                } else if (queuedHere) Request(in, "unqueue");
                else if (!atFocused) Request(in, "queue", {t});
            }
            if (y && !back) in->myCharacter = true;
            if (Pressed({ImGuiKey_GamepadL1}, false) && waiting && !paused) in->training = true;
            if (Pressed({ImGuiKey_GamepadFaceLeft}, false) && !paused && !atFocused) in->watch = v.watching == g.table ? -1 : g.table;
            if (Pressed({ImGuiKey_GamepadStart}, false) && v.owner == v.self && !paused) {
                g.modal = Modal::Rules;
                g.rules.table = g.table;
                g.rules.row = 0;
                g.rules.rules.assign(tables, Rules{});
                for (int t = 0; t < tables && t < static_cast<int>(v.tableRules.size()); t++) ParseRules(v.tableRules[t], &g.rules.rules[t]);
            }
            if (enter) g.typing = true;
            else if (Back()) g.modal = Modal::Leaving, g.choice = 0;
        }
        if (ImGui::GetIO().KeyCtrl && Pressed({ImGuiKey_C}, false)) {
            ImGui::SetClipboardText(v.room.c_str());
            in->copiedCode = true;
        }
    }
    if (in->request.empty() && !paused && !g.rulesPending.empty() && g.rulesPending.front() < static_cast<int>(g.rules.rules.size())) {
        const int t = g.rulesPending.front();
        g.rulesPending.erase(g.rulesPending.begin());
        Request(in, "rules", {std::to_string(t), FormatRules(g.rules.rules[t])});
    }
    if (g.modal == Modal::None && !g.typing && g.cardTable < 0) WheelList(g_v.P(48, 80), g_v.P(868, 832), tables, kTablesInView, &g.scroll, &g.table);
    if (g.table < g.scroll) g.scroll = static_cast<float>(g.table);
    if (g.table > g.scroll + kTablesInView - 1) g.scroll = static_cast<float>(g.table - kTablesInView + 1);

    const bool hideCode = RoomsScreenStreamerMode();
    const std::string code = WeyveService::DisplayCode(v.room);
    const char* access = TR(v.isPublic ? Str::common_public : Str::common_private);
    const float codeW = hideCode ? RoomsScreenDrawHiddenCode(nullptr, 0, 0, 20, 0) : TextW(f.strong, 20, code);
    const float lineW = (paused ? 0 : TextW(f.strong, 20, access) + 16) + TextW(f.body, 20, TR(Str::room_header_code)) + 6 + codeW + 20 + 38 +
                        TextW(f.body, 20, TR(Str::room_header_copy));
    const std::string title = RoomsScreenTitleText(v.title, NameOf(v, v.owner));
    float x = 48 + RoomsScreenFitText(d, f.display, 40, 26, 1552 - 48 - 30 - lineW, g_v.P(48, 22), Rgba(255, 255, 255), title, 0, true) + 30;
    if (!paused) {
        StageText(d, f.strong, 20, x, 39, v.isPublic ? kGreen : kSoft, access);
        x += TextW(f.strong, 20, access) + 16;
    }
    StageText(d, f.body, 20, x, 39, kCream, TR(Str::room_header_code));
    x += TextW(f.body, 20, TR(Str::room_header_code)) + 6;
    if (hideCode) x += RoomsScreenDrawHiddenCode(d, x, 39, 20, kGold) + 20;
    else {
        StageText(d, f.strong, 20, x, 39, kGold, code);
        x += TextW(f.strong, 20, code) + 20;
    }
    Glyph(d, "button_back", g_v.P(x, 33), g_v.P(x + 32, 65));
    StageText(d, f.body, 20, x + 38, 39, kCream, TR(Str::room_header_copy));
    if (g.modal == Modal::None && Clicked() && MouseIn(g_v.P(x, 32), g_v.P(x + 38 + TextW(f.body, 20, TR(Str::room_header_copy)), 68))) {
        ImGui::SetClipboardText(v.room.c_str());
        in->copiedCode = true;
    }

    d->PushClipRect(g_v.P(30, 72), g_v.P(886, 840), true);
    for (int t = 0; t < tables; t++) {
        const float ty = 80 + (t - g.scroll) * kTablePitch;
        if (ty > 832 || ty + kTableH < 80) continue;
        Table(d, f, v, ty, t, t == g.table && !g.onPlayers, paused);
        if (g.modal == Modal::None && !g.typing && g.cardTable < 0 && Clicked() && MouseIn(g_v.P(48, ty), g_v.P(868, ty + kTableH))) {
            g.table = t;
            g.onPlayers = false;
        }
    }
    d->PopClipRect();
    ScrollBar(d, 874, 84, 830, g.scroll, kTablesInView, static_cast<float>(tables), true);

    Players(d, f, v);
    bool clickedBox = false, clickedSend = false;
    Chat(d, f, v, &clickedBox, &clickedSend);
    if (g.modal == Modal::None && g.cardTable < 0) {
        if (clickedBox) g.typing = true;
        if (clickedSend && g.typing) send = true;
    }
    if (send) {
        if (!g.text.empty()) in->chat = g.text;
        g.text.clear();
        g.typing = false;
    }

    int clicked = -1;
    std::vector<std::pair<const char*, std::string>> prompts;
    std::string lead, text;
    ImU32 leadCol = kGold;
    int myTable = -1;
    for (int t = 0; t < static_cast<int>(v.tableViews.size()); t++) {
        const keeper::TableState& s = v.tableViews[t].s;
        if (s.seat[0] == v.self || s.seat[1] == v.self || (s.phase == TablePhase::After && (s.lastWinner == v.self || s.lastLoser == v.self))) myTable = t;
    }
    const keeper::TableState* ms = myTable >= 0 ? &v.tableViews[myTable].s : nullptr;
    if (g.cardTable >= 0) {
        prompts = g.cardReady ? std::vector<std::pair<const char*, std::string>>{}
                              : std::vector<std::pair<const char*, std::string>>{{"button_a", TR(Str::common_prompt_ready)}, {"button_b", TR(Str::room_bar_leave_queue)}};
        text = "";
        if (!v.tableNotice.empty() && !g.cardReady) {
            lead = TR(Str::room_bar_no_match);
            leadCol = kRed;
            text = v.tableNotice;
        }
    } else if (g.modal == Modal::Leaving) {
        prompts = {{"button_a", TR(Str::common_prompt_select)}, {"button_b", TR(Str::common_prompt_stay)}};
    } else if (g.modal == Modal::Remove) {
        prompts = {{"button_a", TR(Str::common_prompt_select)}, {"button_b", TR(Str::common_prompt_cancel)}};
    } else if (g.modal == Modal::Rules) {
        text = "";
        prompts = {{"button_a", TR(Str::common_prompt_done)}, {"button_b", TR(Str::common_prompt_cancel)}};
    } else if (g.typing) {
        text = TR(Str::room_bar_typing);
    } else if (paused) {
        lead = TR(Str::room_bar_paused);
        text = TR(Str::room_bar_paused_text);
        prompts = {{"button_b", TR(Str::common_prompt_leave)}};
    } else if (handoffTable >= 0) {
        const keeper::TableState& s = v.tableViews[handoffTable].s;
        lead = TRF(Str::room_bar_table, handoffTable + 1);
        text = g_handoff.jumping ? std::string(TR(Str::room_bar_loading)) : TRF(Str::room_bar_getting_ready, NameOf(v, s.seat[s.seat[0] == v.self ? 1 : 0]).c_str());
        if (canCancel) prompts = {{"button_b", TR(Str::common_prompt_cancel)}};
    } else if (ms && ms->phase == TablePhase::After && (ms->lastWinner == v.self || ms->lastLoser == v.self)) {
        const bool won = ms->lastWinner == v.self;
        Rules rules;
        const bool early = ParseRules(v.tableViews[myTable].rules, &rules) && ms->lastScore[0] < rules.firstTo;
        lead = TR(early ? Str::room_bar_set_ended : won ? Str::room_bar_won
                                                        : Str::room_bar_lost);
        leadCol = early ? kSoft : won ? kGreen
                                      : kSoft;
        const int mine = won ? ms->lastScore[0] : ms->lastScore[1], theirs = won ? ms->lastScore[1] : ms->lastScore[0];
        const std::vector<uint32_t>& q = v.tableViews[myTable].queue;
        const auto at = std::find(q.begin(), q.end(), v.self);
        if (ms->seat[0] == v.self || ms->seat[1] == v.self) text = TRF(Str::room_bar_result_stay, mine, theirs, myTable + 1);
        else if (at != q.end()) text = TRF(Str::room_bar_result_queue, mine, theirs, Ordinal(static_cast<int>(at - q.begin()) + 1).c_str());
        else text = TRF(Str::room_bar_result_room, mine, theirs);
    } else if (!v.tableNotice.empty()) {
        lead = TR(Str::room_bar_no_match);
        leadCol = kRed;
        text = v.tableNotice;
        prompts = {{"button_a", TR(Str::common_prompt_ok)}};
    } else if (g.onPlayers) {
        const uint32_t who = players.empty() ? 0 : players[g.player].id;
        text = who == v.self ? TR(Str::room_bar_thats_you) : "";
        prompts = {};
        if (who != v.self) prompts.push_back({"button_a", TR(g.muted.count(who) ? Str::room_bar_unmute : Str::room_bar_mute)});
        if (who != v.self && v.owner == v.self) prompts.push_back({"button_x", TR(Str::room_bar_remove)});
        prompts.push_back({"button_b", TR(Str::room_bar_tables)});
    } else {
        const RoomPlace place = RoomsPlaceOf(v, v.self);
        if (v.members.size() <= 1) {
            lead = TR(Str::room_bar_open);
            text = TR(Str::room_bar_open_text);
        } else if (v.watching >= 0 && place.kind == RoomPlace::Kind::Watching) {
            lead = Upper(RoomsPlaceText(place));
            text = TR(Str::room_bar_watching_text);
        } else if (place.kind != RoomPlace::Kind::Idle) {
            lead = Upper(RoomsPlaceText(place));
        } else {
            text = TR(Str::room_bar_pick_table);
        }
        if (fs.phase == TablePhase::Open && atFocused) prompts.push_back({"button_a", TR(Str::room_bar_stand_up)});
        else if (fs.phase == TablePhase::Open && (!fs.seat[0] || !fs.seat[1])) {
            const int seat = !fs.seat[0] && !fs.seat[1] ? g.seatSide : !fs.seat[0] ? 0
                                                                                   : 1;
            prompts.push_back({"button_a", TR(seat ? Str::room_bar_sit_p2 : Str::room_bar_sit_p1)});
        } else if (queuedHere) prompts.push_back({"button_a", TR(Str::room_bar_leave_the_queue)});
        else if (!atFocused) prompts.push_back({"button_a", TR(Str::room_bar_join_queue)});
        if (!atFocused) prompts.push_back({"button_x", TR(v.watching == g.table ? Str::room_bar_stop_watching : Str::room_bar_watch)});
        prompts.push_back({"button_y", TR(Str::room_bar_my_character)});
        if (waiting && !paused) prompts.push_back({"button_lb", TR(Str::room_bar_training)});
        if (v.owner == v.self) prompts.push_back({"button_start", TR(Str::room_bar_table_rules)});
        if (v.members.size() > 1) prompts.push_back({"button_rb", TR(Str::room_bar_players)});
        prompts.push_back({"button_b", TR(Str::common_prompt_leave)});
    }
    const bool modal = g.cardTable >= 0 || handoffTable >= 0 || g.modal != Modal::None;
    PromptLine(d, f.body, f.bold ? f.bold : f.strong, modal ? std::string() : lead, leadCol, modal ? std::string() : text, modal ? PromptList{} : prompts,
               &clicked);
    int pressed = -1;
    if (g.cardTable >= 0) Card(d, f, v, g.cardTable, prompts, lead.empty() ? text : lead + "  " + text, lead.empty() ? kSoft : leadCol, &clicked);
    else if (handoffTable >= 0) HandoffModal(d, f, v, handoffTable, steps, stepSeconds, prompts, &clicked);
    else if (g.modal == Modal::Leaving) {
        Dialog(d, f, RoomsScreenStreamerMode() ? std::string(TR(Str::room_dialog_room)) : TRF(Str::room_dialog_room_code, WeyveService::DisplayCode(v.room).c_str()),
               TR(Str::room_dialog_leave), LeaveText(v), false, TR(Str::common_prompt_leave), TR(Str::common_prompt_stay), prompts, &clicked, &pressed);
    } else if (g.modal == Modal::Remove) {
        Dialog(d, f, TR(Str::room_dialog_owner), TRF(Str::room_dialog_remove, NameOf(v, g.removing).c_str()), TR(Str::room_dialog_remove_text), true,
               TR(Str::room_bar_remove), TR(Str::common_prompt_cancel), prompts, &clicked, &pressed);
    } else if (g.modal == Modal::Rules) RulesModal(f, v, prompts, &clicked);
    if (pressed >= 0 && g_inputOn) {
        if (pressed == 0 && g.modal == Modal::Leaving) in->leave = true;
        else if (pressed == 0) Request(in, "kick", {std::to_string(g.removing)});
        g.modal = Modal::None;
    }
    (void)mySeat;
    g_inputOn = false;
    d->PopClipRect();
}

} // namespace sfv::mod
