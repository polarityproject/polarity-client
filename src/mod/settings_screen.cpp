#include "mod/settings_screen.h"

#include "mod/log.h"
#include "mod/ui_kit.h"

#include <imgui.h>
#include <imgui_internal.h> // ImTextCharToUtf8

#include <algorithm>
#include <atomic>

namespace sfv::mod {

using namespace ui;

namespace {

const ImU32 kGold = IM_COL32(233, 196, 106, 255);
const ImU32 kCream = IM_COL32(244, 241, 232, 255);
const ImU32 kSoft = IM_COL32(228, 224, 212, 255);
const ImU32 kMuted = IM_COL32(198, 193, 180, 255);
const ImU32 kLine = IM_COL32(201, 168, 92, 190);

// The list inside its pane (48, 96)-(948, 832): rows of 50 under strips of 38.
constexpr float kTop = 98, kBottom = 830, kRowH = 50, kRowPitch = 50, kHeaderH = 38;
constexpr float kValueX = 766; // the value box's centre (260 wide), its arrows either side
constexpr size_t kMaxText = 96; // a name's bytes (lobby::kMaxName)

std::atomic<bool> g_open{false};
struct State {
    int focus = 0;
    std::string focusId; // asked for on open
    float scroll = 0;    // design units
    bool typing = false;
    std::string text;
} g;

void Wrapped(ImDrawList* d, ImFont* f, float size, float x, float y, float width, ImU32 col, const std::string& t, float* bottom)
{
    d->AddText(f, TextPx(size), g_v.P(x, y), col, t.c_str(), nullptr, g_v.L(width));
    *bottom = y + f->CalcTextSizeA(TextPx(size), FLT_MAX, g_v.L(width), t.c_str()).y / g_v.s;
}

} // namespace

void OpenSettingsScreen(const std::string& focusRow)
{
    g.focusId = focusRow;
    g.typing = false;
    if (!g_open.exchange(true)) Log("settings: open");
}

void CloseSettingsScreen()
{
    if (g_open.exchange(false)) Log("settings: closed");
}

bool SettingsScreenOpen() { return g_open; }

void DrawSettingsScreen(const HubFonts& f, const std::string& version, const std::vector<SettingsRow>& rows, bool takeInput,
                        SettingsInput* in)
{
    *in = SettingsInput{};
    if (!g_open || !f.display || !f.strong || !f.body || rows.empty()) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    Backdrop(d);
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    const int count = static_cast<int>(rows.size());
    if (!g.focusId.empty()) {
        for (int i = 0; i < count; i++) {
            if (rows[i].id == g.focusId) g.focus = i;
        }
        g.focusId.clear();
    }
    g.focus = std::clamp(g.focus, 0, count - 1);
    // Rows that cannot be used are skipped by the cursor.
    auto step = [&](int from, int dir) {
        for (int i = from + dir; i >= 0 && i < count; i += dir) {
            if (rows[i].enabled) return i;
        }
        return from;
    };
    if (!rows[g.focus].enabled) g.focus = step(g.focus, 1) != g.focus ? step(g.focus, 1) : step(g.focus, -1);

    // ---- input ----
    const SettingsRow& on = rows[g.focus];
    if (g.typing && takeInput) ui::g_textEntry = true; // typing a name: every key is itself
    if (g.typing) {
        for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
            if (c < 32 || c == 127) continue;
            char utf8[5] = {};
            ImTextCharToUtf8(utf8, c);
            if (g.text.size() + std::char_traits<char>::length(utf8) <= kMaxText) g.text += utf8;
        }
        if (Pressed({ImGuiKey_Backspace}) && !g.text.empty()) {
            size_t n = g.text.size() - 1;
            while (n > 0 && (static_cast<unsigned char>(g.text[n]) & 0xC0) == 0x80) n--;
            g.text.erase(n);
        }
        if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false)) {
            in->row = on.id;
            in->typed = true;
            in->text = g.text;
            g.typing = false;
        }
        else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}, false)) {
            g.typing = false;
        }
    }
    else {
        if (Up()) g.focus = step(g.focus, -1);
        if (Down()) g.focus = step(g.focus, 1);
        const SettingsRow& r = rows[g.focus];
        if (r.arrows) {
            if (Left()) in->row = r.id, in->delta = -1;
            else if (Right()) in->row = r.id, in->delta = 1;
        }
        if (Confirm()) {
            if (r.text) {
                g.typing = true;
                g.text = r.editValue;
            }
            else if (r.arrows) {
                in->row = r.id;
                in->delta = 1;
            }
            else if (!r.action.empty()) {
                in->row = r.id;
                in->confirm = true;
            }
        }
        else if (Back()) {
            in->back = true;
        }
    }

    // ---- the list ----
    StageText(d, f.display, 40, 48, 22, Rgba(255, 255, 255), "SFVR SETTINGS");
    StageText(d, f.body, 20, 48 + TextW(f.display, 40, "SFVR SETTINGS") + 28, 39, kCream, "Version " + version);
    // Layout: where each row and header sits (design y, before scrolling).
    std::vector<float> ys(count);
    float y = 0;
    std::string section;
    for (int i = 0; i < count; i++) {
        if (rows[i].section != section) {
            section = rows[i].section;
            y += kHeaderH;
        }
        ys[i] = y;
        y += kRowPitch;
    }
    const float total = y, view = kBottom - kTop;
    // The focused row and its header stay in view.
    const float want = ys[g.focus];
    if (want - kHeaderH < g.scroll) g.scroll = std::max(0.0f, want - kHeaderH);
    if (want + kRowH > g.scroll + view) g.scroll = want + kRowH - view;
    g.scroll = std::clamp(g.scroll, 0.0f, std::max(0.0f, total - view));
    // One glass pane holds the list: a dark strip for each section, the rows
    // with hairlines, the focused one gold.
    Pane(d, 48, kTop - 2, 948, kBottom + 2);
    d->PushClipRect(g_v.P(50, kTop), g_v.P(946, kBottom), true);
    section.clear();
    ImFont* bold = f.bold ? f.bold : f.strong;
    for (int i = 0; i < count; i++) {
        const SettingsRow& r = rows[i];
        const float ry = kTop + ys[i] - g.scroll;
        if (r.section != section) {
            section = r.section;
            Strip(d, 50, ry - kHeaderH, 946, ry, false);
            Text(d, bold, g_v.L(17), g_v.P(72, ry - kHeaderH + 9), kGold, r.section);
        }
        if (ry > kBottom || ry + kRowH < kTop) continue;
        const bool focused = i == g.focus;
        const ImVec2 a = g_v.P(50, ry), b = g_v.P(946, ry + kRowH);
        if (focused) d->AddRectFilled(a, b, look::kGold);
        else d->AddLine(g_v.P(50, ry + kRowH - 0.5f), g_v.P(946, ry + kRowH - 0.5f), look::kHairline, std::max(1.0f, g_v.L(1)));
        const ImU32 labelCol = focused ? look::kInk : r.enabled ? kCream : Rgba(244, 241, 232, 90);
        // Rows in the medium weight (the light one read faintly on the glass).
        ImFont* rowFont = focused ? bold : f.strong;
        Text(d, rowFont, g_v.L(21), g_v.P(90, ry + 11), labelCol, r.label);
        std::string value = r.value;
        if (focused && g.typing) value = g.text + ((static_cast<int>(ImGui::GetTime() * 2) & 1) ? "|" : " ");
        const ImU32 valueCol = focused ? look::kInk : !r.enabled ? Rgba(233, 196, 106, 90) : r.highlight ? kGold : kCream;
        // A long value (a path) takes the room left of the box.
        const float valueW = TextW(rowFont, 21, value);
        Text(d, rowFont, g_v.L(21), g_v.P(valueW > 260 ? 896.0f : kValueX, ry + 11), valueCol, value, valueW > 260 ? 1.0f : 0.5f);
        if (r.arrows && focused) {
            d->AddTriangleFilled(g_v.P(kValueX - 144, ry + 19), g_v.P(kValueX - 144, ry + 31), g_v.P(kValueX - 154, ry + 25), look::kInk);
            d->AddTriangleFilled(g_v.P(kValueX + 144, ry + 19), g_v.P(kValueX + 154, ry + 25), g_v.P(kValueX + 144, ry + 31), look::kInk);
        }
        if (focused && g.typing) {
            d->AddRectFilled(a, b, Rgba(0, 0, 0, 140));
            Text(d, f.strong, g_v.L(21), g_v.P(90, ry + 11), kCream, r.label);
            Text(d, f.strong, g_v.L(21), g_v.P(896, ry + 11), kCream, value, 1.0f);
            d->AddRect(a, b, look::kRing, 0, 0, g_v.L(2));
        }
        if (takeInput && !g.typing && Clicked() && MouseIn(a, b) && r.enabled) {
            if (g.focus == i) {
                if (r.text) {
                    g.typing = true;
                    g.text = r.editValue;
                }
                else if (r.arrows) in->row = r.id, in->delta = MouseIn(g_v.P(48, ry), g_v.P(kValueX, ry + kRowH)) ? -1 : 1;
                else if (!r.action.empty()) in->row = r.id, in->confirm = true;
            }
            g.focus = i;
        }
    }
    d->PopClipRect();
    // Longer than the pane: the bar inside its right edge.
    ScrollBar(d, 936, kTop + 4, kBottom - 4, g.scroll, view, total, false);

    // ---- the side panel ----
    const SettingsRow& r = rows[g.focus];
    if (!r.title.empty()) {
        float py = 140;
        const float x = 1026, width = 500;
        // Measured first, so the frame fits what it holds.
        float bottom = py + 30 + 46;
        for (size_t k = 0; k < r.lines.size(); k++) {
            bottom += f.body->CalcTextSizeA(TextPx(20), FLT_MAX, g_v.L(width), r.lines[k].c_str()).y / g_v.s + 12;
        }
        Pane(d, 1000, 96, 1552, bottom + 12);
        Strip(d, 1002, 98, 1550, 140, false);
        Text(d, f.bold ? f.bold : f.strong, g_v.L(17), g_v.P(x, 109), kGold, r.kicker);
        // A long title (a folder's path) shows its end.
        std::string title = r.title;
        while (title.size() > 4 && TextW(f.display, 28, title) > width) title = "..." + title.substr(4);
        Text(d, f.display, g_v.L(28), g_v.P(x, py + 16), kCream, title);
        py += 66;
        for (size_t k = 0; k < r.lines.size(); k++) {
            float end = py;
            Wrapped(d, f.body, 20, x, py, width, k == 0 ? (r.goldFirst ? kGold : kCream) : kSoft, r.lines[k], &end);
            py = end + 12;
        }
    }

    // ---- the hint and prompts, on the stage ----
    PromptList prompts;
    if (!g.typing) {
        if (r.text) prompts.push_back({"button_a", "Type"});
        else if (!r.action.empty()) prompts.push_back({"button_a", r.action});
        prompts.push_back({"button_b", "Back"});
    }
    int clicked = -1;
    PromptLine(d, f.body, f.bold ? f.bold : f.strong, "", kGold,
               g.typing ? "Enter to save, Escape to cancel." : r.hint, prompts, &clicked);
    (void)clicked; // a prompt clicked is its button, next frame
    g_inputOn = false;
    d->PopClipRect();
}

} // namespace sfv::mod
