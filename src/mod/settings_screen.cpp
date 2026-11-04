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
const ImU32 kSoft = IM_COL32(217, 212, 198, 255);
const ImU32 kMuted = IM_COL32(168, 162, 147, 255);
const ImU32 kLine = IM_COL32(201, 168, 92, 190);

constexpr float kTop = 110, kBottom = 832, kRowH = 52, kRowPitch = 56, kHeaderH = 44;
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
    d->AddText(f, g_v.L(size), g_v.P(x, y), col, t.c_str(), nullptr, g_v.L(width));
    *bottom = y + f->CalcTextSizeA(g_v.L(size), FLT_MAX, g_v.L(width), t.c_str()).y / g_v.s;
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
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(6, 8, 11));
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

    if (const TextureView* bg = Texture(StageTex("CHN", "StageSelectBG"))) {
        Image(d, bg, g_v.P(-500, -40), g_v.P(2100, 720), ImVec2(0, 0), ImVec2(1, 1), Rgba(70, 70, 70));
    }
    d->AddRectFilledMultiColor(g_v.P(0, 315), g_v.P(1600, 630), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 210), Rgba(6, 8, 11, 210));
    d->AddRectFilledMultiColor(g_v.P(0, 630), g_v.P(1600, 900), Rgba(6, 8, 11, 210), Rgba(6, 8, 11, 210), Rgba(6, 8, 11), Rgba(6, 8, 11));

    // ---- input ----
    const SettingsRow& on = rows[g.focus];
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
    Text(d, f.display, g_v.L(40), g_v.P(48, 22), Rgba(255, 255, 255), "SFVR SETTINGS", 0, true);
    Text(d, f.body, g_v.L(16), g_v.P(48, 78), kSoft, "Version " + version);
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
    d->PushClipRect(g_v.P(40, kTop), g_v.P(960, kBottom), true);
    section.clear();
    for (int i = 0; i < count; i++) {
        const SettingsRow& r = rows[i];
        const float ry = kTop + ys[i] - g.scroll;
        if (r.section != section) {
            section = r.section;
            Text(d, f.strong, g_v.L(16), g_v.P(52, ry - kHeaderH + 14), kGold, r.section);
        }
        if (ry > kBottom || ry + kRowH < kTop) continue;
        const bool focused = i == g.focus;
        const ImVec2 a = g_v.P(48, ry), b = g_v.P(948, ry + kRowH);
        if (focused) {
            d->AddRectFilledMultiColor(a, g_v.P(48 + 900 * 0.3f, ry + kRowH), Rgba(240, 200, 90, 128), Rgba(240, 200, 90, 26),
                                       Rgba(240, 200, 90, 26), Rgba(240, 200, 90, 128));
            d->AddRectFilledMultiColor(g_v.P(48 + 900 * 0.3f, ry), b, Rgba(240, 200, 90, 26), Rgba(8, 9, 11, 153), Rgba(8, 9, 11, 153),
                                       Rgba(240, 200, 90, 26));
        }
        else {
            d->AddRectFilled(a, b, Rgba(8, 9, 11, 170));
        }
        d->AddLine(a, g_v.P(948, ry), kLine, g_v.L(1));
        d->AddLine(g_v.P(48, ry + kRowH), b, kLine, g_v.L(1));
        const ImU32 labelCol = r.enabled ? kCream : Rgba(244, 241, 232, 90);
        Text(d, f.body, g_v.L(19), g_v.P(88, ry + 14), labelCol, r.label);
        std::string value = r.value;
        if (focused && g.typing) value = g.text + ((static_cast<int>(ImGui::GetTime() * 2) & 1) ? "|" : " ");
        const ImU32 valueCol = !r.enabled ? Rgba(233, 196, 106, 90) : r.highlight ? kGold : kCream;
        Text(d, f.body, g_v.L(19), g_v.P(r.arrows ? 648.0f : 626.0f, ry + 14), valueCol, value, 0.5f);
        if (r.arrows && focused) {
            d->AddTriangleFilled(g_v.P(376, ry + 20), g_v.P(376, ry + 32), g_v.P(366, ry + 26), kGold);
            d->AddTriangleFilled(g_v.P(920, ry + 20), g_v.P(930, ry + 26), g_v.P(920, ry + 32), kGold);
        }
        if (focused && g.typing) d->AddRect(a, b, kGold, 0, 0, g_v.L(2));
        if (takeInput && !g.typing && Clicked() && MouseIn(a, b) && r.enabled) {
            if (g.focus == i) {
                if (r.text) {
                    g.typing = true;
                    g.text = r.editValue;
                }
                else if (r.arrows) in->row = r.id, in->delta = MouseIn(g_v.P(48, ry), g_v.P(648, ry + kRowH)) ? -1 : 1;
                else if (!r.action.empty()) in->row = r.id, in->confirm = true;
            }
            g.focus = i;
        }
    }
    if (total > view) {
        const float thumb = view * view / total, top = kTop + (view - thumb) * g.scroll / (total - view);
        d->AddRectFilled(g_v.P(954, kTop), g_v.P(958, kBottom), Rgba(255, 255, 255, 20), g_v.L(2));
        d->AddRectFilled(g_v.P(954, top), g_v.P(958, top + thumb), Rgba(233, 196, 106, 190), g_v.L(2));
    }
    d->PopClipRect();

    // ---- the side panel ----
    const SettingsRow& r = rows[g.focus];
    if (!r.title.empty()) {
        float py = 132;
        const float x = 1026, width = 500;
        // Measured first, so the frame fits what it holds.
        float bottom = py + 30 + 46;
        for (size_t k = 0; k < r.lines.size(); k++) {
            bottom += f.body->CalcTextSizeA(g_v.L(17), FLT_MAX, g_v.L(width), r.lines[k].c_str()).y / g_v.s + 12;
        }
        d->AddRectFilledMultiColor(g_v.P(1000, 104), g_v.P(1552, bottom + 20), Rgba(26, 31, 38, 235), Rgba(26, 31, 38, 235),
                                   Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240));
        d->AddLine(g_v.P(1000, 104), g_v.P(1552, 104), kLine, g_v.L(1));
        d->AddLine(g_v.P(1000, bottom + 20), g_v.P(1552, bottom + 20), kLine, g_v.L(1));
        Text(d, f.body, g_v.L(14), g_v.P(x, py - 4), kMuted, r.kicker);
        // A long title (a folder's path) shows its end.
        std::string title = r.title;
        while (title.size() > 4 && TextW(f.display, 26, title) > width) title = "..." + title.substr(4);
        Text(d, f.display, g_v.L(26), g_v.P(x, py + 22), kCream, title);
        py += 76;
        for (size_t k = 0; k < r.lines.size(); k++) {
            float end = py;
            Wrapped(d, f.body, 17, x, py, width, k == 0 && r.goldFirst ? kGold : kSoft, r.lines[k], &end);
            py = end + 12;
        }
    }

    // ---- the stripe ----
    d->AddRectFilled(g_v.P(0, 844), g_v.P(1600, 900), Rgba(8, 9, 11, 245));
    d->AddLine(g_v.P(0, 844), g_v.P(1600, 844), kLine, g_v.L(1));
    Text(d, f.body, g_v.L(16), g_v.P(48, 862), kSoft,
         g.typing ? "Type, then Enter to keep it or Escape to leave it as it was. Empty: your Steam name." : r.hint);
    std::vector<std::pair<const char*, std::string>> prompts;
    if (g.typing) prompts = {};
    else {
        if (r.text) prompts.push_back({"button_a", "Type"});
        else if (!r.action.empty()) prompts.push_back({"button_a", r.action});
        prompts.push_back({"button_b", "Back"});
    }
    float right = 1564;
    for (int i = static_cast<int>(prompts.size()) - 1; i >= 0; i--) {
        const float w = 32 + 8 + TextW(f.body, 17, prompts[i].second);
        const float px = right - w;
        if (const TextureView* icon = Texture(PadTex(prompts[i].first))) Image(d, icon, g_v.P(px, 856), g_v.P(px + 32, 888));
        Text(d, f.body, g_v.L(17), g_v.P(px + 40, 861), kCream, prompts[i].second);
        if (takeInput && Clicked() && MouseIn(g_v.P(px, 850), g_v.P(right, 894))) {
            if (std::string(prompts[i].first) == "button_b") in->back = true;
            else if (r.text) {
                g.typing = true;
                g.text = r.editValue;
            }
            else in->row = r.id, in->confirm = true;
        }
        right = px - 26;
    }
    g_inputOn = false;
    d->PopClipRect();
}

} // namespace sfv::mod
