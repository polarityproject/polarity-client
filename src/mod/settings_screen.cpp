// Polarity settings: the rows, typing a value, the side panel (docs/overlay.md).

#include "mod/settings_screen.h"

#include "core/text.h"
#include "mod/log.h"
#include "mod/ui_kit.h"
#include "mod/ui_sound.h"

#include <imgui.h>
#include <imgui_internal.h>

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

constexpr float kTop = 98, kBottom = 830, kRowH = 50, kRowPitch = 50, kHeaderH = 38;
constexpr float kValueX = 766;
constexpr size_t kMaxText = 96;

std::atomic<bool> g_open{false};

struct State {
    int focus = 0;
    std::string focusId;
    float scroll = 0;
    bool typing = false;
    std::string text;
    bool testType = false;
    std::string testText;
} g;

void Wrapped(ImDrawList* d, ImFont* f, float size, float x, float y, float width, ImU32 col, const std::string& t, float* bottom) {
    d->AddText(f, TextPx(size), g_v.P(x, y), col, t.c_str(), nullptr, g_v.L(width));
    *bottom = y + f->CalcTextSizeA(TextPx(size), FLT_MAX, g_v.L(width), t.c_str()).y / g_v.s;
}

} // namespace

void SettingsScreenShow(const std::string& focusRow) {
    g.focusId = focusRow;
    g.typing = false;
    if (!g_open.exchange(true)) Log("settings: open");
}

void SettingsScreenShowTyping(const std::string& focusRow, const std::string& text) {
    SettingsScreenShow(focusRow);
    g.testText = text;
    g.testType = true;
}

void SettingsScreenHide() {
    if (g_open.exchange(false)) Log("settings: closed");
}

bool SettingsScreenOpen() { return g_open; }

void SettingsScreenDraw(const HubFonts& f, const std::string& version, const std::vector<SettingsRow>& rows, bool takeInput,
                        SettingsInput* in) {
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
    if (g.testType) {
        g.testType = false;
        if (rows[g.focus].text) {
            g.typing = true;
            g.text = g.testText;
        }
    }
    auto step = [&](int from, int dir) {
        for (int i = from + dir; i >= 0 && i < count; i += dir) {
            if (rows[i].enabled) return i;
        }
        return from;
    };
    if (!rows[g.focus].enabled) g.focus = step(g.focus, 1) != g.focus ? step(g.focus, 1) : step(g.focus, -1);

    const SettingsRow& on = rows[g.focus];
    if (g.typing && takeInput) ui::g_textEntry = true;
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
            UiSoundPlay(UiSound::Decide);
            in->row = on.id;
            in->typed = true;
            in->text = g.text;
            g.typing = false;
        } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}, false)) {
            UiSoundPlay(UiSound::Cancel);
            g.typing = false;
        }
    } else {
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
            } else if (r.arrows) {
                in->row = r.id;
                in->delta = 1;
            } else if (!r.action.empty()) {
                in->row = r.id;
                in->confirm = true;
            }
        } else if (Back()) {
            in->back = true;
        }
    }

    StageText(d, f.display, 40, 48, 22, Rgba(255, 255, 255), TR(Str::settings_title));
    StageText(d, f.body, 20, 48 + TextW(f.display, 40, TR(Str::settings_title)) + 28, 39, kCream, TRF(Str::settings_version, version.c_str()));
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
    if (!g.typing) {
        if (const int w = WheelSteps(g_v.P(48, kTop), g_v.P(948, kBottom))) {
            g.scroll = std::clamp(g.scroll - w * kRowPitch, 0.0f, std::max(0.0f, total - view));
            auto inView = [&](int i) { return ys[i] - kHeaderH >= g.scroll && ys[i] + kRowH <= g.scroll + view; };
            const bool above = ys[g.focus] - kHeaderH<g.scroll, below = ys[g.focus] + kRowH> g.scroll + view;
            for (int k = 0; (above || below) && k < count; k++) {
                const int i = above ? k : count - 1 - k;
                if (rows[i].enabled && inView(i)) {
                    g.focus = i;
                    break;
                }
            }
        }
    }
    const float want = ys[g.focus];
    if (want - kHeaderH < g.scroll) g.scroll = std::max(0.0f, want - kHeaderH);
    if (want + kRowH > g.scroll + view) g.scroll = want + kRowH - view;
    g.scroll = std::clamp(g.scroll, 0.0f, std::max(0.0f, total - view));
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
        ImFont* rowFont = focused ? bold : f.strong;
        auto valueAt = [&](float w) { return w > 260 ? 896.0f : kValueX + 0.0f; };
        auto valueAlign = [&](float w) { return w > 260 ? 1.0f : 0.5f; };
        if (focused && g.typing) {
            d->AddRectFilled(a, b, Rgba(0, 0, 0, 140));
            Text(d, f.strong, g_v.L(21), g_v.P(90, ry + 11), kCream, r.label);
            std::string shown = g.text;
            while (!shown.empty() && TextW(f.strong, 21, shown) > 680) {
                size_t n = 1;
                while (n < shown.size() && (static_cast<unsigned char>(shown[n]) & 0xC0) == 0x80) n++;
                shown.erase(0, n);
            }
            const float w = TextW(f.strong, 21, shown);
            const float x = valueAt(w), align = valueAlign(w);
            Text(d, f.strong, g_v.L(21), g_v.P(x, ry + 11), kCream, shown, align);
            if (static_cast<int>(ImGui::GetTime() * 2) & 1) {
                const float end = x + w * (1.0f - align);
                Text(d, f.strong, g_v.L(21), g_v.P(end + 1, ry + 11), kCream, "|");
            }
            d->AddRect(a, b, look::kRing, 0, 0, g_v.L(2));
        } else {
            const ImU32 labelCol = focused ? look::kInk : r.enabled ? kCream
                                                                    : Rgba(244, 241, 232, 90);
            Text(d, rowFont, g_v.L(21), g_v.P(90, ry + 11), labelCol, r.label);
            const ImU32 valueCol = focused ? look::kInk : !r.enabled ? Rgba(233, 196, 106, 90)
                                                      : r.highlight  ? kGold
                                                                     : kCream;
            const float w = TextW(rowFont, 21, r.value);
            Text(d, rowFont, g_v.L(21), g_v.P(valueAt(w), ry + 11), valueCol, r.value, valueAlign(w));
            if (r.arrows && focused) {
                d->AddTriangleFilled(g_v.P(kValueX - 144, ry + 19), g_v.P(kValueX - 144, ry + 31), g_v.P(kValueX - 154, ry + 25), look::kInk);
                d->AddTriangleFilled(g_v.P(kValueX + 144, ry + 19), g_v.P(kValueX + 154, ry + 25), g_v.P(kValueX + 144, ry + 31), look::kInk);
            }
        }
        if (takeInput && !g.typing && Clicked() && MouseIn(a, b) && r.enabled) {
            if (g.focus == i) {
                if (r.text) {
                    g.typing = true;
                    g.text = r.editValue;
                } else if (r.arrows) in->row = r.id, in->delta = MouseIn(g_v.P(48, ry), g_v.P(kValueX, ry + kRowH)) ? -1 : 1;
                else if (!r.action.empty()) in->row = r.id, in->confirm = true;
            }
            g.focus = i;
        }
    }
    d->PopClipRect();
    ScrollBar(d, 936, kTop + 4, kBottom - 4, g.scroll, view, total, false);

    const SettingsRow& r = rows[g.focus];
    if (!r.title.empty()) {
        float py = 140;
        const float x = 1026, width = 500;
        float bottom = py + 30 + 46;
        for (size_t k = 0; k < r.lines.size(); k++) {
            bottom += f.body->CalcTextSizeA(TextPx(20), FLT_MAX, g_v.L(width), r.lines[k].c_str()).y / g_v.s + 12;
        }
        Pane(d, 1000, 96, 1552, bottom + 12);
        Strip(d, 1002, 98, 1550, 140, false);
        Text(d, f.bold ? f.bold : f.strong, g_v.L(17), g_v.P(x, 109), kGold, r.kicker);
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

    PromptList prompts;
    if (!g.typing) {
        if (r.text) prompts.push_back({"button_a", TR(Str::common_prompt_type)});
        else if (!r.action.empty()) prompts.push_back({"button_a", r.action});
        prompts.push_back({"button_b", TR(Str::common_prompt_back)});
    }
    int clicked = -1;
    PromptLine(d, f.body, f.bold ? f.bold : f.strong, "", kGold,
               g.typing ? std::string(TR(Str::settings_typing_hint)) : r.hint, prompts, &clicked);
    (void)clicked;
    g_inputOn = false;
    d->PopClipRect();
}

} // namespace sfv::mod
