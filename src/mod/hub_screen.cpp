#include "mod/hub_screen.h"

#include "mod/log.h"
#include "mod/ui_kit.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace sfv::mod {
namespace {

using namespace ui;

enum class Icon { Pad, Rooms };

struct Entry {
    HubAction action;
    const char* title;
    Icon icon;
    std::string info; // the tile's and the panel's live line
    std::string text; // the bottom stripe
    std::string chara, stage;
};

struct Frame {
    float x, y, w, h;
};

std::atomic<bool> g_open{false};
int g_focus = 1;        // the last entry used keeps focus (0 Play Offline, 1 Custom Rooms)
double g_openedAt = 0;  // ImGui time, for the fade in
bool g_fromBlack = false; // ... from black (at launch)
bool g_clickedTile = false;

// 0.3.0's grid (mock-up "Rooms only"): Play Offline the full height of the
// left column, Custom Rooms the wide tile under the panel.
const Frame kFrames[] = {{60, 118, 470, 706}, {546, 464, 994, 360}};
constexpr Frame kPanel{546, 118, 994, 330};

const ImU32 kGold = IM_COL32(233, 196, 106, 255);
const ImU32 kRing = IM_COL32(240, 194, 74, 255);
const ImU32 kCream = IM_COL32(244, 241, 232, 255);
const ImU32 kMuted = IM_COL32(168, 162, 147, 255);

std::vector<Entry> Entries(const HubLive& live)
{
    return {
        {HubAction::PlayOffline, "PLAY OFFLINE", Icon::Pad, "Story, arcade, training, versus",
         "The game as it ships: story, arcade, training, versus and the rest. Back + Start in its menus comes back here.",
         live.offlineChara, live.offlineStage},
        {HubAction::CustomRooms, "CUSTOM ROOMS", Icon::Rooms, live.roomsInfo,
         "Up to 16 players on up to eight tables with their own rules. Public rooms are listed; private ones take a code.",
         live.roomsChara, live.roomsStage},
    };
}

// A picture filling `a`..`b` (design units) without stretching: cropped.
void Cover(ImDrawList* d, const TextureView* t, float x, float y, float w, float h, ImU32 tint)
{
    if (!t || !t->width || !t->height) return;
    const float ta = float(t->width) / float(t->height), fa = w / h;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (ta > fa) {
        const float keep = fa / ta;
        uv0.x = (1 - keep) / 2;
        uv1.x = 1 - uv0.x;
    }
    else {
        const float keep = ta / fa;
        uv0.y = (1 - keep) / 2;
        uv1.y = 1 - uv0.y;
    }
    Image(ImGui::GetBackgroundDrawList(), t, g_v.P(x, y), g_v.P(x + w, y + h), uv0, uv1, tint);
    (void)d;
}

// The mock-up's icons, on a 24-unit grid at `size` design units.
void DrawIcon(ImDrawList* d, Icon icon, float cx, float cy, float size, ImU32 col)
{
    const float k = size / 24.0f;
    auto at = [&](float x, float y) { return g_v.P(cx + (x - 12) * k, cy + (y - 12) * k); };
    const float stroke = g_v.L(1.4f * k);
    if (icon == Icon::Pad) {
        static const ImVec2 body[] = {{6.5f, 8},   {17.5f, 8},  {20.3f, 9.2f}, {21.4f, 11.1f}, {22.4f, 15.7f}, {21.8f, 17.5f},
                                      {20, 18.4f}, {18.1f, 17.9f}, {15.8f, 16}, {8.2f, 16},     {5.9f, 17.9f},  {4, 18.4f},
                                      {2.2f, 17.5f}, {1.6f, 15.7f}, {2.6f, 11.1f}, {3.7f, 9.2f}};
        for (const ImVec2& p : body) d->PathLineTo(at(p.x, p.y));
        d->PathStroke(col, ImDrawFlags_Closed, stroke);
        d->AddLine(at(7.5f, 10.5f), at(7.5f, 13.5f), col, stroke);
        d->AddLine(at(6, 12), at(9, 12), col, stroke);
        d->AddCircleFilled(at(15.5f, 11.2f), g_v.L(0.9f * k), col);
        d->AddCircleFilled(at(17.3f, 13), g_v.L(0.9f * k), col);
    }
    else {
        for (const ImVec2& o : {ImVec2(3, 3), ImVec2(13.5f, 3), ImVec2(3, 13.5f), ImVec2(13.5f, 13.5f)}) {
            d->AddRect(at(o.x, o.y), at(o.x + 7.5f, o.y + 7.5f), col, g_v.L(1 * k), 0, stroke);
        }
    }
}

// The panel over the Custom Rooms tile: the focused entry's stage and
// character inside a glass pane, its name and one tagline on them.
void DrawPanel(ImDrawList* d, const HubFonts& f, const Entry& e)
{
    const Frame p = kPanel;
    DropShadow(d, p.x, p.y, p.x + p.w, p.y + p.h, 6, 18, 0.32f, true);
    d->AddRectFilled(g_v.P(p.x, p.y), g_v.P(p.x + p.w, p.y + p.h), look::kGround);
    d->PushClipRect(g_v.P(p.x, p.y), g_v.P(p.x + p.w, p.y + p.h), true);
    if (!e.stage.empty()) Cover(d, TextureSized(StageTex(e.stage, "StageSelectBG"), g_v.L(p.w)), p.x, p.y, p.w, p.h, Rgba(230, 230, 230));
    d->AddRectFilled(g_v.P(p.x, p.y), g_v.P(p.x + p.w, p.y + p.h), Rgba(10, 12, 16, 97));
    if (!e.chara.empty()) {
        if (const TextureView* art = TextureSized(CharaTex(e.chara, "PartCharacter"), g_v.L(460))) {
            const float size = 460, x = p.x + p.w - 30 - size, y = p.y - 40;
            Image(d, art, g_v.P(x, y), g_v.P(x + size, y + size));
        }
    }
    StageText(d, f.display, 64, p.x + 40, p.y + 150, Rgba(255, 255, 255), e.title);
    StageText(d, f.bold ? f.bold : f.strong, 19, p.x + 40, p.y + 232, look::kGold, e.info);
    d->PopClipRect();
    const float edge = g_v.L(2);
    const ImVec2 a = g_v.P(p.x, p.y), b = g_v.P(p.x + p.w, p.y + p.h);
    d->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), Rgba(0, 0, 0, 140), 0.0f, 0, 1.0f);
    d->AddRect(ImVec2(a.x + edge / 2, a.y + edge / 2), ImVec2(b.x - edge / 2, b.y - edge / 2), Rgba(236, 228, 206, 102), 0.0f, 0, edge);
    d->AddLine(ImVec2(a.x + edge, a.y + edge + 0.5f), ImVec2(b.x - edge, a.y + edge + 0.5f), Rgba(255, 255, 255, 77), std::max(1.0f, g_v.L(1)));
}

// A tile: a glass pane with its name in a strip (gold with the focus) and
// its icon; no description inside it (the hint at the bottom says it).
// `state`: live state on the strip's right (the rooms' counts).
void DrawTile(ImDrawList* d, const HubFonts& f, const Entry& e, const Frame& r, bool on, const std::string& state)
{
    Pane(d, r.x, r.y, r.x + r.w, r.y + r.h);
    Strip(d, r.x + 2, r.y + 2, r.x + r.w - 2, r.y + 46, on);
    Text(d, f.display, g_v.L(22), g_v.P(r.x + 24, r.y + 11), on ? look::kInk : kCream, e.title);
    if (!state.empty()) Text(d, f.bold ? f.bold : f.strong, g_v.L(15), g_v.P(r.x + r.w - 24, r.y + 14), on ? look::kInkSoft : look::kSoft, state, 1);
    const float icon = r.h > 400 ? 96.0f : 80.0f;
    DrawIcon(d, e.icon, r.x + r.w / 2, r.y + 46 + (r.h - 46) / 2, icon, on ? kGold : look::kSoft);
    if (on) Ring(d, r.x, r.y, r.x + r.w, r.y + r.h);
}

void DrawTop(ImDrawList* d, const HubFonts& f, const HubLive& live)
{
    StageText(d, f.display, 42, 800, 30, Rgba(255, 255, 255), "SFVR", 0.5f);
    // Right to left: the version, the server, the player.
    float x = 1560;
    const float y = 22;
    StageText(d, f.body, 20, x, y, look::kSoft, live.version, 1);
    x -= TextW(f.body, 20, live.version) + 24;
    static const char* const kStates[] = {"offline", "connecting", "connected", "unreachable"};
    const std::string server = live.server + " \xC2\xB7 " + kStates[std::clamp(live.serverState, 0, 3)];
    StageText(d, f.body, 20, x, y, kGold, server, 1);
    x -= TextW(f.body, 20, server) + 18;
    static const ImU32 kDots[] = {IM_COL32(120, 116, 104, 255), IM_COL32(233, 196, 106, 255), IM_COL32(127, 216, 139, 255),
                                  IM_COL32(224, 108, 96, 255)};
    d->AddCircleFilled(g_v.P(x, y + 13), g_v.L(5), kDots[std::clamp(live.serverState, 0, 3)]);
    x -= 24;
    StageText(d, f.bold ? f.bold : f.strong, 20, x, y, kCream, Upper(live.player.empty() ? "Player" : live.player), 1);
}

} // namespace

void OpenHub(bool fromBlack)
{
    if (!g_open.exchange(true)) {
        g_openedAt = -1;
        g_fromBlack = fromBlack;
        Log("hub: open%s", fromBlack ? " (at launch)" : "");
    }
}

void CloseHub()
{
    if (g_open.exchange(false)) Log("hub: closed");
}

bool HubOpen() { return g_open; }

void WarmHubTextures(const HubLive& live)
{
    g_drawing = false;
    for (const char* icon : {"button_a", "button_start"}) Texture(PadTex(icon));
    for (const std::string& c : {live.roomsChara, live.offlineChara}) {
        if (!c.empty()) Texture(CharaTex(c, "PartCharacter"));
    }
    for (const std::string& s : {live.roomsStage, live.offlineStage}) {
        if (!s.empty()) Texture(StageTex(s, "StageSelectBG"));
    }
    g_drawing = true;
}

HubAction DrawHub(const HubFonts& f, const HubLive& live, bool takeInput)
{
    if (!g_open || !f.display || !f.strong || !f.body) return HubAction::None;
    BeginScreen();
    const double now = ImGui::GetTime();
    if (g_openedAt < 0) g_openedAt = now;
    const float alpha = static_cast<float>(std::clamp((now - g_openedAt) / (g_fromBlack ? 0.4 : 0.25), 0.0, 1.0));
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    if (g_fromBlack) {
        // At launch the game's main menu is behind: black under the fade.
        if (alpha < 1) d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, IM_COL32(0, 0, 0, 255));
        else g_fromBlack = false;
    }
    const int from = d->VtxBuffer.Size;
    Backdrop(d);
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    const std::vector<Entry> entries = Entries(live);
    const int count = static_cast<int>(entries.size());
    g_focus = std::clamp(g_focus, 0, count - 1);

    // Input: left and right between the column and the online tiles; the
    // mouse focuses what it is over and a click selects it.
    HubAction action = HubAction::None;
    if (Left()) g_focus = 0;
    if (Right()) g_focus = 1;
    for (int i = 0; i < count; i++) {
        const Frame& r = kFrames[i];
        if (takeInput && MouseIn(g_v.P(r.x, r.y), g_v.P(r.x + r.w, r.y + r.h))) {
            if (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0) g_focus = i;
            if (Clicked()) {
                g_focus = i;
                g_clickedTile = true;
            }
        }
    }
    DrawTop(d, f, live);
    DrawPanel(d, f, entries[g_focus]);
    for (int i = 0; i < count; i++) DrawTile(d, f, entries[i], kFrames[i], i == g_focus, entries[i].action == HubAction::CustomRooms ? live.roomsState : "");
    int prompt = -1;
    PromptLine(d, f.body, f.bold ? f.bold : f.strong, "", kGold, entries[g_focus].text,
               {{"button_a", "Select"}, {"button_start", "SFVR settings"}}, &prompt);
    if (Confirm() || std::exchange(g_clickedTile, false) || prompt == 0) action = entries[g_focus].action;
    if (Pressed({ImGuiKey_GamepadStart}, false) || prompt == 1) action = HubAction::Settings;
    g_inputOn = false;
    d->PopClipRect();
    FadeFrom(d, from, alpha);
    if (action != HubAction::None) {
        static const char* const kNames[] = {"none", "play offline", "custom rooms", "settings"};
        Log("hub: %s", kNames[static_cast<int>(action)]);
    }
    return action;
}

} // namespace sfv::mod
