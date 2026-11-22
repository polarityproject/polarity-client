// The fight's own screens: the leave modal, the post-match vote and notices (docs/overlay.md).

#include "mod/fight_ui.h"

#include "core/text.h"
#include "mod/log.h"
#include "mod/rooms_screen.h"
#include "mod/ui_kit.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace sfv::mod {

using namespace ui;

namespace {

constexpr int kLeaveButton = 0, kStayButton = 1;
constexpr int kVoteButtons = 3, kDelayRow = kVoteButtons;
int g_leaveChoice = kStayButton;
int g_voteFocus = 0;

void Diamond(ImDrawList* d, float x, float y, float size, ImU32 col) {
    const float h = size * 0.7071f;
    d->AddQuadFilled(g_v.P(x, y - h), g_v.P(x + h, y), g_v.P(x, y + h), g_v.P(x - h, y), col);
}

float WrappedH(ImFont* f, float size, float width, const std::string& t) {
    return f->CalcTextSizeA(TextPx(size), FLT_MAX, g_v.L(width), t.c_str()).y / g_v.s;
}

void Wrapped(ImDrawList* d, ImFont* f, float size, float x, float y, float width, ImU32 col, const std::string& t) {
    d->AddText(f, TextPx(size), g_v.P(x, y), col, t.c_str(), nullptr, g_v.L(width));
}

} // namespace

void FightUiResetLeaveModal() { g_leaveChoice = kStayButton; }

void FightUiDrawLeaveModal(const HubFonts& f, bool set, bool takeInput, bool* leave, bool* stay) {
    *leave = *stay = false;
    if (!f.display || !f.body) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    g_inputOn = takeInput;
    ImFont* bold = f.bold ? f.bold : f.strong;
    ModalSpec m;
    m.w = 600;
    m.h = 44 + 22 + 34 + 12 + 24 + 24 + 48 + 26 + 48;
    m.x0 = 500;
    m.y0 = (844 - m.h) / 2;
    m.kicker = TR(Str::fight_leave_kicker);
    m.danger = true;
    m.glass = Glass::Fight;
    m.prompts = {{"button_a", TR(Str::fight_leave_prompt_select)}, {"button_b", TR(Str::fight_leave_prompt_stay)}};
    int prompt = -1;
    float y = Modal(d, bold, f.body, m, &prompt) + 22;
    Text(d, f.display, g_v.L(28), g_v.P(528, y), look::kCream, TR(set ? Str::fight_leave_title_set : Str::fight_leave_title_match));
    y += 46;
    Text(d, f.body, g_v.L(19), g_v.P(528, y), look::kCream, TR(set ? Str::fight_leave_body_set : Str::fight_leave_body_session));
    y += 48;
    int pressed = -1;
    if (Button(d, bold, 528, y, 806, y + 48, TR(Str::fight_leave_leave), g_leaveChoice == kLeaveButton, true)) pressed = kLeaveButton;
    if (Button(d, bold, 822, y, 1072, y + 48, TR(Str::fight_leave_stay), g_leaveChoice == kStayButton)) pressed = kStayButton;
    if (takeInput) {
        if (Left() || Right()) g_leaveChoice = g_leaveChoice == kLeaveButton ? kStayButton : kLeaveButton;
        if (pressed >= 0) g_leaveChoice = pressed;
        const char* how = nullptr;
        if (Back()) how = "B", *stay = true;
        else if (Confirm() || pressed >= 0) {
            how = pressed >= 0 ? "a click" : "A";
            if (g_leaveChoice == kLeaveButton) *leave = true;
            else *stay = true;
        }
        if (how) Log("leave modal: %s by %s", *leave ? "Leave" : "Stay", how);
    }
    g_inputOn = false;
}

void FightUiResetVote() { g_voteFocus = 0; }

void FightUiDrawVote(const HubFonts& f, const VoteSide (&sides)[2], bool armed, bool takeInput, VoteInput* in) {
    *in = VoteInput{};
    if (!f.display || !f.body) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    g_inputOn = takeInput;
    ImFont* bold = f.bold ? f.bold : f.strong;
    constexpr float kW = 380, kBottom = 832, kInner = kW - 44;
    const char* const kButtons[3] = {TR(Str::fight_vote_rematch), TR(Str::fight_vote_character_select), TR(Str::fight_vote_leave)};
    for (const VoteSide& s : sides) {
        if (!s.shown) continue;
        float h = 44 + 16 + 40 + 10 + 22;
        for (const std::string& line : s.lines) h += WrappedH(f.body, 15, kInner, line) + 6;
        const bool buttons = s.mine && s.choosing;
        if (buttons) h += 4 + 3 * 48 + 2 * 8 + 10 + 50 + (s.suggestion.empty() ? 0 : 26);
        h += 18 + 46;
        const float x0 = s.slot == 0 ? 48 : 1600 - 48 - kW, x1 = x0 + kW, y0 = kBottom - h;
        Pane(d, x0, y0, x1, kBottom, Glass::Fight);
        Strip(d, x0 + 2, y0 + 2, x1 - 2, y0 + 44, false);
        Text(d, bold, g_v.L(14), g_v.P(x0 + 22, y0 + 14), look::kGold, TR(s.slot == 0 ? (s.mine ? Str::fight_vote_p1_you : Str::fight_vote_p1) : (s.mine ? Str::fight_vote_p2_you : Str::fight_vote_p2)));
        std::string name = s.name.empty() ? TR(s.slot == 0 ? Str::fight_vote_player1 : Str::fight_vote_player2) : s.name;
        const float maxName = kW - 44 - 90 - 28;
        while (name.size() > 1 && TextW(f.body, 15, name) > maxName) name.pop_back();
        const float nameW = TextW(f.body, 15, name);
        Text(d, f.body, g_v.L(15), g_v.P(x1 - 22, y0 + 13), look::kSoft, name, 1);
        RoomsScreenDrawFlag(d, f.strong, s.country, x1 - 22 - nameW - 8 - 20, y0 + 16, 13);
        float y = y0 + 44 + 16;
        const std::string wins = std::to_string(s.wins);
        Text(d, f.display, g_v.L(40), g_v.P(x0 + 22, y - 6), look::kCream, wins);
        const std::string of = s.firstTo > 0 ? TRF(Str::fight_vote_of_set, s.firstTo) : TR(s.wins == 1 ? Str::fight_vote_match_won : Str::fight_vote_matches_won);
        Text(d, f.body, g_v.L(16), g_v.P(x0 + 22 + TextW(f.display, 40, wins) + 10, y + 16), look::kSoft, of);
        y += 40 + 10;
        static const ImU32 kStatus[3] = {look::kSoft, look::kOk, look::kMuted};
        Text(d, bold, g_v.L(16), g_v.P(x0 + 22, y), kStatus[static_cast<int>(s.tone)], s.status);
        y += 22;
        for (const std::string& line : s.lines) {
            Wrapped(d, f.body, 15, x0 + 22, y + 2, kInner, look::kSoft, line);
            y += WrappedH(f.body, 15, kInner, line) + 6;
        }
        if (buttons) {
            y += 4;
            int pressed = -1;
            for (int b = 0; b < kVoteButtons; b++) {
                const bool on = g_voteFocus == b;
                if (Button(d, bold, x0 + 22, y, x1 - 22, y + 48, kButtons[b], on) && takeInput) pressed = b;
                if (s.chosen == b && !on) Ring(d, x0 + 22, y, x1 - 22, y + 48);
                y += 48 + (b < kVoteButtons - 1 ? 8 : 0);
            }
            y += 10;
            const bool on = g_voteFocus == kDelayRow;
            Control(d, x0 + 22, y, x1 - 22, y + 50, on);
            const ImU32 ink = on ? look::kInk : look::kCream;
            Text(d, on ? bold : f.body, g_v.L(18), g_v.P(x0 + 44, y + 13), ink, TR(Str::fight_vote_delay));
            const std::string value = TRF(s.delay == 1 ? Str::fight_vote_delay_frame : Str::fight_vote_delay_frames, s.delay);
            const float vx = x1 - 44 - 18 - 12;
            Text(d, on ? bold : f.body, g_v.L(18), g_v.P(vx, y + 13), ink, value, 1);
            const float vw = TextW(on ? bold : f.body, 18, value);
            const ImU32 arrow = on ? look::kInk : look::kGold;
            d->AddTriangleFilled(g_v.P(vx - vw - 14, y + 19), g_v.P(vx - vw - 14, y + 31), g_v.P(vx - vw - 24, y + 25), arrow);
            d->AddTriangleFilled(g_v.P(x1 - 44 - 10, y + 19), g_v.P(x1 - 44, y + 25), g_v.P(x1 - 44 - 10, y + 31), arrow);
            if (takeInput && Clicked() && MouseIn(g_v.P(x0 + 22, y), g_v.P(x1 - 22, y + 50))) {
                g_voteFocus = kDelayRow;
                in->delayStep = MouseIn(g_v.P(x0 + 22, y), g_v.P((x0 + x1) / 2, y + 50)) ? -1 : 1;
            }
            y += 50;
            if (!s.suggestion.empty()) Text(d, f.body, g_v.L(15), g_v.P(x0 + 22, y + 6), look::kMuted, s.suggestion);
            if (takeInput) {
                if (Up()) g_voteFocus = std::max(0, g_voteFocus - 1);
                if (Down()) g_voteFocus = std::min(kDelayRow, g_voteFocus + 1);
                if (g_voteFocus == kDelayRow) {
                    if (Left()) in->delayStep = -1;
                    if (Right()) in->delayStep = 1;
                }
                if (Confirm() && g_voteFocus < kDelayRow) pressed = g_voteFocus;
                if (pressed >= 0) {
                    g_voteFocus = pressed;
                    if (armed) in->choose = pressed;
                }
            }
        }
        Strip(d, x0 + 2, kBottom - 48, x1 - 2, kBottom - 2, false);
        std::string hint = s.hint;
        while (hint.size() > 1 && TextW(f.body, 15, hint) > kInner) hint.pop_back();
        Text(d, f.body, g_v.L(15), g_v.P(x0 + 22, kBottom - 35), look::kMuted, hint);
    }
    g_inputOn = false;
}

void FightUiDrawToasts(const HubFonts& f, const std::vector<Notice>& toasts, bool overScreen) {
    if (toasts.empty() || !f.body) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    constexpr float kRight = 1568, kMaxW = 520, kTextW = kMaxW - 18 - 10 - 12 - 18;
    float y = 24;
    for (int i = static_cast<int>(toasts.size()) - 1; i >= 0; i--) {
        const Notice& n = toasts[i];
        const float in = std::clamp(n.age / 0.22f, 0.0f, 1.0f), out = std::clamp(n.left / 0.4f, 0.0f, 1.0f);
        const float ease = 1 - (1 - in) * (1 - in);
        const float alpha = std::min(ease, out);
        const float textW = std::min(kTextW, TextW(f.body, 16, n.text));
        const float textH = std::max(21.0f, WrappedH(f.body, 16, kTextW, n.text));
        const float w = 18 + 10 + 12 + textW + 18 + 2, h = textH + 20;
        const float x0 = kRight - w, y0 = y - (1 - ease) * (h + 10);
        const int from = d->VtxBuffer.Size;
        if (!overScreen) d->AddRectFilled(g_v.P(x0, y0), g_v.P(kRight, y0 + h), Rgba(10, 12, 16, 160));
        Pane(d, x0, y0, kRight, y0 + h, overScreen ? Glass::Modal : Glass::Fight);
        Diamond(d, x0 + 18 + 5, y0 + h / 2, 10, n.color);
        Wrapped(d, f.body, 16, x0 + 18 + 10 + 12, y0 + 10, kTextW, look::kCream, n.text);
        FadeFrom(d, from, alpha);
        y += (h + 10) * ease;
    }
}

void FightUiDrawNotes(const HubFonts& f, const std::vector<Notice>& notes) {
    if (notes.empty() || !f.body) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    float y1 = 900 - 92;
    for (int i = static_cast<int>(notes.size()) - 1; i >= 0; i--) {
        const Notice& n = notes[i];
        const float alpha = std::min(std::clamp(n.age / 0.2f, 0.0f, 1.0f), std::clamp(n.left / 0.4f, 0.0f, 1.0f));
        std::string text = n.text;
        const float kickW = n.kicker.empty() ? 0 : TextW(bold, 14, n.kicker) + 10;
        while (text.size() > 1 && 14 + 8 + 10 + kickW + TextW(f.body, 14, text) + 14 > 900) text.pop_back();
        const float w = 14 + 8 + 10 + kickW + TextW(f.body, 14, text) + 14, h = 30;
        const float x0 = 800 - w / 2, y0 = y1 - h;
        const int from = d->VtxBuffer.Size;
        DropShadow(d, x0, y0, x0 + w, y1, 4, 12, 0.35f, true);
        d->AddRectFilled(g_v.P(x0, y0), g_v.P(x0 + w, y1), Rgba(10, 12, 16, 184));
        d->AddRect(g_v.P(x0, y0), g_v.P(x0 + w, y1), Rgba(236, 228, 206, 89), 0.0f, 0, std::max(1.0f, g_v.L(1)));
        Diamond(d, x0 + 14 + 4, y0 + h / 2, 8, n.color);
        float x = x0 + 14 + 8 + 10;
        if (!n.kicker.empty()) {
            Text(d, bold, g_v.L(14), g_v.P(x, y0 + 6), n.color, n.kicker);
            x += kickW;
        }
        Text(d, f.body, g_v.L(14), g_v.P(x, y0 + 6), look::kSoft, text);
        FadeFrom(d, from, alpha);
        y1 = y0 - 6;
    }
}

void FightUiDrawStatusNote(const HubFonts& f, const std::string& text, uint32_t color) {
    if (!f.body || text.empty()) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    std::string shown = text;
    while (shown.size() > 1 && TextW(f.body, 14, shown) > 760) shown.pop_back();
    const float w = 14 + 8 + 10 + TextW(f.body, 14, shown) + 14, h = 30;
    const float x1 = 1600 - 24, y1 = 900 - 18, x0 = x1 - w, y0 = y1 - h;
    d->AddRectFilled(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(10, 12, 16, 184));
    d->AddRect(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(236, 228, 206, 89), 0.0f, 0, std::max(1.0f, g_v.L(1)));
    Diamond(d, x0 + 14 + 4, y0 + h / 2, 8, color);
    Text(d, f.body, g_v.L(14), g_v.P(x0 + 32, y0 + 6), look::kSoft, shown);
}

void FightUiDrawMenuNote(const HubFonts& f) {
    if (!f.body) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    const std::string text = TR(Str::fight_menu_note);
    const float kickW = TextW(bold, 17, "Polarity") + 10;
    const float w = 14 + 8 + 10 + kickW + TextW(f.body, 17, text) + 16, h = 36;
    const float x0 = 800 - w / 2, y0 = 12;
    DropShadow(d, x0, y0, x0 + w, y0 + h, 4, 12, 0.35f, true);
    d->AddRectFilled(g_v.P(x0, y0), g_v.P(x0 + w, y0 + h), Rgba(10, 12, 16, 184));
    d->AddRect(g_v.P(x0, y0), g_v.P(x0 + w, y0 + h), Rgba(236, 228, 206, 89), 0.0f, 0, std::max(1.0f, g_v.L(1)));
    Diamond(d, x0 + 14 + 4, y0 + h / 2, 8, look::kGold);
    Text(d, bold, g_v.L(17), g_v.P(x0 + 32, y0 + 7), look::kGold, "Polarity");
    Text(d, f.body, g_v.L(17), g_v.P(x0 + 32 + kickW, y0 + 7), look::kCream, text);
}

} // namespace sfv::mod
