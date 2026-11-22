#include "mod/watch_panel.h"

#include "mod/rooms_screen.h" // RoomsScreenDrawFlag
#include "mod/ui_kit.h"

#include <imgui.h>

#include <algorithm>

namespace sfv::mod {

using namespace ui;

namespace {

int g_focus = 1; // 0 Previous, 1 Current, 2 Next

} // namespace

void DrawWatchPanel(const HubFonts& f, const WatchPanelLive& live, float alpha, bool takeInput, WatchPanelInput* in) {
    *in = WatchPanelInput{};
    if (!f.display || !f.strong || !f.body || alpha <= 0) {
        g_focus = 1; // it opens on the current game
        return;
    }
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const int from = d->VtxBuffer.Size;
    g_inputOn = takeInput;
    ImFont* bold = f.bold ? f.bold : f.strong;
    const int count = static_cast<int>(live.games.size());
    const int at = std::clamp(live.at, -1, count - 1);
    // Previous and Next only where there is a game that way; no results, no
    // list of games, so nothing about the set is spoiled.
    const bool can[3] = {at > 0, true, at >= 0 && at < count - 1};

    // A modal over the dimmed match: the table and the game in its header,
    // the two players, the three buttons; its prompts in its footer.
    ModalSpec m;
    m.x0 = 420;
    m.w = 760;
    m.h = 44 + 20 + 34 + 18 + 52 + 24 + 48;
    m.y0 = 440;
    m.glass = Glass::Fight;
    m.kicker = live.table >= 0 ? "WATCHING \xC2\xB7 TABLE " + std::to_string(live.table + 1) : "WATCHING";
    m.aside = "GAME " + std::to_string(at >= 0 ? at + 1 : std::max(count, 1));
    m.asideCol = look::kGold;
    m.hint = "Each game plays from the start. Results stay hidden.";
    m.prompts = {{"button_a", "Go"}, {"button_y", "Stop watching"}, {"button_b", "Resume"}};
    int prompt = -1;
    float y = Modal(d, bold, f.body, m, &prompt) + 20;
    // Flag, name, "vs", flag, name: smaller (down to 18) when two long names
    // would run past the pane, then cut.
    std::string names[2];
    for (int i = 0; i < 2; i++) names[i] = live.players[i].empty() ? (i == 0 ? "P1" : "P2") : live.players[i];
    auto lineW = [&](float size) {
        return TextW(f.display, size, names[0]) + TextW(f.display, size, names[1]) + TextW(f.display, size, "vs") + 4 * 12 + 2 * 32;
    };
    float size = 28;
    while (size > 18 && lineW(size) > 704) size -= 1;
    for (int i = 0; i < 2; i++) {
        while (names[i].size() > 1 && lineW(size) > 704) names[i].pop_back();
    }
    float x = 448;
    const float ty = y + (28 - size) / 2;
    for (int i = 0; i < 2; i++) {
        if (i == 1) {
            Text(d, f.display, g_v.L(size), g_v.P(x, ty), look::kSoft, "vs");
            x += TextW(f.display, size, "vs") + 12;
        }
        if (const float fw = RoomsScreenDrawFlag(d, f.strong, live.countries[i], x, y + 11, 15)) x += fw + 12;
        Text(d, f.display, g_v.L(size), g_v.P(x, ty), look::kCream, names[i]);
        x += TextW(f.display, size, names[i]) + 12;
    }
    y += 34 + 18;
    // The three buttons, the focused one gold; LB, X and RB go straight there.
    static const char* const kGlyphs[3] = {"button_lb", "button_x", "button_rb"};
    static const char* const kLabels[3] = {"Previous game", "Current game", "Next game"};
    if (!can[g_focus]) g_focus = 1;
    int pressed = -1;
    const float bw = (704 - 2 * 12) / 3.0f;
    for (int i = 0; i < 3; i++) {
        const float bx = 448 + i * (bw + 12);
        const bool on = i == g_focus;
        Control(d, bx, y, bx + bw, y + 52, on);
        const ImU32 col = on ? look::kInk : can[i] ? look::kCream
                                                   : Rgba(244, 241, 232, 80);
        const float lw = TextW(bold, 18, kLabels[i]);
        const float gw = 30, gx = bx + (bw - gw - 10 - lw) / 2;
        Glyph(d, kGlyphs[i], g_v.P(gx, y + 11), g_v.P(gx + gw, y + 41), Rgba(255, 255, 255, can[i] ? 255 : 80), false);
        Text(d, bold, g_v.L(18), g_v.P(gx + gw + 10, y + 14), col, kLabels[i]);
        if (takeInput && can[i] && Clicked() && MouseIn(g_v.P(bx, y), g_v.P(bx + bw, y + 52))) pressed = i;
    }
    if (takeInput) {
        if (Left()) {
            for (int i = g_focus - 1; i >= 0; i--) {
                if (can[i]) {
                    g_focus = i;
                    break;
                }
            }
        }
        if (Right()) {
            for (int i = g_focus + 1; i < 3; i++) {
                if (can[i]) {
                    g_focus = i;
                    break;
                }
            }
        }
        if (Pressed({ImGuiKey_GamepadL1}, false) && can[0]) pressed = 0;
        if (Pressed({ImGuiKey_GamepadFaceLeft}, false)) pressed = 1;
        if (Pressed({ImGuiKey_GamepadR1}, false) && can[2]) pressed = 2;
        if (Confirm()) pressed = g_focus;
        if (Pressed({ImGuiKey_GamepadFaceUp}, false)) in->stop = true;
        if (Back() || Pressed({ImGuiKey_GamepadStart}, false)) in->resume = true;
        if (pressed >= 0) {
            g_focus = pressed;
            in->watch = pressed == 0 ? at - 1 : pressed == 2 ? at + 1
                                                             : -1;
        }
    }
    g_inputOn = false;
    FadeFrom(d, from, alpha);
}

} // namespace sfv::mod
