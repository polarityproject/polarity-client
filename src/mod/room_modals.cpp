// The room screen's modals: the your-turn card, the connecting steps, the table rules.

#include "mod/room_internal.h"

#include "mod/log.h"
#include "mod/ui_sound.h"

#include <algorithm>
#include <cmath>

namespace sfv::mod {

using namespace roomui;
using namespace ui;
using keeper::TablePhase;

namespace roomui {

std::string LeaveText(const RoomsView& v) {
    std::string text;
    for (const TableView& t : v.tableViews) {
        const auto at = std::find(t.queue.begin(), t.queue.end(), v.self);
        if (at != t.queue.end() || t.s.seat[0] == v.self || t.s.seat[1] == v.self) {
            text = "You lose your place at Table " + std::to_string(&t - v.tableViews.data() + 1) + ". ";
        }
    }
    if (v.owner == v.self) {
        if (v.members.size() <= 1) return text + "You're the last one here, so the room closes.";
        for (const RoomMember& m : v.members) {
            if (m.id != v.self) return text + m.name + " becomes the owner.";
        }
    }
    return text.empty() ? "You can rejoin with the code while the room is open." : text;
}

void Card(ImDrawList* d, const HubFonts& f, const RoomsView& v, int t, const PromptList& prompts, const std::string& hint, ImU32 hintCol, int* clicked) {
    const keeper::TableState& s = v.tableViews[t].s;
    ImFont* bold = f.bold ? f.bold : f.strong;
    const float x0 = 380, w = 840, h = 616, y0 = 142, x1 = x0 + w;
    ModalSpec m;
    m.x0 = x0, m.y0 = y0, m.w = w, m.h = h;
    m.kicker = "YOUR TURN \xC2\xB7 TABLE " + std::to_string(t + 1);
    Rules rules;
    if (ParseRules(v.tableViews[t].rules, &rules)) m.aside = DescribeRules(rules), m.asideCol = kGold;
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
        Text(d, bold, g_v.L(15), g_v.P(ax, y0 + 192), kSoft, i ? (i == mySeat ? "P2 \xC2\xB7 YOU" : "P2") : i == mySeat ? "P1 \xC2\xB7 YOU \xC2\xB7 HOSTS"
                                                                                                                        : "P1 \xC2\xB7 HOSTS THE MATCH",
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
            const char* kicker = k ? "V-TRIGGER" : "V-SKILL";
            const int value = k ? vt : vs;
            const std::string text = value < 0 ? std::string("Random") : value == 0 ? std::string("\xE2\x80\x94")
                                                                                    : SkillLabel(code, k == 1, value);
            const float kw = TextW(bold, 15, kicker) + 10;
            const float tw = TextW(f.strong, 18, text);
            const float tx = i ? ax - tw : ax + kw;
            Text(d, bold, g_v.L(15), g_v.P(i ? tx - kw : ax, ly + 3), kGold, kicker);
            Text(d, f.strong, g_v.L(18), g_v.P(tx, ly), fresh ? kGold : kCream, text);
        }
    }
    StageText(d, f.display, 36, x0 + w / 2, y0 + 104, Rgba(255, 255, 255), "VS", 0.5f);
    if (const RoomMember* other = Member(v, s.seat[1 - mySeat]); other && other->pingMs >= 0) {
        const std::string ms = std::to_string(other->pingMs) + " ms";
        const ImU32 col = PingColor(static_cast<unsigned>(other->pingMs));
        Text(d, bold, g_v.L(14), g_v.P(x0 + w / 2, y0 + 150), kSoft, "PING", 0.5f);
        StageText(d, f.strong, 22, x0 + w / 2, y0 + 168, col, ms, 0.5f);
        if (other->relayed) IconCloud(d, g_v.P(x0 + w / 2, y0 + 208), g_v.L(1.7f), col);
    }
    const double total = std::max(1.0, (s.callSeconds ? s.callSeconds : kKeeperCallSeconds) - kCardMarginSeconds);
    const double left = std::clamp(total - (RoomsClockMs() - v.tableViews[t].callSeenMs) / 1000.0, 0.0, total);
    const float ty = y0 + 296;
    const int second = static_cast<int>(std::ceil(left));
    if (!g.cardReady && !s.ready[mySeat] && second > 0 && second <= 5 && second != g.cardTick) PlayUi(UiSound::Countdown);
    g.cardTick = second;
    if (g.cardReady || s.ready[mySeat]) {
        Text(d, f.display, g_v.L(28), g_v.P(x0 + w / 2, ty + 40), kGreen, "READY", 0.5f);
        Text(d, f.body, g_v.L(19), g_v.P(x0 + w / 2, ty + 86), kCream, "Waiting for " + NameOf(v, s.seat[1 - mySeat]) + ".", 0.5f);
        return;
    }
    Text(d, f.display, g_v.L(26), g_v.P(x0 + w / 2, ty), Rgba(255, 255, 255),
         left > 0 ? "Ready in " + std::to_string(static_cast<int>(left + 0.99)) : std::string("Out of time"), 0.5f);
    d->AddRectFilled(g_v.P(x0 + 28, ty + 40), g_v.P(x1 - 28, ty + 44), Rgba(255, 255, 255, 31));
    d->AddRectFilled(g_v.P(x0 + 28, ty + 40), g_v.P(x0 + 28 + (w - 56) * static_cast<float>(left / total), ty + 44), kGold);
    static const char* const kRows[] = {"V-Skill", "V-Trigger", "Input delay"};
    const float rx0 = x0 + 160, rx1 = x1 - 160, vx = rx1 - 150;
    for (int r = 0; r < kCardRows; r++) {
        const float ry = ty + 62 + r * 56;
        const bool on = r == g.cardRow, usable = r == kCardDelay || g_cardPick.editable;
        Control(d, rx0, ry, rx1, ry + 50, on);
        const ImU32 col = on ? look::kInk : usable ? kCream
                                                   : Rgba(244, 241, 232, 110);
        Text(d, on ? bold : f.strong, g_v.L(19), g_v.P(rx0 + 22, ry + 12), col, kRows[r]);
        std::string value = r == kCardDelay        ? std::to_string(g.cardDelay) + (g.cardDelay == 1 ? " frame" : " frames")
                            : !g_cardPick.editable ? std::string("Random")
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
    Text(d, f.body, g_v.L(17), g_v.P(x0 + w / 2, ty + 62 + 3 * 56 + 6), kSoft, "If time runs out, you go to the back of the queue.", 0.5f);
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
    steps.push_back({"Both ready", HandoffStep::Done});
    steps.push_back({room ? std::string("Match set up") : host ? std::string("Setting up the match")
                                                               : "Waiting for " + other,
                     room ? HandoffStep::Done : HandoffStep::InProgress});
    std::string connected = "Connected";
    if (h.pingMs) connected += " \xC2\xB7 " + std::to_string(h.pingMs) + " ms";
    steps.push_back({linked ? connected : host ? "Waiting for " + other + " to join"
                                               : "Connecting to " + other,
                     linked ? HandoffStep::Done : room ? HandoffStep::InProgress
                                                       : HandoffStep::ToCome});
    const bool mine = h.preloaded[host ? 0 : 1];
    steps.push_back({loaded ? std::string("Characters loaded") : mine ? "Waiting for " + other + "'s game"
                                                                      : std::string("Loading characters"),
                     loaded ? HandoffStep::Done : linked ? HandoffStep::InProgress
                                                         : HandoffStep::ToCome});
    steps.push_back({"Starting the match", h.jumping ? HandoffStep::InProgress : HandoffStep::ToCome});
    return steps;
}

void HandoffModal(ImDrawList* d, const HubFonts& f, const RoomsView& v, int t, const std::vector<HandoffStep>& steps, double stepSeconds, const PromptList& prompts, int* clicked) {
    const keeper::TableState& s = v.tableViews[t].s;
    const uint32_t otherId = s.seat[s.seat[0] == v.self ? 1 : 0];
    ImFont* bold = f.bold ? f.bold : f.strong;
    const float x0 = 480, w = 640, h = 404, y0 = 220, x1 = x0 + w;
    ModalSpec m;
    m.x0 = x0, m.y0 = y0, m.w = w, m.h = h;
    m.kicker = "TABLE " + std::to_string(t + 1) + " \xC2\xB7 " + (g_handoff.played > 0 ? "GAME " + std::to_string(g_handoff.played + 1) : std::string("YOUR TURN"));
    Rules rules;
    if (ParseRules(v.tableViews[t].rules, &rules)) m.hint = DescribeRules(rules);
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
    Text(d, f.display, g_v.L(28), g_v.P(tx, y0 + 92), kCream, "vs");
    float nx = tx + TextW(f.display, 28, "vs") + 12;
    if (other && other->country.size() == 2) nx += DrawFlag(d, f.strong, other->country, nx, y0 + 103, 13) + 10;
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
        if (st.state == HandoffStep::InProgress && stepSeconds >= 3) text += " \xC2\xB7 " + std::to_string(static_cast<int>(stepSeconds)) + " s";
        Text(d, f.body, g_v.L(17), g_v.P(x0 + 66, y), st.state == HandoffStep::ToCome ? kMuted : kCream, text);
        y += 36;
    }
}

} // namespace roomui

namespace {

const char* const kRuleRows[] = {"First to", "Rounds per game", "Round timer", "Rotation", "Stays up to"};

std::string RuleValue(const Rules& r, int row) {
    switch (row) {
        case 0: return std::to_string(r.firstTo) + (r.firstTo == 1 ? " game" : " games");
        case 1: return r.bestOf == 1 ? "One round" : "Best of " + std::to_string(r.bestOf);
        case 2: return std::to_string(r.timer) + " seconds";
        case 3: return r.rotation == Rotation::WinnerStays ? "Winner stays" : r.rotation == Rotation::LoserStays ? "Loser stays"
                                                                                                                 : "Round robin";
        default: return r.rotation == Rotation::RoundRobin ? "-" : r.staysLimit ? std::to_string(r.staysLimit) + " sets in a row"
                                                                                : "No limit";
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
    RoomDrawTableRules(f, g.rules, "OWNER", "Changes apply from the table's next set.", changed ? kGold : kSoft, prompts, clicked);
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
    Text(d, f.display, g_v.L(28), g_v.P(x0 + 28, y0 + 64), kCream, "Table rules");
    const int tables = static_cast<int>(e.rules.size());
    float tw = 0;
    for (int t = 0; t < tables; t++) tw += TextW(bold, 17, "Table " + std::to_string(t + 1)) + 28 + 8;
    float tx = x1 - 28 - 30 - 8 - tw - 30;
    Glyph(d, "button_lb", g_v.P(tx, y0 + 66), g_v.P(tx + 30, y0 + 96), IM_COL32_WHITE, false);
    if (Clicked() && MouseIn(g_v.P(tx - 4, y0 + 62), g_v.P(tx + 34, y0 + 100))) ui::ClickPad(ImGuiKey_GamepadL1);
    tx += 38;
    for (int t = 0; t < tables; t++) {
        const std::string label = "Table " + std::to_string(t + 1);
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
        Text(d, rowFont, g_v.L(21), g_v.P(x0 + 50, y + 11), on ? look::kInk : kCream, kRuleRows[row]);
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
