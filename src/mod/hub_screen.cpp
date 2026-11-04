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
         "The game as it ships: story, arcade, training, versus and the rest. Hold Start in its menus to come back here.",
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

void DrawPanel(ImDrawList* d, const HubFonts& f, const Entry& e)
{
    const Frame p = kPanel;
    d->AddRectFilled(g_v.P(p.x, p.y), g_v.P(p.x + p.w, p.y + p.h), Rgba(11, 14, 18));
    d->PushClipRect(g_v.P(p.x, p.y), g_v.P(p.x + p.w, p.y + p.h), true);
    if (!e.stage.empty()) Cover(d, Texture(StageTex(e.stage, "StageSelectBG")), p.x, p.y, p.w, p.h, Rgba(158, 158, 158));
    // Dark on the left for the title, clear on the right for the art.
    const float x46 = p.x + p.w * 0.46f, x72 = p.x + p.w * 0.72f;
    d->AddRectFilledMultiColor(g_v.P(p.x, p.y), g_v.P(x46, p.y + p.h), Rgba(8, 9, 11, 235), Rgba(8, 9, 11, 140), Rgba(8, 9, 11, 140),
                               Rgba(8, 9, 11, 235));
    d->AddRectFilledMultiColor(g_v.P(x46, p.y), g_v.P(x72, p.y + p.h), Rgba(8, 9, 11, 140), Rgba(8, 9, 11, 0), Rgba(8, 9, 11, 0),
                               Rgba(8, 9, 11, 140));
    if (!e.chara.empty()) {
        if (const TextureView* art = Texture(CharaTex(e.chara, "PartCharacter"))) {
            const float size = 520, x = p.x + p.w - 20 - size, y = p.y - 40;
            Image(d, art, g_v.P(x, y), g_v.P(x + size, y + size));
        }
    }
    d->AddRectFilledMultiColor(g_v.P(p.x, p.y + p.h - 120), g_v.P(p.x + p.w, p.y + p.h), Rgba(8, 9, 11, 0), Rgba(8, 9, 11, 0),
                               Rgba(8, 9, 11, 217), Rgba(8, 9, 11, 217));
    Text(d, f.display, g_v.L(64), g_v.P(p.x + 40, p.y + p.h - 34 - 24 - 12 - 64), Rgba(255, 255, 255), e.title, 0, true);
    Text(d, f.strong, g_v.L(19), g_v.P(p.x + 40, p.y + p.h - 34 - 24), kGold, e.info);
    d->PopClipRect();
    d->AddRect(g_v.P(p.x, p.y), g_v.P(p.x + p.w, p.y + p.h), Rgba(201, 168, 92, 190), 0, 0, g_v.L(2));
}

void DrawTile(ImDrawList* d, const HubFonts& f, const Entry& e, const Frame& r, bool on)
{
    const ImVec2 a = g_v.P(r.x, r.y), b = g_v.P(r.x + r.w, r.y + r.h);
    if (on) {
        // The glow around the focused tile.
        for (int i = 1; i <= 6; i++) {
            const float g = g_v.L(4.0f * i);
            d->AddRect(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g), IM_COL32(240, 194, 74, 34 - i * 5), 0, 0, g_v.L(4));
        }
        d->AddRectFilledMultiColor(a, b, Rgba(42, 38, 24), Rgba(42, 38, 24), Rgba(21, 19, 13), Rgba(21, 19, 13));
    }
    else {
        d->AddRectFilledMultiColor(a, b, Rgba(26, 31, 38, 235), Rgba(26, 31, 38, 235), Rgba(11, 14, 18, 235), Rgba(11, 14, 18, 235));
    }
    d->AddRect(ImVec2(a.x + g_v.L(1.5f), a.y + g_v.L(1.5f)), ImVec2(b.x - g_v.L(1.5f), b.y - g_v.L(1.5f)),
               on ? kRing : Rgba(157, 153, 143, 90), 0, 0, g_v.L(3));
    const ImU32 iconCol = on ? kRing : kMuted, titleCol = on ? Rgba(255, 255, 255) : Rgba(217, 212, 198), infoCol = on ? kGold : kMuted;
    const float cx = r.x + r.w / 2;
    if (r.h > 300) {
        // A big tile stacks its icon over the title, as the game's tall CFN tile.
        const float icon = 96, top = r.y + (r.h - (icon + 22 + 30 + 8 + 22)) / 2;
        DrawIcon(d, e.icon, cx, top + icon / 2, icon, iconCol);
        Text(d, f.display, g_v.L(30), g_v.P(cx, top + icon + 22), titleCol, e.title, 0.5f);
        Text(d, f.strong, g_v.L(17), g_v.P(cx, top + icon + 22 + 30 + 8), infoCol, e.info, 0.5f);
    }
    else {
        const float icon = 60, cy = r.y + r.h / 2;
        DrawIcon(d, e.icon, r.x + 32 + icon / 2, cy, icon, iconCol);
        Text(d, f.display, g_v.L(30), g_v.P(r.x + 32 + icon + 26, cy - 30), titleCol, e.title);
        Text(d, f.strong, g_v.L(17), g_v.P(r.x + 32 + icon + 26, cy + 8), infoCol, e.info);
    }
}

void DrawTop(ImDrawList* d, const HubFonts& f, const HubLive& live)
{
    Text(d, f.display, g_v.L(42), g_v.P(800, 52), Rgba(255, 255, 255), "SFVR", 0.5f, true);
    // Right to left: the version, the server, the player.
    float x = 1560;
    const float y = 24;
    Text(d, f.body, g_v.L(16), g_v.P(x, y), kMuted, live.version, 1);
    x -= TextW(f.body, 16, live.version) + 22;
    static const char* const kStates[] = {"offline", "connecting", "connected", "unreachable"};
    const std::string server = live.server + " \xC2\xB7 " + kStates[std::clamp(live.serverState, 0, 3)];
    Text(d, f.body, g_v.L(16), g_v.P(x, y), kGold, server, 1);
    x -= TextW(f.body, 16, server) + 16;
    static const ImU32 kDots[] = {IM_COL32(120, 116, 104, 255), IM_COL32(233, 196, 106, 255), IM_COL32(127, 216, 139, 255),
                                  IM_COL32(224, 108, 96, 255)};
    d->AddCircleFilled(g_v.P(x, y + 11), g_v.L(4), kDots[std::clamp(live.serverState, 0, 3)]);
    x -= 22;
    Text(d, f.strong, g_v.L(16), g_v.P(x, y), kCream, Upper(live.player.empty() ? "Player" : live.player), 1);
}

// The bottom stripe: the focused entry's description, and the prompts.
// Returns which prompt was clicked: 0 A, 1 Start, -1 none.
int DrawBottom(ImDrawList* d, const HubFonts& f, const std::string& text)
{
    d->AddRectFilled(g_v.P(0, 844), g_v.P(1600, 900), Rgba(8, 9, 11, 245));
    d->AddLine(g_v.P(0, 844), g_v.P(1600, 844), Rgba(201, 168, 92, 190), g_v.L(1));
    Text(d, f.body, g_v.L(16), g_v.P(48, 862), Rgba(217, 212, 198), text);
    struct P {
        const char* icon;
        const char* label;
    };
    const P prompts[] = {{"button_a", "Select"}, {"button_start", "SFVR settings"}};
    float right = 1564;
    int clicked = -1;
    for (int i = 1; i >= 0; i--) {
        const float w = 32 + 8 + TextW(f.body, 17, prompts[i].label);
        const float x = right - w;
        if (const TextureView* icon = Texture(PadTex(prompts[i].icon))) Image(d, icon, g_v.P(x, 856), g_v.P(x + 32, 888));
        Text(d, f.body, g_v.L(17), g_v.P(x + 40, 861), kCream, prompts[i].label);
        if (Clicked() && MouseIn(g_v.P(x, 850), g_v.P(right, 894))) clicked = i;
        right = x - 26;
    }
    return clicked;
}

} // namespace

void OpenHub()
{
    if (!g_open.exchange(true)) {
        g_openedAt = -1;
        Log("hub: open");
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
    const float alpha = static_cast<float>(std::clamp((now - g_openedAt) / 0.25, 0.0, 1.0));
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const int from = d->VtxBuffer.Size;
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(6, 8, 11));
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    const std::vector<Entry> entries = Entries(live);
    const int count = static_cast<int>(entries.size());
    g_focus = std::clamp(g_focus, 0, count - 1);

    // Behind everything: the focused entry's stage, dark, fading to black.
    const Entry& focused = entries[g_focus];
    if (!focused.stage.empty()) Cover(d, Texture(StageTex(focused.stage, "StageSelectBG")), -500, -40, 2600, 700, Rgba(82, 82, 82));
    d->AddRectFilledMultiColor(g_v.P(0, 0), g_v.P(1600, 270), Rgba(6, 8, 11, 115), Rgba(6, 8, 11, 115), Rgba(6, 8, 11, 51),
                               Rgba(6, 8, 11, 51));
    d->AddRectFilledMultiColor(g_v.P(0, 270), g_v.P(1600, 560), Rgba(6, 8, 11, 51), Rgba(6, 8, 11, 51), Rgba(6, 8, 11, 180),
                               Rgba(6, 8, 11, 180));
    d->AddRectFilledMultiColor(g_v.P(0, 560), g_v.P(1600, 900), Rgba(6, 8, 11, 180), Rgba(6, 8, 11, 180), Rgba(6, 8, 11),
                               Rgba(6, 8, 11));

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
    for (int i = 0; i < count; i++) DrawTile(d, f, entries[i], kFrames[i], i == g_focus);
    const int prompt = DrawBottom(d, f, entries[g_focus].text);
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
