#include "mod/watch_panel.h"

#include "mod/ui_kit.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace sfv::mod {

using namespace ui;

void DrawWatchPanel(const HubFonts& f, const WatchPanelLive& live, float alpha, bool takeInput, WatchPanelInput* in)
{
    *in = WatchPanelInput{};
    if (!f.display || !f.strong || !f.body || alpha <= 0) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    const int from = d->VtxBuffer.Size;
    g_inputOn = takeInput;
    const ImU32 white = Rgba(244, 241, 232), dim = Rgba(168, 162, 147), gold = Rgba(233, 196, 106), green = Rgba(127, 216, 139);
    // The match dimmed behind, the panel in the middle.
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(4, 5, 7, 150));
    const float x0 = 380, y0 = 230, x1 = 1220, y1 = 700;
    d->AddRectFilled(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(8, 9, 11, 235));
    d->AddRect(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(201, 168, 92, 170), 0, 0, std::max(1.0f, g_v.L(1)));
    const std::string title = live.table >= 0 ? "WATCHING \xC2\xB7 TABLE " + std::to_string(live.table + 1) : "WATCHING";
    Text(d, f.display, g_v.L(30), g_v.P(x0 + 36, y0 + 28), white, title);
    const int count = static_cast<int>(live.games.size());
    const int at = std::clamp(live.at, -1, count - 1);
    char line[96];
    if (at >= 0) std::snprintf(line, sizeof(line), "GAME %d of %d", at + 1, count);
    else std::snprintf(line, sizeof(line), "%d game%s so far", count, count == 1 ? "" : "s");
    Text(d, f.strong, g_v.L(20), g_v.P(x1 - 36, y0 + 36), gold, line, 1.0f);
    // Who plays, and how far behind them this view is.
    const std::string vs = (live.players[0].empty() ? "P1" : live.players[0]) + "  vs  " + (live.players[1].empty() ? "P2" : live.players[1]);
    Text(d, f.strong, g_v.L(20), g_v.P(x0 + 36, y0 + 86), white, vs);
    const int gamesBehind = at >= 0 ? count - 1 - at : 0;
    std::string behind;
    if (gamesBehind > 0) behind = std::to_string(gamesBehind) + (gamesBehind == 1 ? " game" : " games") + " behind the players";
    else if (live.behindSeconds >= 1.5f) {
        std::snprintf(line, sizeof(line), "%.0f s behind the players", live.behindSeconds);
        behind = line;
    }
    else behind = "Live";
    Text(d, f.body, g_v.L(17), g_v.P(x0 + 36, y0 + 118), gamesBehind > 0 || live.behindSeconds >= 1.5f ? dim : green, behind);
    // The chips: one per kept game (up to 20 in a row, two rows), the one
    // watched lit; a click watches it.
    const float cw = 36, ch = 44, gap = 4, cx0 = x0 + 36, cy0 = y0 + 160;
    for (int i = 0; i < count && i < 40; i++) {
        const float cx = cx0 + (i % 20) * (cw + gap), cy = cy0 + (i / 20) * (ch + gap);
        const WatchPanelGame& g = live.games[i];
        const bool on = i == at;
        const ImVec2 a = g_v.P(cx, cy), b = g_v.P(cx + cw, cy + ch);
        d->AddRectFilled(a, b, on ? Rgba(240, 194, 74) : Rgba(28, 30, 34));
        d->AddRect(a, b, on ? Rgba(255, 230, 150) : Rgba(70, 70, 74), 0, 0, std::max(1.0f, g_v.L(1)));
        Text(d, f.strong, g_v.L(15), g_v.P(cx + cw / 2, cy + 5), on ? Rgba(26, 20, 5) : white, std::to_string(i + 1), 0.5f);
        const char* mark = !g.finished ? "\xE2\x80\xA2" : g.winner == 0 ? "P1" : g.winner == 1 ? "P2" : "-";
        Text(d, f.body, g_v.L(12), g_v.P(cx + cw / 2, cy + 25), on ? Rgba(26, 20, 5) : !g.finished ? green : dim, mark, 0.5f);
        if (takeInput && Clicked() && MouseIn(a, b)) in->watch = i;
    }
    if (at >= 0 && at < count) {
        const WatchPanelGame& g = live.games[at];
        std::snprintf(line, sizeof(line), "%s vs %s%s", g.p1.c_str(), g.p2.c_str(),
                      !g.finished ? "  \xC2\xB7  playing" : g.winner == 0 ? "  \xC2\xB7  P1 won" : g.winner == 1 ? "  \xC2\xB7  P2 won" : "");
        Text(d, f.body, g_v.L(17), g_v.P(x0 + 36, cy0 + 2 * (ch + gap) + 14), dim, line);
    }
    Text(d, f.body, g_v.L(15), g_v.P(x0 + 36, y1 - 122), dim, "Left and right (or LB and RB): the previous or next game, from its start.");
    Text(d, f.body, g_v.L(15), g_v.P(x0 + 36, y1 - 98), dim,
         live.table >= 0 ? "Watching follows the table to its next set." : "Watching ends when the players leave.");
    // Previous and Next on left and right; X the current game; B resumes; Y
    // stops watching.
    std::vector<Prompt> prompts;
    prompts.push_back({"button_x", "Current game", 2});
    prompts.push_back({"button_y", "Stop watching", 3});
    prompts.push_back({"button_b", "Resume", 1});
    // In the panel's foot (at the screen's top right they covered the
    // spectator's player bars).
    d->AddLine(g_v.P(x0 + 24, y1 - 62), g_v.P(x1 - 24, y1 - 62), Rgba(201, 168, 92, 90), std::max(1.0f, g_v.L(1)));
    DrawPrompts(d, f.body, prompts, x1 - 20, y1 - 52);
    if (takeInput) {
        if ((Pressed({ImGuiKey_GamepadL1}, false) || Left()) && at > 0) in->watch = at - 1;
        if ((Pressed({ImGuiKey_GamepadR1}, false) || Right()) && at >= 0 && at < count - 1) in->watch = at + 1;
        if (Pressed({ImGuiKey_GamepadFaceLeft, ImGuiKey_X}, false)) in->watch = -1;
        if (Pressed({ImGuiKey_GamepadFaceUp, ImGuiKey_Y}, false) || g_clickSetting) in->stop = true;
        if (Back() || Pressed({ImGuiKey_GamepadStart}, false)) in->resume = true;
    }
    g_inputOn = false;
    FadeFrom(d, from, alpha);
}

} // namespace sfv::mod
