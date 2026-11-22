// The room screen's modals: the your-turn card, the connecting steps, the table rules.

#include "mod/room_internal.h"

#include "core/text.h"
#include "mod/log.h"
#include "mod/ui_sound.h"

#include <algorithm>
#include <cmath>

namespace sfv::mod {

using namespace roomui;
using namespace ui;

namespace roomui {

std::string LeaveText(const RoomsView& v) {
    std::string text;
    for (const TableView& t : v.tableViews) {
        const auto at = std::find(t.queue.begin(), t.queue.end(), v.self);
        if (at != t.queue.end() || t.s.seat[0] == v.self || t.s.seat[1] == v.self) {
            text = TRF(Str::room_leave_lose_place, static_cast<int>(&t - v.tableViews.data() + 1)) + " ";
        }
    }
    if (v.owner == v.self) {
        if (v.members.size() <= 1) return text + TR(Str::room_leave_last_one);
        for (const RoomMember& m : v.members) {
            if (m.id != v.self) return text + TRF(Str::room_leave_new_owner, m.name.c_str());
        }
    }
    return text.empty() ? std::string(TR(Str::room_leave_rejoin)) : text;
}

void Card(ImDrawList* d, const HubFonts& f, const RoomsView& v, int t, const PromptList& prompts, const std::string& hint, ImU32 hintCol, int* clicked) {
    const keeper::TableState& s = v.tableViews[t].s;
    ImFont* bold = f.bold ? f.bold : f.strong;
    const float x0 = 380, w = 840, h = 616, y0 = 142, x1 = x0 + w;
    ModalSpec m;
    m.x0 = x0, m.y0 = y0, m.w = w, m.h = h;
    m.kicker = TRF(Str::room_card_kicker, t + 1);
    Rules rules;
    if (ParseRules(v.tableViews[t].rules, &rules)) m.aside = RoomsRulesText(rules), m.asideCol = kGold;
    m.hint = hint, m.hintCol = hintCol;
    m.prompts = prompts;
    ui::Modal(d, bold, f.body, m, clicked);
    const int mySeat = s.seat[0] == v.self ? 0 : 1;
    if (const RoomMember* other = Member(v, s.seat[1 - mySeat])) {
        if ((other->vskill != g.oppVs || other->vtrigger != g.oppVt) && g.oppVs > 0 && other->vskill > 0) g.oppChangedAt = ImGui::GetTime();
        g.oppVs = other->vskill;
        g.oppVt = other->vtrigger;
    }
    for (int i = 0; i < 2; i++) {
        const RoomMember* mm = Member(v, s.seat[i]);
        const float sx = i ? x1 - 28 - 330 : x0 + 28;
        const float align = i ? 1.0f : 0.0f, ax = i ? sx + 330 : sx;
        Seat(d, f, sx, y0 + 66, mm, s.ready[i], false, i, false);
        Text(d, bold, g_v.L(15), g_v.P(ax, y0 + 192), kSoft,
             TR(i ? (i == mySeat ? Str::room_card_p2_you : Str::room_card_p2) : i == mySeat ? Str::room_card_p1_you_hosts
                                                                                            : Str::room_card_p1_hosts),
             align);
        const bool me = i == mySeat;
        const std::string code = me ? g_cardPick.chara : mm ? mm->chara
                                                            : std::string();
        const int vs = me ? (g_cardPick.editable ? g.cardVs : -1) : mm ? mm->vskill
                                                                       : 0;
        const int vt = me ? (g_cardPick.editable ? g.cardVt : -1) : mm ? mm->vtrigger
                                                                       : 0;
        const bool fresh = !me && ImGui::GetTime() - g.oppChangedAt < 1.5;
        for (int k = 0; k < 2; k++) {
            const float ly = y0 + 222 + k * 30;
            const char* kicker = TR(k ? Str::room_card_vtrigger : Str::room_card_vskill);
            const int value = k ? vt : vs;
            const std::string text = value < 0 ? std::string(TR(Str::common_random)) : value == 0 ? std::string(TR(Str::room_card_none))
                                                                                                  : SkillLabel(code, k == 1, value);
            const float kw = TextW(bold, 15, kicker) + 10;
            const float tw = TextW(f.strong, 18, text);
            const float tx = i ? ax - tw : ax + kw;
            Text(d, bold, g_v.L(15), g_v.P(i ? tx - kw : ax, ly + 3), kGold, kicker);
            Text(d, f.strong, g_v.L(18), g_v.P(tx, ly), fresh ? kGold : kCream, text);
        }
    }
    StageText(d, f.display, 36, x0 + w / 2, y0 + 104, Rgba(255, 255, 255), TR(Str::common_vs), 0.5f);
    if (const RoomMember* other = Member(v, s.seat[1 - mySeat]); other && other->pingMs >= 0) {
        const std::string ms = TRF(Str::common_ms, other->pingMs);
        const ImU32 col = PingColor(static_cast<unsigned>(other->pingMs));
        Text(d, bold, g_v.L(14), g_v.P(x0 + w / 2, y0 + 150), kSoft, TR(Str::room_card_ping), 0.5f);
        StageText(d, f.strong, 22, x0 + w / 2, y0 + 168, col, ms, 0.5f);
        if (other->relayed) IconCloud(d, g_v.P(x0 + w / 2, y0 + 208), g_v.L(1.7f), col);
    }
    const double total = std::max(1.0, (s.callSeconds ? s.callSeconds : kKeeperCallSeconds) - kCardMarginSeconds);
    const double left = std::clamp(total - (RoomsClockMs() - v.tableViews[t].callSeenMs) / 1000.0, 0.0, total);
    const float ty = y0 + 296;
    const int second = static_cast<int>(std::ceil(left));
    if (!g.cardReady && !s.ready[mySeat] && second > 0 && second <= 5 && second != g.cardTick) UiSoundPlay(UiSound::Countdown);
    g.cardTick = second;
    if (g.cardReady || s.ready[mySeat]) {
        Text(d, f.display, g_v.L(28), g_v.P(x0 + w / 2, ty + 40), kGreen, TR(Str::common_ready), 0.5f);
        Text(d, f.body, g_v.L(19), g_v.P(x0 + w / 2, ty + 86), kCream, TRF(Str::room_card_waiting_for, NameOf(v, s.seat[1 - mySeat]).c_str()), 0.5f);
        return;
    }
    Text(d, f.display, g_v.L(26), g_v.P(x0 + w / 2, ty), Rgba(255, 255, 255),
         left > 0 ? TRF(Str::room_card_ready_in, static_cast<int>(left + 0.99)) : std::string(TR(Str::room_card_out_of_time)), 0.5f);
    d->AddRectFilled(g_v.P(x0 + 28, ty + 40), g_v.P(x1 - 28, ty + 44), Rgba(255, 255, 255, 31));
    d->AddRectFilled(g_v.P(x0 + 28, ty + 40), g_v.P(x0 + 28 + (w - 56) * static_cast<float>(left / total), ty + 44), kGold);
    const char* const kRows[] = {TR(Str::room_card_row_vskill), TR(Str::room_card_row_vtrigger), TR(Str::room_card_row_delay)};
    const float rx0 = x0 + 160, rx1 = x1 - 160, vx = rx1 - 150;
    for (int r = 0; r < kCardRows; r++) {
        const float ry = ty + 62 + r * 56;
        const bool on = r == g.cardRow, usable = r == kCardDelay || g_cardPick.editable;
        Control(d, rx0, ry, rx1, ry + 50, on);
        const ImU32 col = on ? look::kInk : usable ? kCream
                                                   : Rgba(244, 241, 232, 110);
        Text(d, on ? bold : f.strong, g_v.L(19), g_v.P(rx0 + 22, ry + 12), col, kRows[r]);
        std::string value = r == kCardDelay        ? TRF(g.cardDelay == 1 ? Str::common_frame : Str::common_frames, g.cardDelay)
                            : !g_cardPick.editable ? std::string(TR(Str::common_random))
                                                   : SkillLabel(g_cardPick.chara, r == kCardVTrigger, r == kCardVTrigger ? g.cardVt : g.cardVs);
        Text(d, on ? bold : f.strong, g_v.L(19), g_v.P(vx, ry + 12), col, Ellipsize(bold, 19, value, 230), 0.5f);
        if (on && usable) {
            d->AddTriangleFilled(g_v.P(vx - 128, ry + 19), g_v.P(vx - 128, ry + 31), g_v.P(vx - 138, ry + 25), look::kInk);
            d->AddTriangleFilled(g_v.P(vx + 128, ry + 19), g_v.P(vx + 138, ry + 25), g_v.P(vx + 128, ry + 31), look::kInk);
        }
        if (usable && Clicked() && MouseIn(g_v.P(rx0, ry), g_v.P(rx1, ry + 50))) {
            g.cardMouseRow = r;
            g.cardMouseDir = on ? (MouseIn(g_v.P(rx0, ry), g_v.P(vx, ry + 50)) ? -1 : 1) : 0;
        }
    }
    Text(d, f.body, g_v.L(17), g_v.P(x0 + w / 2, ty + 62 + 3 * 56 + 6), kSoft, TR(Str::room_card_time_out), 0.5f);
}

} // namespace roomui

namespace {

} // namespace

namespace roomui {

std::vector<HandoffStep> HandoffSteps(const HubFonts& f, const RoomsView& v, const keeper::TableState& s, const TableHandoff& h) {
    const bool host = s.seat[0] == v.self;
    const std::string other = Ellipsize(f.body, 17, NameOf(v, s.seat[host ? 1 : 0]), 190);
    const bool room = h.jumping || (h.session && (!host || h.roomOpen));
    const bool linked = h.jumping || h.linked;
    const bool loaded = h.jumping || (h.preloaded[0] && h.preloaded[1]);
    std::vector<HandoffStep> steps;
    steps.push_back({TR(Str::room_handoff_both_ready), HandoffStep::Done});
    steps.push_back({room ? std::string(TR(Str::room_handoff_set_up)) : host ? std::string(TR(Str::room_handoff_setting_up))
                                                                             : TRF(Str::room_handoff_waiting_for, other.c_str()),
                     room ? HandoffStep::Done : HandoffStep::InProgress});
    const std::string connected = h.pingMs ? TRF(Str::room_handoff_connected_ms, h.pingMs) : std::string(TR(Str::room_handoff_connected));
    steps.push_back({linked ? connected : host ? TRF(Str::room_handoff_waiting_join, other.c_str())
                                               : TRF(Str::room_handoff_connecting, other.c_str()),
                     linked ? HandoffStep::Done : room ? HandoffStep::InProgress
                                                       : HandoffStep::ToCome});
    const bool mine = h.preloaded[host ? 0 : 1];
    steps.push_back({loaded ? std::string(TR(Str::room_handoff_loaded)) : mine ? TRF(Str::room_handoff_waiting_game, other.c_str())
                                                                               : std::string(TR(Str::room_handoff_loading)),
                     loaded ? HandoffStep::Done : linked ? HandoffStep::InProgress
                                                         : HandoffStep::ToCome});
    steps.push_back({TR(Str::room_handoff_starting), h.jumping ? HandoffStep::InProgress : HandoffStep::ToCome});
    return steps;
}

void HandoffModal(ImDrawList* d, const HubFonts& f, const RoomsView& v, int t, const std::vector<HandoffStep>& steps, double stepSeconds, const PromptList& prompts, int* clicked) {
    const keeper::TableState& s = v.tableViews[t].s;
    const uint32_t otherId = s.seat[s.seat[0] == v.self ? 1 : 0];
    ImFont* bold = f.bold ? f.bold : f.strong;
    const float x0 = 480, w = 640, h = 404, y0 = 220, x1 = x0 + w;
    ModalSpec m;
    m.x0 = x0, m.y0 = y0, m.w = w, m.h = h;
    m.kicker = g_handoff.played > 0 ? TRF(Str::room_handoff_kicker_game, t + 1, g_handoff.played + 1) : TRF(Str::room_handoff_kicker_turn, t + 1);
    Rules rules;
    if (ParseRules(v.tableViews[t].rules, &rules)) m.hint = RoomsRulesText(rules);
    m.prompts = prompts;
    ui::Modal(d, bold, f.body, m, clicked);
    float tx = x0 + 28;
    const RoomMember* other = Member(v, otherId);
    if (const TextureView* art = other && !other->chara.empty() ? TextureSized(CharaTex(other->chara, "PartCharacter"), g_v.L(200)) : nullptr) {
        Image(d, art, g_v.P(x0 + 28, y0 + 64), g_v.P(x0 + 80, y0 + 138), ImVec2(0.38f, 0.06f), ImVec2(0.62f, 0.42f), Rgba(255, 255, 255));
        d->AddRect(g_v.P(x0 + 28, y0 + 64), g_v.P(x0 + 80, y0 + 138), look::kEdgeControl, 0, 0, std::max(1.0f, g_v.L(1)));
        tx = x0 + 98;
    }
    if (other && g_names.count(other->chara)) Text(d, f.body, g_v.L(14), g_v.P(tx, y0 + 70), kSoft, g_names[other->chara]);
    Text(d, f.display, g_v.L(28), g_v.P(tx, y0 + 92), kCream, TR(Str::room_handoff_vs));
    float nx = tx + TextW(f.display, 28, TR(Str::room_handoff_vs)) + 12;
    if (other && other->country.size() == 2) nx += RoomsScreenDrawFlag(d, f.strong, other->country, nx, y0 + 103, 13) + 10;
    Text(d, f.display, g_v.L(28), g_v.P(nx, y0 + 92), kCream, Ellipsize(f.display, 28, Upper(NameOf(v, otherId)), x1 - 28 - nx));
    float y = y0 + 158;
    for (const HandoffStep& st : steps) {
        const ImVec2 c = g_v.P(x0 + 40, y + 11);
        const ImU32 ring = st.state == HandoffStep::Done ? kGreen : st.state == HandoffStep::InProgress ? kRing
                                                                                                        : look::kEdgeControl;
        d->AddCircle(c, g_v.L(12), ring, 24, g_v.L(2));
        if (st.state == HandoffStep::Done) {
            const ImVec2 tick[] = {g_v.P(x0 + 34, y + 11), g_v.P(x0 + 38.5f, y + 16), g_v.P(x0 + 46.5f, y + 6)};
            d->AddPolyline(tick, 3, kGreen, 0, g_v.L(2.2f));
        } else if (st.state == HandoffStep::InProgress) {
            const float turn = static_cast<float>(ImGui::GetTime() * 4.0);
            d->AddCircleFilled(g_v.P(x0 + 40 + 7 * std::cos(turn), y + 11 + 7 * std::sin(turn)), g_v.L(2.6f), kRing);
        }
        std::string text = st.text;
        if (st.state == HandoffStep::InProgress && stepSeconds >= 3) text = TRF(Str::room_handoff_seconds, text.c_str(), static_cast<int>(stepSeconds));
        Text(d, f.body, g_v.L(17), g_v.P(x0 + 66, y), st.state == HandoffStep::ToCome ? kMuted : kCream, text);
        y += 36;
    }
}

} // namespace roomui

namespace {

const Str kRuleRows[] = {Str::rules_row_first_to, Str::rules_row_rounds, Str::rules_row_timer, Str::rules_row_rotation, Str::rules_row_stays};

std::string RuleValue(const Rules& r, int row) {
    switch (row) {
        case 0: return TRF(r.firstTo == 1 ? Str::rules_value_game : Str::rules_value_games, r.firstTo);
        case 1: return r.bestOf == 1 ? std::string(TR(Str::rules_value_one_round)) : TRF(Str::rules_value_best_of, r.bestOf);
        case 2: return TRF(Str::rules_value_seconds, r.timer);
        case 3:
            return TR(r.rotation == Rotation::WinnerStays  ? Str::rules_value_winner_stays
                      : r.rotation == Rotation::LoserStays ? Str::rules_value_loser_stays
                                                           : Str::rules_value_round_robin);
        default:
            return r.rotation == Rotation::RoundRobin ? std::string("-")
                   : r.staysLimit                     ? TRF(Str::rules_value_sets_in_row, r.staysLimit)
                                                      : std::string(TR(Str::rules_value_no_limit));
    }
}

void StepRule(Rules* r, int row, int dir) {
    auto cycle = [&](int value, std::initializer_list<int> values) {
        std::vector<int> list(values);
        auto at = std::find(list.begin(), list.end(), value);
        int i = at == list.end() ? 0 : static_cast<int>(at - list.begin());
        i = (i + dir + static_cast<int>(list.size())) % static_cast<int>(list.size());
        return list[i];
    };
    switch (row) {
        case 0: r->firstTo = static_cast<uint8_t>(std::clamp(r->firstTo + dir, 1, 20)); break;
        case 1: r->bestOf = static_cast<uint8_t>(cycle(r->bestOf, {3, 5, 1})); break;
        case 2: r->timer = static_cast<uint8_t>(cycle(r->timer, {99, 60, 30})); break;
        case 3: r->rotation = static_cast<Rotation>(cycle(static_cast<int>(r->rotation), {0, 1, 2})); break;
        default:
            if (r->rotation != Rotation::RoundRobin) r->staysLimit = static_cast<uint8_t>(cycle(r->staysLimit, {3, 5, 0}));
            break;
    }
}

} // namespace

namespace roomui {

void RulesModal(const HubFonts& f, const RoomsView& v, const PromptList& prompts, int* clicked) {
    const int t = g.rules.table;
    Rules now;
    const std::string current = t < static_cast<int>(v.tableRules.size()) ? v.tableRules[t] : "";
    const bool changed = !(ParseRules(current, &now) && now == g.rules.rules[t]);
    RoomDrawTableRules(f, g.rules, TR(Str::rules_modal_owner), TR(Str::rules_modal_next_set), changed ? kGold : kSoft, prompts, clicked);
}

void Request(RoomScreenInput* in, const char* kind, std::vector<std::string> args) {
    in->request = kind;
    in->args = std::move(args);
}

} // namespace roomui

void RoomStepTableRules(TableRulesEditor& e) {
    const int tables = static_cast<int>(e.rules.size());
    if (!tables) return;
    e.table = std::clamp(e.table, 0, tables - 1);
    if (Pressed({ImGuiKey_GamepadL1}, false)) e.table = (e.table + tables - 1) % tables;
    if (Pressed({ImGuiKey_GamepadR1}, false)) e.table = (e.table + 1) % tables;
    if (Up()) e.row = std::max(0, e.row - 1);
    if (Down()) e.row = std::min(4, e.row + 1);
    if (Left()) StepRule(&e.rules[e.table], e.row, -1);
    if (Right()) StepRule(&e.rules[e.table], e.row, 1);
}

void RoomDrawTableRules(const HubFonts& f, const TableRulesEditor& e, const char* kicker, const std::string& hint, ImU32 hintCol,
                        const PromptList& prompts, int* clicked) {
    if (e.rules.empty()) return;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    const float x0 = 350, w = 900, h = 470, y0 = 187, x1 = x0 + w;
    const int at = std::clamp(e.table, 0, static_cast<int>(e.rules.size()) - 1);
    const Rules& r = e.rules[at];
    ModalSpec m;
    m.x0 = x0, m.y0 = y0, m.w = w, m.h = h;
    m.kicker = kicker;
    m.hint = hint;
    m.hintCol = hintCol;
    m.prompts = prompts;
    ui::Modal(d, bold, f.body, m, clicked);
    Text(d, f.display, g_v.L(28), g_v.P(x0 + 28, y0 + 64), kCream, TR(Str::rules_modal_title));
    const int tables = static_cast<int>(e.rules.size());
    float tw = 0;
    for (int t = 0; t < tables; t++) tw += TextW(bold, 17, TRF(Str::rules_modal_table, t + 1)) + 28 + 8;
    float tx = x1 - 28 - 30 - 8 - tw - 30;
    Glyph(d, "button_lb", g_v.P(tx, y0 + 66), g_v.P(tx + 30, y0 + 96), IM_COL32_WHITE, false);
    if (Clicked() && MouseIn(g_v.P(tx - 4, y0 + 62), g_v.P(tx + 34, y0 + 100))) ui::ClickPad(ImGuiKey_GamepadL1);
    tx += 38;
    for (int t = 0; t < tables; t++) {
        const std::string label = TRF(Str::rules_modal_table, t + 1);
        const float lw = TextW(bold, 17, label) + 28;
        const bool on = t == at;
        Control(d, tx, y0 + 65, tx + lw, y0 + 97, on);
        Text(d, bold, g_v.L(17), g_v.P(tx + 14, y0 + 70), on ? look::kInk : kCream, label);
        tx += lw + 8;
    }
    Glyph(d, "button_rb", g_v.P(tx, y0 + 66), g_v.P(tx + 30, y0 + 96), IM_COL32_WHITE, false);
    if (Clicked() && MouseIn(g_v.P(tx - 4, y0 + 62), g_v.P(tx + 34, y0 + 100))) ui::ClickPad(ImGuiKey_GamepadR1);
    for (int row = 0; row < 5; row++) {
        const float y = y0 + 120 + row * 58.0f;
        const bool on = row == e.row;
        Control(d, x0 + 28, y, x1 - 28, y + 50, on);
        ImFont* rowFont = on ? bold : (f.strong ? f.strong : f.body);
        Text(d, rowFont, g_v.L(21), g_v.P(x0 + 50, y + 11), on ? look::kInk : kCream, TR(kRuleRows[row]));
        Text(d, rowFont, g_v.L(21), g_v.P(x0 + 600, y + 11), on ? look::kInk : kCream, RuleValue(r, row), 0.5f);
        if (on) {
            d->AddTriangleFilled(g_v.P(x0 + 440, y + 19), g_v.P(x0 + 440, y + 31), g_v.P(x0 + 430, y + 25), look::kInk);
            d->AddTriangleFilled(g_v.P(x0 + 760, y + 19), g_v.P(x0 + 770, y + 25), g_v.P(x0 + 760, y + 31), look::kInk);
        }
    }
}

void RoomQueueTableRules(const std::vector<std::string>& rules) {
    g.rules.rules.assign(rules.size(), Rules{});
    g.rulesPending.clear();
    for (size_t t = 0; t < rules.size(); t++) {
        if (!ParseRules(rules[t], &g.rules.rules[t])) continue;
        if (!(g.rules.rules[t] == Rules{})) g.rulesPending.push_back(static_cast<int>(t));
    }
    Log("room screen: the creation form's rules for %zu of %zu tables to send", g.rulesPending.size(), rules.size());
}

} // namespace sfv::mod
