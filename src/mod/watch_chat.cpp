#include "mod/watch_chat.h"

#include "core/keeper_protocol.h"
#include "mod/log.h"
#include "mod/room_screen.h"
#include "mod/ui_kit.h"

#include <imgui.h>
#include <imgui_internal.h> // ImTextCharToUtf8

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <mutex>

namespace sfv::mod {

using namespace ui;

namespace {

const ImU32 kGold = IM_COL32(233, 196, 106, 255);
const ImU32 kCream = IM_COL32(244, 241, 232, 255);
const ImU32 kSoft = IM_COL32(228, 224, 212, 255);

// The box (design units, 1600 x 900): between the V-gauges at the bottom,
// clear of the round timer and the health bars at the top.
constexpr float kX0 = 540, kX1 = 1060, kBottom = 858, kTextSize = 17, kLines = 6, kPad = 10;

std::atomic<bool> g_shown{true};
std::atomic<bool> g_toggle{false}, g_type{false};
bool g_typing = false;
std::string g_text;
std::mutex g_testMutex;
std::string g_testText;
bool g_testPending = false, g_testSend = false;

} // namespace

void WatchChatToggle() { g_toggle = true; }

void WatchChatStartTyping() { g_type = true; }

bool WatchChatShown() { return g_shown; }

void WatchChatTestText(const std::string& text, bool send) {
    std::lock_guard lock(g_testMutex);
    g_testText = text;
    g_testSend = send;
    g_testPending = true;
}

void DrawWatchChat(const HubFonts& f, const RoomsView& v, bool takeInput, WatchChatInput* in) {
    *in = WatchChatInput{};
    if (!f.body || !f.strong) return;
    BeginScreen();
    g_inputOn = takeInput;
    // Show and hide: a Back tap (the overlay's pad reading) or Tab.
    const bool toggle = g_toggle.exchange(false) || (takeInput && !g_typing && Pressed({ImGuiKey_Tab}, false));
    if (toggle && takeInput) {
        g_shown = !g_shown;
        if (!g_shown) g_typing = false;
        Log("watch chat: %s", g_shown ? "shown" : "hidden");
    }
    // Typing: Back + Y, or T (a letter the game does not take as a button).
    const bool type = g_type.exchange(false) || (takeInput && !g_typing && Pressed({ImGuiKey_T}, false));
    if (type && takeInput && !g_typing) {
        g_shown = true;
        g_typing = true;
        g_text.clear();
    }
    if (!takeInput) g_typing = false;
    {
        std::lock_guard lock(g_testMutex);
        if (g_testPending && takeInput) {
            g_testPending = false;
            g_shown = true;
            g_typing = true;
            g_text = g_testText;
            if (g_testSend) {
                in->send = g_text;
                g_text.clear();
                g_typing = false;
            }
        }
    }
    if (g_typing) {
        ui::g_textEntry = true; // every key is itself, not the game's
        for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
            if (c < 32 || c == 127) continue;
            char utf8[5] = {};
            ImTextCharToUtf8(utf8, c);
            if (keeper::Utf8Length(g_text) < keeper::kChatMaxChars) g_text += utf8;
        }
        if (Pressed({ImGuiKey_Backspace}) && !g_text.empty()) {
            size_t n = g_text.size() - 1;
            while (n > 0 && (static_cast<unsigned char>(g_text[n]) & 0xC0) == 0x80) n--;
            g_text.erase(n);
        }
        if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false)) {
            if (!g_text.empty()) in->send = g_text;
            g_text.clear();
            g_typing = false;
        } else if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}, false)) {
            g_text.clear();
            g_typing = false;
        }
    }
    g_inputOn = false;
    if (!g_shown) return;

    // The last lines that fit (muted members' left out), newest at the bottom.
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    const float wrap = kX1 - kX0 - 2 * kPad;
    const float lineH = kTextSize * 1.3f;

    struct Shown {
        std::string name, all;
        bool room;
        float h;
    };

    std::vector<Shown> shown;
    float used = 0;
    const float room = kLines * lineH;
    for (int i = static_cast<int>(v.chat.size()) - 1; i >= 0 && used < room; i--) {
        const ChatLine& line = v.chat[i];
        if (line.from && RoomMemberMuted(line.from)) continue;
        const std::string name = Upper(v.wordFilter ? line.name : line.realName);
        const std::string& text = v.wordFilter ? line.censored : line.text;
        std::string all = text;
        if (line.from) {
            // The name first (bold), the message after it: its first line
            // indented by the name, the rest wrapping under.
            const float nameW = TextW(bold, kTextSize, name) + 8;
            std::string pad;
            while (TextW(f.body, kTextSize, pad) < nameW) pad += ' ';
            all = pad + text;
        }
        const float h = f.body->CalcTextSizeA(TextPx(kTextSize), FLT_MAX, g_v.L(wrap), all.c_str()).y / g_v.s;
        if (used + h > room && !shown.empty()) break;
        used += h + 2;
        shown.push_back({name, all, line.from == 0, h});
    }
    const float inputH = g_typing ? lineH + 8 : 0;
    const float top = kBottom - kPad - inputH - used - kPad;
    if (shown.empty() && !g_typing) return; // nothing said yet: no box
    d->AddRectFilled(g_v.P(kX0, top), g_v.P(kX1, kBottom), IM_COL32(8, 10, 14, 120), g_v.L(6));
    float y = kBottom - kPad - inputH;
    for (const Shown& s : shown) {
        y -= s.h + 2;
        if (s.room) {
            d->AddText(f.body, TextPx(kTextSize), g_v.P(kX0 + kPad, y), kSoft, s.all.c_str(), nullptr, g_v.L(wrap));
        } else {
            d->AddText(f.body, TextPx(kTextSize), g_v.P(kX0 + kPad, y), kCream, s.all.c_str(), nullptr, g_v.L(wrap));
            d->AddText(bold, TextPx(kTextSize), g_v.P(kX0 + kPad, y), kGold, s.name.c_str());
        }
    }
    if (g_typing) {
        // The line being typed, with its caret, in a field of its own.
        const float fy = kBottom - kPad - lineH - 4;
        d->AddRectFilled(g_v.P(kX0 + kPad - 4, fy - 2), g_v.P(kX1 - kPad + 4, fy + lineH + 2), IM_COL32(0, 0, 0, 150), g_v.L(4));
        d->AddRect(g_v.P(kX0 + kPad - 4, fy - 2), g_v.P(kX1 - kPad + 4, fy + lineH + 2), kGold, g_v.L(4), 0, std::max(1.0f, g_v.L(1)));
        std::string shownText = g_text;
        while (!shownText.empty() && TextW(f.body, kTextSize, shownText) > wrap - 20) shownText.erase(0, 1);
        const bool caret = static_cast<int>(ImGui::GetTime() * 2) % 2 == 0;
        Text(d, f.body, g_v.L(kTextSize), g_v.P(kX0 + kPad + 2, fy + 1), kCream, shownText + (caret ? "|" : ""));
        if (g_text.empty()) Text(d, f.body, g_v.L(kTextSize), g_v.P(kX0 + kPad + 8, fy + 1), IM_COL32(198, 193, 180, 160), "Enter sends, Escape cancels");
    }
}

} // namespace sfv::mod
