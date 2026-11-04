#include "mod/room_screen.h"

#include "core/keeper_protocol.h"
#include "core/rules.h"
#include "mod/rooms_screen.h"
#include "mod/ui_kit.h"
#include "mod/weyve_service.h"

#include <imgui.h>
#include <imgui_internal.h> // ImTextCharToUtf8

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace sfv::mod {

using namespace ui;

namespace {

const ImU32 kGold = IM_COL32(233, 196, 106, 255);
const ImU32 kRing = IM_COL32(240, 194, 74, 255);
const ImU32 kCream = IM_COL32(244, 241, 232, 255);
const ImU32 kSoft = IM_COL32(217, 212, 198, 255);
const ImU32 kMuted = IM_COL32(168, 162, 147, 255);
const ImU32 kGreen = IM_COL32(127, 216, 139, 255);
const ImU32 kLine = IM_COL32(201, 168, 92, 190);

constexpr float kTablePitch = 190, kTableH = 182;
constexpr int kTablesInView = 4;
constexpr int kPlayersInView = 8;

struct State {
    int table = 0;
    float scroll = 0;
    bool leaving = false;
    bool typing = false;
    std::string text;
} g;

ImU32 Dim(ImU32 c, bool dim) { return dim ? (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * 0.45f) << IM_COL32_A_SHIFT) : c; }

void Panel(ImDrawList* d, float x0, float y0, float x1, float y1)
{
    d->AddRectFilledMultiColor(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(8, 9, 11, 214), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 214));
    d->AddLine(g_v.P(x0, y0), g_v.P(x1, y0), kLine, g_v.L(1));
    d->AddLine(g_v.P(x0, y1), g_v.P(x1, y1), kLine, g_v.L(1));
}

void Seat(ImDrawList* d, const HubFonts& f, float x, float y, bool dim)
{
    d->AddRectFilledMultiColor(g_v.P(x, y), g_v.P(x + 330, y + 120), Rgba(26, 31, 38, 230), Rgba(26, 31, 38, 230), Rgba(14, 17, 21, 230),
                               Rgba(14, 17, 21, 230));
    d->AddRect(g_v.P(x, y), g_v.P(x + 330, y + 120), Rgba(93, 90, 82, 160), 0, 0, g_v.L(1));
    Text(d, f.body, g_v.L(16), g_v.P(x + 165, y + 50), Dim(kMuted, dim), "Open seat", 0.5f);
}

void Table(ImDrawList* d, const HubFonts& f, float y, int index, const std::string& rulesText, int queued, bool on, bool dim)
{
    const ImVec2 a = g_v.P(48, y), b = g_v.P(868, y + kTableH);
    d->AddRectFilledMultiColor(a, b, Rgba(20, 24, 30, 235), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240), Rgba(20, 24, 30, 235));
    d->AddLine(a, g_v.P(868, y), Rgba(201, 168, 92, 128), g_v.L(1));
    Text(d, f.display, g_v.L(21), g_v.P(64, y + 13), Dim(kCream, dim), "TABLE " + std::to_string(index + 1));
    Rules rules;
    std::string line = ParseRules(rulesText, &rules) ? DescribeRules(rules) : std::string();
    // How many wait in the table's queue (user, 2026-11-04: not the
    // watchers); queues come with tables (M4), none until then.
    const std::string queue = std::to_string(queued) + " in queue";
    Text(d, f.body, g_v.L(16), g_v.P(852, y + 16), Dim(kSoft, dim), queue, 1);
    Text(d, f.body, g_v.L(16), g_v.P(852 - TextW(f.body, 16, queue) - 18, y + 16), Dim(kGold, dim), line, 1);
    Seat(d, f, 64, y + 48, dim);
    Seat(d, f, 522, y + 48, dim);
    Text(d, f.display, g_v.L(30), g_v.P(458, y + 88), Dim(Rgba(74, 68, 54), dim), "VS", 0.5f);
    if (on) d->AddRect(ImVec2(a.x + g_v.L(1.5f), a.y + g_v.L(1.5f)), ImVec2(b.x - g_v.L(1.5f), b.y - g_v.L(1.5f)), Dim(kRing, dim), 0, 0, g_v.L(3));
}

void Players(ImDrawList* d, const HubFonts& f, const RoomsView& v)
{
    Panel(d, 892, 80, 1552, 493);
    Text(d, f.body, g_v.L(14), g_v.P(910, 94), kMuted, "PLAYERS \xC2\xB7 " + std::to_string(v.members.size()) + " / 16");
    // You first, then the others oldest first.
    std::vector<RoomMember> list = v.members;
    std::stable_partition(list.begin(), list.end(), [&](const RoomMember& m) { return m.id == v.self; });
    for (int i = 0; i < static_cast<int>(list.size()) && i < kPlayersInView; i++) {
        const RoomMember& m = list[i];
        const float y = 115 + i * 45.0f;
        if (m.id == v.self) d->AddRectFilled(g_v.P(908, y), g_v.P(1534, y + 41), Rgba(42, 38, 24, 160));
        d->AddLine(g_v.P(908, y + 43), g_v.P(1534, y + 43), Rgba(255, 255, 255, 14), g_v.L(1));
        DrawFace(d, m.chara, 912, y + 5, 22, 30);
        float x = 952;
        if (m.owner) {
            DrawCrown(d, x, y + 26, 0.85f, kGold);
            x += 20;
        }
        Text(d, f.strong, g_v.L(16), g_v.P(x, y + 11), kCream, Upper(m.name));
        if (m.id == v.self) Text(d, f.body, g_v.L(16), g_v.P(x + TextW(f.strong, 16, Upper(m.name)) + 6, y + 11), kMuted, "(you)");
        Text(d, f.body, g_v.L(16), g_v.P(1128, y + 11), kSoft, "Idle");
        // The round trip to them (green under 60 ms, gold under 120, red
        // after), with a cloud when it goes through a relay.
        if (m.pingMs >= 0) {
            const ImU32 col = m.pingMs < 60 ? kGreen : m.pingMs < 120 ? kGold : IM_COL32(255, 107, 94, 255);
            const std::string ms = std::to_string(m.pingMs) + " ms";
            Text(d, f.strong, g_v.L(16), g_v.P(1418, y + 11), col, ms, 1);
            if (m.relayed) IconCloud(d, g_v.P(1418 - TextW(f.strong, 16, ms) - 16, y + 21), g_v.L(1.6f), col);
        }
        if (m.link) {
            const char* what = m.link == 1 ? "Wired" : "Wi-Fi";
            Text(d, f.body, g_v.L(15), g_v.P(1512, y + 12), kSoft, what, 1);
            IconLinkType(d, g_v.P(1512 - TextW(f.body, 15, what) - 14, y + 21), g_v.L(1.4f), m.link, kSoft);
        }
    }
    if (static_cast<int>(list.size()) > kPlayersInView) {
        Text(d, f.body, g_v.L(14), g_v.P(910, 476), kMuted, "+" + std::to_string(list.size() - kPlayersInView) + " more");
    }
}

void Chat(ImDrawList* d, const HubFonts& f, const RoomsView& v, bool* clickedBox, bool* clickedSend)
{
    Panel(d, 892, 507, 1552, 832);
    Text(d, f.body, g_v.L(14), g_v.P(910, 518), kMuted, "ROOM CHAT");
    // The newest at the bottom, above the box; older lines go up and out.
    const float wrap = 620, size = 16;
    float bottom = 773;
    d->PushClipRect(g_v.P(900, 540), g_v.P(1544, 775), true);
    for (int i = static_cast<int>(v.chat.size()) - 1; i >= 0 && bottom > 540; i--) {
        const ChatLine& line = v.chat[i];
        // A player's line: the name in bold, the message after it, its first
        // line indented by the name (spaces as wide), the rest wrapping under.
        std::string all = line.text;
        if (line.from) {
            const float nameW = TextW(f.strong, size, Upper(line.name)) + 8;
            std::string pad;
            while (TextW(f.body, size, pad) < nameW) pad += ' ';
            all = pad + line.text;
        }
        const float h = f.body->CalcTextSizeA(g_v.L(size), FLT_MAX, g_v.L(wrap), all.c_str()).y / g_v.s;
        const float y = bottom - h;
        if (line.from) {
            d->AddText(f.body, g_v.L(size), g_v.P(910, y), kCream, all.c_str(), nullptr, g_v.L(wrap));
            d->AddText(f.strong, g_v.L(size), g_v.P(910, y), kCream, Upper(line.name).c_str());
        }
        else {
            d->AddText(f.body, g_v.L(size), g_v.P(910, y), kGold, all.c_str(), nullptr, g_v.L(wrap));
        }
        bottom = y - 4;
    }
    d->PopClipRect();
    // The box and Send.
    const ImVec2 a = g_v.P(910, 781), b = g_v.P(1460, 819);
    d->AddRectFilled(a, b, Rgba(6, 8, 11, 230));
    d->AddRect(a, b, g.typing ? kRing : Rgba(93, 90, 82, 160), 0, 0, g_v.L(g.typing ? 2.0f : 1.0f));
    if (g.typing) {
        const std::string shown = g.text + ((static_cast<int>(ImGui::GetTime() * 2) & 1) ? "|" : " ");
        // Long text shows its end.
        std::string tail = shown;
        while (tail.size() > 1 && TextW(f.body, 16, tail) > 520) tail.erase(0, 1);
        Text(d, f.body, g_v.L(16), g_v.P(924, 790), kCream, tail);
    }
    else {
        Text(d, f.body, g_v.L(16), g_v.P(924, 790), kMuted, "Say something (Enter, or Back + Y on a pad)");
    }
    d->AddRectFilled(g_v.P(1468, 781), g_v.P(1534, 819), kGold, g_v.L(19));
    Text(d, f.strong, g_v.L(16), g_v.P(1501, 790), Rgba(21, 19, 13), "Send", 0.5f);
    *clickedBox = Clicked() && MouseIn(a, b);
    *clickedSend = Clicked() && MouseIn(g_v.P(1468, 781), g_v.P(1534, 819));
}

void LeaveDialog(ImDrawList* d, const HubFonts& f, const RoomsView& v)
{
    d->AddRectFilled(g_v.P(0, 0), g_v.P(1600, 844), Rgba(0, 0, 0, 150));
    const ImVec2 a = g_v.P(470, 330), b = g_v.P(1130, 500);
    d->AddRectFilledMultiColor(a, b, Rgba(26, 31, 38, 250), Rgba(26, 31, 38, 250), Rgba(11, 14, 18, 250), Rgba(11, 14, 18, 250));
    d->AddLine(a, g_v.P(1130, 330), kLine, g_v.L(2));
    d->AddLine(g_v.P(470, 500), b, kLine, g_v.L(2));
    Text(d, f.strong, g_v.L(16), g_v.P(504, 362), kGold, "ROOM " + WeyveService::DisplayCode(v.room));
    Text(d, f.display, g_v.L(28), g_v.P(504, 396), kCream, "Leave the room?");
    std::string text = "You can come back with the code while the room is open.";
    if (v.owner == v.self) {
        // The longest present after you gets it (the keeper's rule).
        for (const RoomMember& m : v.members) {
            if (m.id != v.self) {
                text = m.name + " becomes the owner.";
                break;
            }
        }
        if (v.members.size() <= 1) text = "You are the last one here: the room closes.";
    }
    Text(d, f.body, g_v.L(17), g_v.P(504, 452), kSoft, text);
}

void Bar(ImDrawList* d, const HubFonts& f, const std::string& lead, const std::string& text,
         const std::vector<std::pair<const char*, std::string>>& prompts, int* clicked)
{
    d->AddRectFilled(g_v.P(0, 844), g_v.P(1600, 900), Rgba(8, 9, 11, 245));
    d->AddLine(g_v.P(0, 844), g_v.P(1600, 844), kLine, g_v.L(1));
    float x = 48;
    if (!lead.empty()) {
        Text(d, f.strong, g_v.L(15), g_v.P(x, 863), kGold, lead);
        x += TextW(f.strong, 15, lead) + 14;
    }
    Text(d, f.body, g_v.L(16), g_v.P(x, 862), kSoft, text);
    float right = 1564;
    *clicked = -1;
    for (int i = static_cast<int>(prompts.size()) - 1; i >= 0; i--) {
        const float w = 32 + 8 + TextW(f.body, 17, prompts[i].second);
        const float px = right - w;
        if (const TextureView* icon = Texture(PadTex(prompts[i].first))) Image(d, icon, g_v.P(px, 856), g_v.P(px + 32, 888));
        Text(d, f.body, g_v.L(17), g_v.P(px + 40, 861), kCream, prompts[i].second);
        if (Clicked() && MouseIn(g_v.P(px, 850), g_v.P(right, 894))) *clicked = i;
        right = px - 26;
    }
}

} // namespace

void ResetRoomScreen() { g = State{}; }

void DrawRoomScreen(const HubFonts& f, const RoomsView& v, bool takeInput, RoomScreenInput* in)
{
    *in = RoomScreenInput{};
    if (!f.display || !f.strong || !f.body) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(6, 8, 11));
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    const bool paused = !v.keeper;
    const int tables = std::clamp(v.tables, 1, keeper::kMaxTables);
    g.table = std::clamp(g.table, 0, tables - 1);

    if (const TextureView* bg = Texture(StageTex("CHN", "StageSelectBG"))) {
        Image(d, bg, g_v.P(-500, -40), g_v.P(2100, 720), ImVec2(0, 0), ImVec2(1, 1), Rgba(80, 80, 80));
    }
    d->AddRectFilledMultiColor(g_v.P(0, 315), g_v.P(1600, 630), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 210), Rgba(6, 8, 11, 210));
    d->AddRectFilledMultiColor(g_v.P(0, 630), g_v.P(1600, 900), Rgba(6, 8, 11, 210), Rgba(6, 8, 11, 210), Rgba(6, 8, 11), Rgba(6, 8, 11));

    // ---- input (before drawing: the dialog and typing take it first) ----
    bool send = false;
    if (g.leaving) {
        if (Confirm()) {
            in->leave = true;
            g.leaving = false;
        }
        else if (Back()) {
            g.leaving = false;
        }
    }
    else if (g.typing) {
        for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
            if (c < 32 || c == 127) continue;
            char utf8[5] = {};
            ImTextCharToUtf8(utf8, c);
            if (keeper::Utf8Length(g.text) < keeper::kChatMaxChars) g.text += utf8;
        }
        if (Pressed({ImGuiKey_Backspace}) && !g.text.empty()) {
            // One character, not one byte.
            size_t n = g.text.size() - 1;
            while (n > 0 && (static_cast<unsigned char>(g.text[n]) & 0xC0) == 0x80) n--;
            g.text.erase(n);
        }
        if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false)) send = true;
        if (Pressed({ImGuiKey_Escape, ImGuiKey_GamepadFaceRight}, false)) g.typing = false;
    }
    else {
        if (Up()) g.table = std::max(0, g.table - 1);
        if (Down()) g.table = std::min(tables - 1, g.table + 1);
        // On a pad, Back + Y starts typing (a Deck's keyboard, Steam + X,
        // types into it); Back alone, let go without Y, copies the code.
        static bool backHeld = false, usedY = false;
        const bool back = g_inputOn && ImGui::IsKeyDown(ImGuiKey_GamepadBack);
        if (back && Pressed({ImGuiKey_GamepadFaceUp}, false)) {
            g.typing = true;
            usedY = true;
        }
        if (backHeld && !back) {
            if (!usedY) {
                ImGui::SetClipboardText(v.room.c_str());
                in->copiedCode = true;
            }
            usedY = false;
        }
        backHeld = back;
        if (Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false)) g.typing = true;
        else if (Back()) g.leaving = true;
        if (ImGui::GetIO().KeyCtrl && Pressed({ImGuiKey_C}, false)) {
            ImGui::SetClipboardText(v.room.c_str());
            in->copiedCode = true;
        }
    }
    if (g.table < g.scroll) g.scroll = static_cast<float>(g.table);
    if (g.table > g.scroll + kTablesInView - 1) g.scroll = static_cast<float>(g.table - kTablesInView + 1);

    // ---- the title line ----
    Text(d, f.display, g_v.L(40), g_v.P(48, 22), Rgba(255, 255, 255), "CUSTOM ROOM", 0, true);
    float x = 48 + TextW(f.display, 40, "CUSTOM ROOM") + 30;
    if (!paused) {
        Text(d, f.strong, g_v.L(17), g_v.P(x, 41), v.isPublic ? kGreen : kMuted, v.isPublic ? "Public" : "Private");
        x += TextW(f.strong, 17, v.isPublic ? "Public" : "Private") + 16;
    }
    Text(d, f.body, g_v.L(17), g_v.P(x, 41), kSoft, "Code");
    x += TextW(f.body, 17, "Code") + 6;
    const std::string code = WeyveService::DisplayCode(v.room);
    Text(d, f.strong, g_v.L(17), g_v.P(x, 41), kGold, code);
    x += TextW(f.strong, 17, code) + 20;
    if (const TextureView* icon = Texture(PadTex("button_back"))) Image(d, icon, g_v.P(x, 36), g_v.P(x + 28, 64));
    Text(d, f.body, g_v.L(17), g_v.P(x + 34, 41), kCream, "Copy code");
    if (!g.leaving && Clicked() && MouseIn(g_v.P(x, 32), g_v.P(x + 34 + TextW(f.body, 17, "Copy code"), 68))) {
        ImGui::SetClipboardText(v.room.c_str());
        in->copiedCode = true;
    }

    // ---- the tables ----
    d->PushClipRect(g_v.P(48, 80), g_v.P(880, 832), true);
    for (int t = 0; t < tables; t++) {
        const float y = 80 + (t - g.scroll) * kTablePitch;
        if (y > 832 || y + kTableH < 80) continue;
        Table(d, f, y, t, t < static_cast<int>(v.tableRules.size()) ? v.tableRules[t] : std::string(), 0, t == g.table, paused);
        if (!g.leaving && !g.typing && Clicked() && MouseIn(g_v.P(48, y), g_v.P(868, y + kTableH))) g.table = t;
    }
    d->PopClipRect();

    // ---- players and chat ----
    Players(d, f, v);
    bool clickedBox = false, clickedSend = false;
    Chat(d, f, v, &clickedBox, &clickedSend);
    if (!g.leaving) {
        if (clickedBox) g.typing = true;
        if (clickedSend && g.typing) send = true;
    }
    if (send) {
        if (!g.text.empty()) in->chat = g.text;
        g.text.clear();
        g.typing = false;
    }

    // ---- the stripe ----
    int clicked = -1;
    std::vector<std::pair<const char*, std::string>> prompts;
    std::string lead, text;
    if (g.leaving) {
        prompts = {{"button_a", "Leave"}, {"button_b", "Stay"}};
    }
    else if (g.typing) {
        text = "Enter sends, Escape or B stops. On a Steam Deck, Steam + X opens the keyboard.";
    }
    else if (paused) {
        lead = "ROOM PAUSED";
        text = "The keeper is away; it comes back by itself. Chat carries on.";
        prompts = {{"button_b", "Leave"}};
    }
    else {
        if (v.members.size() <= 1) lead = "ROOM OPEN";
        text = v.members.size() <= 1 ? "Friends join with the code." : "Seats and queues open on these tables in the next rooms build.";
        prompts = {{"button_b", "Leave"}};
    }
    if (g.leaving) LeaveDialog(d, f, v);
    Bar(d, f, lead, text, prompts, &clicked);
    if (clicked >= 0) {
        const std::string what = prompts[clicked].second;
        if (what == "Leave" && g.leaving) in->leave = true, g.leaving = false;
        else if (what == "Leave") g.leaving = true;
        else if (what == "Stay") g.leaving = false;
    }
    g_inputOn = false;
    d->PopClipRect();
}

} // namespace sfv::mod
