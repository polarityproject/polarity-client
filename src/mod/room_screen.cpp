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
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace sfv::mod {

using namespace ui;
using keeper::TablePhase;

namespace {

const ImU32 kGold = IM_COL32(233, 196, 106, 255);
const ImU32 kRing = IM_COL32(240, 194, 74, 255);
const ImU32 kCream = IM_COL32(244, 241, 232, 255);
const ImU32 kSoft = IM_COL32(217, 212, 198, 255);
const ImU32 kMuted = IM_COL32(168, 162, 147, 255);
const ImU32 kGreen = IM_COL32(127, 216, 139, 255);
const ImU32 kRed = IM_COL32(255, 107, 94, 255);
const ImU32 kLine = IM_COL32(201, 168, 92, 190);

constexpr float kTablePitch = 190, kTableH = 182;
constexpr int kTablesInView = 4;
constexpr int kPlayersInView = 8;
constexpr double kCardMarginSeconds = 1.5; // the call's time the card does not show (the trip to the keeper)
constexpr int kMaxCardDelay = 10;

enum class Modal { None, Leaving, Rules, Remove };

struct State {
    int table = 0;
    float scroll = 0;
    Modal modal = Modal::None;
    bool typing = false;
    std::string text;
    int seatSide = 0;          // the seat A takes when both are open (left/right)
    bool onPlayers = false;    // RB: the cursor in the player list
    int player = 0;
    std::set<uint32_t> muted;  // their chat hidden (this session)
    uint32_t removing = 0;     // the owner's removal, to confirm
    // The your-turn card.
    int cardTable = -1;
    uint32_t cardCall = 0;     // the keeper's number for the call (a call again is a new card)
    double cardSince = 0;
    int cardDelay = 2;
    bool cardReady = false;
    // The connecting modal: the step in progress and since when.
    int handoffTable = -1, handoffStep = -1;
    double handoffSince = 0;
    // The table rules (owner).
    int rulesTable = 0, rulesRow = 0;
    std::vector<Rules> rulesEdit;
    std::vector<int> rulesPending; // tables whose new rules are still to send (one a frame)
} g;

int g_suggestedDelay = 2;
TableHandoff g_handoff;
std::map<std::string, std::string> g_names;

ImU32 Dim(ImU32 c, bool dim) { return dim ? (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * 0.45f) << IM_COL32_A_SHIFT) : c; }

const RoomMember* Member(const RoomsView& v, uint32_t id)
{
    for (const RoomMember& m : v.members) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

std::string NameOf(const RoomsView& v, uint32_t id)
{
    const RoomMember* m = Member(v, id);
    return m ? m->name : std::string("?");
}

std::string Ordinal(int n)
{
    static const char* const k[] = {"1st", "2nd", "3rd"};
    return n >= 1 && n <= 3 ? k[n - 1] : std::to_string(n) + "th";
}

void Panel(ImDrawList* d, float x0, float y0, float x1, float y1)
{
    d->AddRectFilledMultiColor(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(8, 9, 11, 214), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 214));
    d->AddLine(g_v.P(x0, y0), g_v.P(x1, y0), kLine, g_v.L(1));
    d->AddLine(g_v.P(x0, y1), g_v.P(x1, y1), kLine, g_v.L(1));
}

// A seat: the player's character and name, or "Open seat" ("Sit here as P1"
// when it is the one A would take). The art faces right: P1's sits at the
// card's left as drawn and P2's at its right mirrored, so both look at the
// score, each fading out toward it; the text is on the side toward the score
// (mock-up Room2, 2026-11-04).
void Seat(ImDrawList* d, const HubFonts& f, float x, float y, const RoomMember* m, bool ready, bool sitHere, int seat, bool dim)
{
    const ImVec2 a = g_v.P(x, y), b = g_v.P(x + 330, y + 120);
    d->AddRectFilledMultiColor(a, b, Rgba(26, 31, 38, 230), Rgba(26, 31, 38, 230), Rgba(14, 17, 21, 230), Rgba(14, 17, 21, 230));
    if (m) {
        const bool p2 = seat == 1;
        if (const TextureView* art = m->chara.empty() ? nullptr : TextureSized(CharaTex(m->chara, "PartCharacter"), g_v.L(250))) {
            // Opaque over the face, fading only in the art's last quarter toward
            // the score (user, 2026-11-04: the earlier, longer fade hid faces).
            static const FadeStop kP1[] = {{0, 1}, {0.72f, 1}, {0.84f, 0.6f}, {0.98f, 0}, {1, 0}};
            static const FadeStop kP2[] = {{0, 0}, {0.02f, 0}, {0.16f, 0.6f}, {0.28f, 1}, {1, 1}};
            const float ax = p2 ? x + 110 : x - 30; // the whole 250 px art, 30 px past the card's outer edge
            d->PushClipRect(a, b, true);
            ImageFadeX(d, art, g_v.P(ax, y - 18), g_v.P(ax + 250, y + 232), ImVec2(0, 0), ImVec2(1, 1), p2 ? kP2 : kP1, 5, p2,
                       Dim(Rgba(255, 255, 255), dim));
            d->PopClipRect();
        }
        const float tx = p2 ? x + 16 : x + 314, align = p2 ? 0.0f : 1.0f;
        const auto name = g_names.find(m->chara);
        if (name != g_names.end()) Text(d, f.body, g_v.L(14), g_v.P(tx, y + 64), Dim(kSoft, dim), name->second, align, true);
        std::string who = Upper(m->name);
        if (TextW(f.strong, 21, who) > 164) {
            while (who.size() > 1 && TextW(f.strong, 21, who + "...") > 164) {
                who.pop_back();
                while (!who.empty() && (static_cast<unsigned char>(who.back()) & 0xC0) == 0x80) who.pop_back(); // whole characters
            }
            who += "...";
        }
        Text(d, f.strong, g_v.L(21), g_v.P(tx, y + 84), Dim(kCream, dim), who, align, true);
        if (ready) Text(d, f.strong, g_v.L(13), g_v.P(tx, y + 12), Dim(kGreen, dim), "READY", align, true);
        d->AddRect(a, b, Rgba(93, 90, 82, 160), 0, 0, g_v.L(1));
        return;
    }
    if (sitHere) {
        d->AddRect(a, b, kRing, 0, 0, g_v.L(2));
        Text(d, f.strong, g_v.L(16), g_v.P(x + 165, y + 50), kGold, seat ? "Sit here as P2" : "Sit here as P1", 0.5f);
        return;
    }
    d->AddRect(a, b, Rgba(93, 90, 82, 160), 0, 0, g_v.L(1));
    Text(d, f.body, g_v.L(16), g_v.P(x + 165, y + 50), Dim(kMuted, dim), "Open seat", 0.5f);
}

void Table(ImDrawList* d, const HubFonts& f, const RoomsView& v, float y, int index, bool on, bool dim)
{
    const TableView* tv = index < static_cast<int>(v.tableViews.size()) ? &v.tableViews[index] : nullptr;
    const keeper::TableState s = tv ? tv->s : keeper::TableState{};
    const ImVec2 a = g_v.P(48, y), b = g_v.P(868, y + kTableH);
    d->AddRectFilledMultiColor(a, b, Rgba(20, 24, 30, 235), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240), Rgba(20, 24, 30, 235));
    d->AddLine(a, g_v.P(868, y), Rgba(201, 168, 92, 128), g_v.L(1));
    Text(d, f.display, g_v.L(21), g_v.P(64, y + 13), Dim(kCream, dim), "TABLE " + std::to_string(index + 1));
    Rules rules;
    const std::string line = tv && ParseRules(tv->rules, &rules) ? DescribeRules(rules) : std::string();
    // How many wait in the queue (user, 2026-11-04: not the watchers).
    const std::string queue = std::to_string(tv ? tv->queue.size() : 0) + " in queue";
    Text(d, f.body, g_v.L(16), g_v.P(852, y + 16), Dim(kSoft, dim), queue, 1);
    Text(d, f.body, g_v.L(16), g_v.P(852 - TextW(f.body, 16, queue) - 18, y + 16), Dim(kGold, dim), line, 1);
    // The seat A would take: on the focused open table, for a player not at it.
    const bool mine = s.seat[0] == v.self || s.seat[1] == v.self;
    int sitHere = -1;
    if (on && s.phase == TablePhase::Open && !mine && !dim) {
        if (!s.seat[0] && !s.seat[1]) sitHere = g.seatSide;
        else if (!s.seat[0]) sitHere = 0;
        else if (!s.seat[1]) sitHere = 1;
    }
    for (int i = 0; i < 2; i++) {
        Seat(d, f, i ? 522.0f : 64.0f, y + 48, s.seat[i] ? Member(v, s.seat[i]) : nullptr, s.ready[i] && s.phase == TablePhase::Call,
             sitHere == i, i, dim);
    }
    // The middle: the set's score, or VS; what the table is doing under it.
    const bool live = s.phase == TablePhase::Playing || s.phase == TablePhase::Connecting;
    if (live) Text(d, f.display, g_v.L(40), g_v.P(458, y + 78), Dim(Rgba(255, 255, 255), dim), std::to_string(s.wins[0]) + " - " + std::to_string(s.wins[1]), 0.5f);
    else Text(d, f.display, g_v.L(30), g_v.P(458, y + 88), Dim(Rgba(74, 68, 54), dim), "VS", 0.5f);
    const char* doing = s.phase == TablePhase::Call ? "Your turn" : s.phase == TablePhase::Connecting ? "Connecting"
                        : s.phase == TablePhase::Playing ? "Playing" : s.phase == TablePhase::After ? "Set over" : "";
    if (s.phase == TablePhase::Call && !mine) doing = "Getting ready";
    if (*doing) Text(d, f.body, g_v.L(14), g_v.P(458, y + 132), Dim(kSoft, dim), doing, 0.5f);
    if (on) d->AddRect(ImVec2(a.x + g_v.L(1.5f), a.y + g_v.L(1.5f)), ImVec2(b.x - g_v.L(1.5f), b.y - g_v.L(1.5f)), Dim(kRing, dim), 0, 0, g_v.L(3));
}

std::vector<RoomMember> PlayerOrder(const RoomsView& v)
{
    // You first, then the others oldest first.
    std::vector<RoomMember> list = v.members;
    std::stable_partition(list.begin(), list.end(), [&](const RoomMember& m) { return m.id == v.self; });
    return list;
}

// The player list: eight 50 px rows, each with the player's character fading
// in from the left under the name (mock-up Room2: SF6's lists), then the
// chat under it.

// Where each character's eyes are in its PartCharacter art drawn 200 px
// wide, marked by hand (2026-11-04), then tuned per character from the
// in-game sheet (user): a row moves the art so this point lands at kRowFace,
// clear of the name. Big characters' faces sit far right in
// their art (Zangief's at 126, under the name without this).
struct FacePoint {
    const char* code;
    int8_t x, y;
};
constexpr FacePoint kFaces[] = {
    {"ALX", 75, 68}, {"BLR", 83, 51}, {"BRD", 81, 49}, {"BSN", 86, 62}, {"CMY", 83, 50}, {"CNL", 98, 64}, {"DSM", 75, 52},
    {"ELV", 90, 74}, {"FAN", 84, 52}, {"GUL", 94, 52}, {"IBK", 98, 42}, {"JRI", 110, 65}, {"KEN", 80, 48}, {"KRN", 69, 53},
    {"LAR", 85, 51}, {"NCL", 101, 53}, {"NSH", 73, 58}, {"RMK", 102, 53}, {"RSD", 78, 53}, {"RYU", 87, 54}, {"URN", 79, 44},
    {"VEG", 91, 44}, {"Z20", 88, 49}, {"Z21", 102, 64}, {"Z22", 85, 44}, {"Z23", 89, 46}, {"Z24", 107, 46}, {"Z25", 76, 66},
    {"Z26", 83, 54}, {"Z27", 110, 56}, {"Z28", 95, 52}, {"Z29", 87, 50}, {"Z30", 84, 52}, {"Z31", 86, 50}, {"Z32", 108, 50},
    {"Z33", 90, 52}, {"Z34", 85, 53}, {"Z35", 82, 58}, {"Z36", 104, 46}, {"Z37", 116, 53}, {"Z38", 88, 53}, {"Z39", 100, 53},
    {"Z40", 75, 54}, {"Z41", 85, 49}, {"Z42", 99, 54}, {"ZGF", 126, 64},
};
constexpr int kRowFaceX = 80, kRowFaceY = 52;

ImVec2 RowArtShift(const std::string& code)
{
    for (const FacePoint& p : kFaces) {
        if (code == p.code) return ImVec2(static_cast<float>(kRowFaceX - p.x), static_cast<float>(kRowFaceY - p.y));
    }
    return ImVec2(0, 0);
}
constexpr float kRowsTop = 112, kRowH = 50, kPlayersBottom = 526, kChatTop = 538;

void Players(ImDrawList* d, const HubFonts& f, const RoomsView& v)
{
    Panel(d, 892, 80, 1552, kPlayersBottom);
    Text(d, f.body, g_v.L(14), g_v.P(910, 94), kMuted, "PLAYERS \xC2\xB7 " + std::to_string(v.members.size()) + " / 16");
    const std::vector<RoomMember> list = PlayerOrder(v);
    const int first = std::clamp(g.player - kPlayersInView + 1, 0, std::max(0, static_cast<int>(list.size()) - kPlayersInView));
    for (int i = first; i < static_cast<int>(list.size()) && i < first + kPlayersInView; i++) {
        const RoomMember& m = list[i];
        const float y = kRowsTop + (i - first) * kRowH, ty = y + 15;
        if (m.id == v.self) d->AddRectFilled(g_v.P(908, y), g_v.P(1534, y + kRowH - 1), Rgba(42, 38, 24, 160));
        // The character: head and shoulders, from the panel's edge, fading in
        // and then out under the name.
        if (const TextureView* art = m.chara.empty() ? nullptr : TextureSized(CharaTex(m.chara, "PartCharacter"), g_v.L(200))) {
            // The fade stays put (x 892..1082); the art moves under it to
            // frame the face.
            static const FadeStop kRow[] = {{0, 0}, {0.06f, 0}, {0.22f, 1}, {0.38f, 1}, {0.60f, 0.5f}, {0.86f, 0}, {1, 0}};
            const ImVec2 shift = RowArtShift(m.chara);
            d->PushClipRect(g_v.P(893, y), g_v.P(1534, y + kRowH - 1), true);
            ImageFadeX(d, art, g_v.P(872 + shift.x, y - 28 + shift.y), g_v.P(1072 + shift.x, y + 172 + shift.y), ImVec2(0, 0), ImVec2(1, 1),
                       kRow, 7, false, IM_COL32_WHITE, g_v.P(892, 0).x, g_v.P(1082, 0).x);
            d->PopClipRect();
        }
        if (g.onPlayers && i == g.player) d->AddRect(g_v.P(908, y), g_v.P(1534, y + kRowH - 1), kRing, 0, 0, g_v.L(2));
        d->AddLine(g_v.P(908, y + kRowH - 1), g_v.P(1534, y + kRowH - 1), Rgba(255, 255, 255, 14), g_v.L(1));
        float x = 994;
        if (m.owner) {
            DrawCrown(d, x, ty + 15, 0.85f, kGold);
            x += 20;
        }
        std::string who = Upper(m.name);
        const float room = 1162 - x - (m.id == v.self ? TextW(f.body, 16, "(you)") + 6 : 0);
        if (TextW(f.strong, 16, who) > room) {
            while (who.size() > 1 && TextW(f.strong, 16, who + "...") > room) {
                who.pop_back();
                while (!who.empty() && (static_cast<unsigned char>(who.back()) & 0xC0) == 0x80) who.pop_back(); // whole characters
            }
            who += "...";
        }
        Text(d, f.strong, g_v.L(16), g_v.P(x, ty), kCream, who, 0, true);
        x += TextW(f.strong, 16, who) + 6;
        if (m.id == v.self) Text(d, f.body, g_v.L(16), g_v.P(x, ty), kMuted, "(you)", 0, true);
        if (g.muted.count(m.id)) Text(d, f.body, g_v.L(14), g_v.P(x, ty + 1), kMuted, "muted", 0, true);
        const std::string place = RoomsPlace(v, m.id);
        Text(d, f.body, g_v.L(16), g_v.P(1170, ty), place == "Idle" ? kSoft : kGreen, place);
        // The round trip to them (green under 60 ms, gold under 120, red
        // after), with a cloud when it goes through a relay.
        if (m.pingMs >= 0) {
            const ImU32 col = m.pingMs < 60 ? kGreen : m.pingMs < 120 ? kGold : kRed;
            const std::string ms = std::to_string(m.pingMs) + " ms";
            Text(d, f.strong, g_v.L(16), g_v.P(1418, ty), col, ms, 1);
            if (m.relayed) IconCloud(d, g_v.P(1418 - TextW(f.strong, 16, ms) - 16, ty + 10), g_v.L(1.6f), col);
        }
        if (m.link) {
            const char* what = m.link == 1 ? "Wired" : "Wi-Fi";
            Text(d, f.body, g_v.L(15), g_v.P(1512, ty + 1), kSoft, what, 1);
            IconLinkType(d, g_v.P(1512 - TextW(f.body, 15, what) - 14, ty + 10), g_v.L(1.4f), m.link, kSoft);
        }
    }
    if (static_cast<int>(list.size()) > kPlayersInView) {
        // A thin scrollbar (the rest: RB, then up and down).
        const float top = kRowsTop + 4, h = kPlayersInView * kRowH - 8, n = static_cast<float>(list.size());
        d->AddRectFilled(g_v.P(1541, top), g_v.P(1546, top + h), Rgba(255, 255, 255, 20), g_v.L(3));
        d->AddRectFilled(g_v.P(1541, top + h * first / n), g_v.P(1546, top + h * (first + kPlayersInView) / n), Rgba(233, 196, 106, 190), g_v.L(3));
    }
}

void Chat(ImDrawList* d, const HubFonts& f, const RoomsView& v, bool* clickedBox, bool* clickedSend)
{
    Panel(d, 892, kChatTop, 1552, 832);
    Text(d, f.body, g_v.L(14), g_v.P(910, kChatTop + 11), kMuted, "ROOM CHAT");
    // The newest at the bottom, above the box; older lines go up and out.
    const float wrap = 620, size = 16;
    float bottom = 773;
    d->PushClipRect(g_v.P(900, kChatTop + 32), g_v.P(1544, 775), true);
    for (int i = static_cast<int>(v.chat.size()) - 1; i >= 0 && bottom > kChatTop + 32; i--) {
        const ChatLine& line = v.chat[i];
        if (line.from && g.muted.count(line.from)) continue;
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

void Dialog(ImDrawList* d, const HubFonts& f, const std::string& kicker, const std::string& title, const std::string& text, ImU32 line = kLine)
{
    d->AddRectFilled(g_v.P(0, 0), g_v.P(1600, 844), Rgba(0, 0, 0, 150));
    const ImVec2 a = g_v.P(470, 330), b = g_v.P(1130, 500);
    d->AddRectFilledMultiColor(a, b, Rgba(26, 31, 38, 250), Rgba(26, 31, 38, 250), Rgba(11, 14, 18, 250), Rgba(11, 14, 18, 250));
    d->AddLine(a, g_v.P(1130, 330), line, g_v.L(2));
    d->AddLine(g_v.P(470, 500), b, line, g_v.L(2));
    Text(d, f.strong, g_v.L(16), g_v.P(504, 362), kGold, kicker);
    Text(d, f.display, g_v.L(28), g_v.P(504, 396), kCream, title);
    d->AddText(f.body, g_v.L(17), g_v.P(504, 452), kSoft, text.c_str(), nullptr, g_v.L(590));
}

std::string LeaveText(const RoomsView& v)
{
    std::string text;
    for (const TableView& t : v.tableViews) {
        const auto at = std::find(t.queue.begin(), t.queue.end(), v.self);
        if (at != t.queue.end() || t.s.seat[0] == v.self || t.s.seat[1] == v.self) {
            text = "You lose your place at Table " + std::to_string(&t - v.tableViews.data() + 1) + ". ";
        }
    }
    if (v.owner == v.self) {
        // The longest present after you gets it (the keeper's rule).
        if (v.members.size() <= 1) return text + "You are the last one here: the room closes.";
        for (const RoomMember& m : v.members) {
            if (m.id != v.self) return text + m.name + " becomes the owner.";
        }
    }
    return text.empty() ? "You can come back with the code while the room is open." : text;
}

// The your-turn card: the two players, the timer, the delay for this match.
void Card(ImDrawList* d, const HubFonts& f, const RoomsView& v, int t)
{
    const keeper::TableState& s = v.tableViews[t].s;
    d->AddRectFilled(g_v.P(0, 0), g_v.P(1600, 844), Rgba(0, 0, 0, 160));
    const float x0 = 380, y0 = 210, w = 840, h = 430;
    const ImVec2 a = g_v.P(x0, y0), b = g_v.P(x0 + w, y0 + h);
    d->AddRectFilled(a, b, Rgba(11, 14, 18, 250));
    d->PushClipRect(a, b, true);
    if (const TextureView* bg = Texture(StageTex("CHN", "StageSelectBG"))) {
        Image(d, bg, g_v.P(x0 - 200, y0), g_v.P(x0 + w + 200, y0 + 310), ImVec2(0, 0), ImVec2(1, 1), Rgba(120, 120, 120));
    }
    d->AddRectFilledMultiColor(g_v.P(x0, y0 + 150), g_v.P(x0 + w, y0 + h), Rgba(11, 14, 18, 0), Rgba(11, 14, 18, 0), Rgba(11, 14, 18, 250),
                               Rgba(11, 14, 18, 250));
    d->PopClipRect();
    d->AddLine(a, g_v.P(x0 + w, y0), kRing, g_v.L(2));
    d->AddLine(g_v.P(x0, y0 + h), b, kRing, g_v.L(2));
    Text(d, f.strong, g_v.L(16), g_v.P(x0 + 30, y0 + 22), kGold, "YOUR TURN \xC2\xB7 TABLE " + std::to_string(t + 1));
    Rules rules;
    if (ParseRules(v.tableViews[t].rules, &rules)) Text(d, f.body, g_v.L(15), g_v.P(x0 + w - 30, y0 + 23), kSoft, DescribeRules(rules), 1);
    for (int i = 0; i < 2; i++) {
        const RoomMember* m = Member(v, s.seat[i]);
        Seat(d, f, i ? x0 + w - 30 - 330 : x0 + 30, y0 + 60, m, s.ready[i], false, i, false);
        Text(d, f.strong, g_v.L(14), g_v.P(i ? x0 + w - 30 - 330 : x0 + 30, y0 + 186), kMuted, i ? "P2" : "P1 \xC2\xB7 hosts the match");
    }
    Text(d, f.display, g_v.L(36), g_v.P(x0 + w / 2, y0 + 98), Rgba(255, 255, 255), "VS", 0.5f);
    // The time left: the keeper's call, from when it showed here (also while
    // this game was still coming back from a battle), less a little for the
    // trip to the keeper.
    const double total = std::max(1.0, (s.callSeconds ? s.callSeconds : 10) - kCardMarginSeconds);
    const double left = std::clamp(total - (RoomsClockMs() - v.tableViews[t].callSeenMs) / 1000.0, 0.0, total);
    d->AddRectFilled(g_v.P(x0 + 30, y0 + 218), g_v.P(x0 + w - 30, y0 + 222), Rgba(74, 68, 54));
    d->AddRectFilled(g_v.P(x0 + 30, y0 + 218), g_v.P(x0 + 30 + (w - 60) * static_cast<float>(left / total), y0 + 222), kGold);
    const int mySeat = s.seat[0] == v.self ? 0 : 1;
    if (g.cardReady || s.ready[mySeat]) {
        Text(d, f.display, g_v.L(26), g_v.P(x0 + w / 2, y0 + 262), kGreen, "READY", 0.5f);
        Text(d, f.body, g_v.L(17), g_v.P(x0 + w / 2, y0 + 304), kSoft, "Waiting for " + NameOf(v, s.seat[1 - mySeat]) + ".", 0.5f);
        return;
    }
    Text(d, f.display, g_v.L(26), g_v.P(x0 + w / 2, y0 + 246), Rgba(255, 255, 255),
         left > 0 ? "Ready in " + std::to_string(static_cast<int>(left + 0.99)) : std::string("Out of time"), 0.5f);
    // The delay for this match.
    const float ry = y0 + 300;
    d->AddRectFilled(g_v.P(x0 + 220, ry), g_v.P(x0 + w - 220, ry + 48), Rgba(20, 24, 30, 230));
    Text(d, f.strong, g_v.L(18), g_v.P(x0 + 240, ry + 13), kCream, "Input delay");
    const std::string frames = std::to_string(g.cardDelay) + (g.cardDelay == 1 ? " frame" : " frames");
    Text(d, f.body, g_v.L(18), g_v.P(x0 + w - 300, ry + 13), kCream, frames, 0.5f);
    d->AddTriangleFilled(g_v.P(x0 + w - 368, ry + 18), g_v.P(x0 + w - 368, ry + 30), g_v.P(x0 + w - 378, ry + 24), kGold);
    d->AddTriangleFilled(g_v.P(x0 + w - 232, ry + 18), g_v.P(x0 + w - 222, ry + 24), g_v.P(x0 + w - 232, ry + 30), kGold);
    Text(d, f.body, g_v.L(15), g_v.P(x0 + w / 2, ry + 62), kMuted, "No answer in time: back of the queue.", 0.5f);
}

// A name cut to fit `width` (ui units) at this size, with an ellipsis.
std::string Fit(ImFont* font, float size, std::string text, float width)
{
    if (TextW(font, size, text) <= width) return text;
    while (!text.empty() && TextW(font, size, text + "...") > width) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back(); // whole characters
    }
    return text + "...";
}

// The connecting modal's steps: 2 done, 1 in progress, 0 still to come.
struct HandoffStep {
    std::string text;
    int state;
};

std::vector<HandoffStep> HandoffSteps(const HubFonts& f, const RoomsView& v, const keeper::TableState& s, const TableHandoff& h)
{
    const bool host = s.seat[0] == v.self;
    const std::string other = Fit(f.body, 17, NameOf(v, s.seat[host ? 1 : 0]), 190);
    const bool room = h.jumping || (h.session && (!host || h.roomOpen));
    const bool linked = h.jumping || h.linked;
    const bool loaded = h.jumping || (h.preloaded[0] && h.preloaded[1]);
    std::vector<HandoffStep> steps;
    steps.push_back({"Both ready", 2});
    steps.push_back({room ? (host ? std::string("Match room open") : other + " opened the match room")
                          : (host ? std::string("Opening the match room") : "Waiting for " + other + "'s match room"),
                     room ? 2 : 1});
    std::string connected = "Connected";
    if (h.pingMs) connected += " \xC2\xB7 " + std::to_string(h.pingMs) + " ms";
    if (h.path) connected += h.path == 2 ? ", relayed" : ", peer to peer";
    steps.push_back({linked ? connected : host ? "Waiting for " + other + " to join" : "Connecting to " + other, linked ? 2 : room ? 1 : 0});
    const bool mine = h.preloaded[host ? 0 : 1];
    steps.push_back({loaded ? std::string("Characters loaded") : mine ? "Waiting for " + other + "'s game" : std::string("Loading the characters"),
                     loaded ? 2 : linked ? 1 : 0});
    steps.push_back({"Starting the match", h.jumping ? 1 : 0});
    return steps;
}

// Over the room from both players' Ready to the match loading (mock-up board
// 6): who against whom, each step of the way in, the set's rules.
void HandoffModal(ImDrawList* d, const HubFonts& f, const RoomsView& v, int t, const std::vector<HandoffStep>& steps, double stepSeconds)
{
    const keeper::TableState& s = v.tableViews[t].s;
    const uint32_t otherId = s.seat[s.seat[0] == v.self ? 1 : 0];
    d->AddRectFilled(g_v.P(0, 0), g_v.P(1600, 844), Rgba(0, 0, 0, 160));
    const float x0 = 520, y0 = 250, w = 560, h = 350;
    const ImVec2 a = g_v.P(x0, y0), b = g_v.P(x0 + w, y0 + h);
    d->AddRectFilledMultiColor(a, b, Rgba(26, 31, 38, 250), Rgba(26, 31, 38, 250), Rgba(11, 14, 18, 250), Rgba(11, 14, 18, 250));
    d->AddLine(a, g_v.P(x0 + w, y0), kRing, g_v.L(2));
    d->AddLine(g_v.P(x0, y0 + h), b, kRing, g_v.L(2));
    // The opponent's character, head and shoulders.
    float tx = x0 + 28;
    const RoomMember* other = Member(v, otherId);
    // The portrait shows 0.24 of the art's width in 48 px: the art at 200 px.
    if (const TextureView* art = other && !other->chara.empty() ? TextureSized(CharaTex(other->chara, "PartCharacter"), g_v.L(200)) : nullptr) {
        Image(d, art, g_v.P(x0 + 26, y0 + 26), g_v.P(x0 + 74, y0 + 98), ImVec2(0.38f, 0.06f), ImVec2(0.62f, 0.42f), Rgba(255, 255, 255));
        d->AddRect(g_v.P(x0 + 26, y0 + 26), g_v.P(x0 + 74, y0 + 98), Rgba(93, 90, 82, 160), 0, 0, g_v.L(1));
        tx = x0 + 92;
    }
    const std::string kicker = "TABLE " + std::to_string(t + 1) + " \xC2\xB7 " +
                               (g_handoff.played > 0 ? "GAME " + std::to_string(g_handoff.played + 1) : std::string("YOUR TURN"));
    Text(d, f.strong, g_v.L(16), g_v.P(tx, y0 + 34), kGold, kicker);
    Text(d, f.display, g_v.L(28), g_v.P(tx, y0 + 58), kCream, Fit(f.display, 28, "vs " + Upper(NameOf(v, otherId)), x0 + w - 28 - tx));
    float y = y0 + 132;
    for (const HandoffStep& st : steps) {
        const ImVec2 c = g_v.P(x0 + 42, y + 11);
        const ImU32 ring = st.state == 2 ? kGreen : st.state == 1 ? kRing : Rgba(93, 90, 82, 200);
        d->AddCircle(c, g_v.L(12), ring, 24, g_v.L(2));
        if (st.state == 2) {
            const ImVec2 tick[] = {g_v.P(x0 + 36, y + 11), g_v.P(x0 + 40.5f, y + 16), g_v.P(x0 + 48.5f, y + 6)};
            d->AddPolyline(tick, 3, kGreen, 0, g_v.L(2.2f));
        }
        else if (st.state == 1) {
            // A dot going round: still working.
            const float turn = static_cast<float>(ImGui::GetTime() * 4.0);
            d->AddCircleFilled(g_v.P(x0 + 42 + 7 * std::cos(turn), y + 11 + 7 * std::sin(turn)), g_v.L(2.6f), kRing);
        }
        std::string text = st.text;
        if (st.state == 1 && stepSeconds >= 3) text += " \xC2\xB7 " + std::to_string(static_cast<int>(stepSeconds)) + " s";
        Text(d, f.body, g_v.L(17), g_v.P(x0 + 68, y), st.state == 0 ? kMuted : st.state == 1 ? kCream : kSoft, text);
        y += 36;
    }
    Rules rules;
    if (ParseRules(v.tableViews[t].rules, &rules)) Text(d, f.body, g_v.L(15), g_v.P(x0 + 28, y0 + h - 36), kMuted, DescribeRules(rules));
}

const char* const kRuleRows[] = {"First to", "Rounds per game", "Round timer", "Rotation", "Stays up to"};

std::string RuleValue(const Rules& r, int row)
{
    switch (row) {
    case 0: return std::to_string(r.firstTo) + (r.firstTo == 1 ? " game" : " games");
    case 1: return r.bestOf == 1 ? "One round" : "Best of " + std::to_string(r.bestOf);
    case 2: return std::to_string(r.timer) + " seconds";
    case 3: return r.rotation == Rotation::WinnerStays ? "Winner stays" : r.rotation == Rotation::LoserStays ? "Loser stays" : "Round robin";
    default: return r.rotation == Rotation::RoundRobin ? "-" : r.staysLimit ? std::to_string(r.staysLimit) + " sets in a row" : "No limit";
    }
}

void StepRule(Rules* r, int row, int dir)
{
    auto cycle = [&](int value, std::initializer_list<int> values) {
        std::vector<int> list(values);
        auto at = std::find(list.begin(), list.end(), value);
        int i = at == list.end() ? 0 : static_cast<int>(at - list.begin());
        i = (i + dir + static_cast<int>(list.size())) % static_cast<int>(list.size());
        return list[i];
    };
    switch (row) {
    case 0: r->firstTo = static_cast<uint8_t>(std::clamp(r->firstTo + dir, 1, 20)); break;
    case 1: r->bestOf = static_cast<uint8_t>(cycle(r->bestOf, {3, 5, 1})); break;
    case 2: r->timer = static_cast<uint8_t>(cycle(r->timer, {99, 60, 30})); break;
    case 3: r->rotation = static_cast<Rotation>(cycle(static_cast<int>(r->rotation), {0, 1, 2})); break;
    default:
        if (r->rotation != Rotation::RoundRobin) r->staysLimit = static_cast<uint8_t>(cycle(r->staysLimit, {3, 5, 0}));
        break;
    }
}

void RulesModal(ImDrawList* d, const HubFonts& f, const RoomsView& v)
{
    d->AddRectFilled(g_v.P(0, 0), g_v.P(1600, 844), Rgba(0, 0, 0, 160));
    const float x0 = 400, y0 = 150, w = 800, h = 560;
    d->AddRectFilledMultiColor(g_v.P(x0, y0), g_v.P(x0 + w, y0 + h), Rgba(26, 31, 38, 250), Rgba(26, 31, 38, 250), Rgba(11, 14, 18, 250),
                               Rgba(11, 14, 18, 250));
    d->AddLine(g_v.P(x0, y0), g_v.P(x0 + w, y0), kLine, g_v.L(2));
    d->AddLine(g_v.P(x0, y0 + h), g_v.P(x0 + w, y0 + h), kLine, g_v.L(2));
    Text(d, f.display, g_v.L(30), g_v.P(x0 + 30, y0 + 22), kCream, "TABLE RULES");
    // A tab per table (LB and RB).
    float tx = x0 + 30;
    const int tables = static_cast<int>(g.rulesEdit.size());
    for (int t = 0; t < tables; t++) {
        const std::string label = "Table " + std::to_string(t + 1);
        const float tw = TextW(f.strong, 16, label) + 24;
        const bool on = t == g.rulesTable;
        d->AddRectFilled(g_v.P(tx, y0 + 74), g_v.P(tx + tw, y0 + 104), on ? kGold : Rgba(20, 24, 30), g_v.L(15));
        Text(d, f.strong, g_v.L(16), g_v.P(tx + 12, y0 + 79), on ? Rgba(21, 19, 13) : kSoft, label);
        tx += tw + 8;
    }
    const Rules& r = g.rulesEdit[g.rulesTable];
    for (int row = 0; row < 5; row++) {
        const float y = y0 + 130 + row * 62.0f;
        const bool on = row == g.rulesRow;
        if (on) d->AddRectFilledMultiColor(g_v.P(x0 + 30, y), g_v.P(x0 + w - 30, y + 54), Rgba(240, 200, 90, 110), Rgba(8, 9, 11, 150),
                                           Rgba(8, 9, 11, 150), Rgba(240, 200, 90, 110));
        else d->AddRectFilled(g_v.P(x0 + 30, y), g_v.P(x0 + w - 30, y + 54), Rgba(20, 24, 30, 179));
        Text(d, f.strong, g_v.L(19), g_v.P(x0 + 52, y + 15), kCream, kRuleRows[row]);
        Text(d, f.body, g_v.L(19), g_v.P(x0 + 560, y + 15), kCream, RuleValue(r, row), 0.5f);
        if (on) {
            d->AddTriangleFilled(g_v.P(x0 + 400, y + 21), g_v.P(x0 + 400, y + 33), g_v.P(x0 + 390, y + 27), kGold);
            d->AddTriangleFilled(g_v.P(x0 + 720, y + 21), g_v.P(x0 + 730, y + 27), g_v.P(x0 + 720, y + 33), kGold);
        }
    }
    Rules now;
    const std::string current = g.rulesTable < static_cast<int>(v.tableRules.size()) ? v.tableRules[g.rulesTable] : "";
    const bool changed = !(ParseRules(current, &now) && now == r);
    Text(d, f.body, g_v.L(16), g_v.P(x0 + 30, y0 + h - 44), changed ? kGold : kMuted,
         changed ? "Changed: applies from this table's next set." : "Everyone sees these on the table.");
}

void Bar(ImDrawList* d, const HubFonts& f, const std::string& lead, ImU32 leadCol, const std::string& text,
         const std::vector<std::pair<const char*, std::string>>& prompts, int* clicked)
{
    d->AddRectFilled(g_v.P(0, 844), g_v.P(1600, 900), Rgba(8, 9, 11, 245));
    d->AddLine(g_v.P(0, 844), g_v.P(1600, 844), kLine, g_v.L(1));
    float x = 48;
    if (!lead.empty()) {
        Text(d, f.strong, g_v.L(15), g_v.P(x, 863), leadCol, lead);
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

void Request(RoomScreenInput* in, const char* kind, std::vector<std::string> args = {})
{
    in->request = kind;
    in->args = std::move(args);
}

} // namespace

void ResetRoomScreen() { g = State{}; }

void SetCardDelay(int frames) { g_suggestedDelay = std::clamp(frames, 0, kMaxCardDelay); }

void SetTableHandoff(const TableHandoff& handoff) { g_handoff = handoff; }

void SetCharaNames(std::vector<std::pair<std::string, std::string>> codeToName)
{
    g_names.clear();
    for (auto& [code, name] : codeToName) g_names[code] = name;
}

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
    const std::vector<RoomMember> players = PlayerOrder(v);
    g.player = std::clamp(g.player, 0, std::max(0, static_cast<int>(players.size()) - 1));

    // Where this player is: their table's call brings the card up.
    int callTable = -1, mySeat = -1;
    for (int t = 0; t < static_cast<int>(v.tableViews.size()); t++) {
        const keeper::TableState& s = v.tableViews[t].s;
        const int seat = s.seat[0] == v.self ? 0 : s.seat[1] == v.self ? 1 : -1;
        if (seat >= 0 && s.phase == TablePhase::Call && !paused) {
            callTable = t;
            mySeat = seat;
        }
    }
    // This player's table from both Ready to the match: the connecting modal.
    int handoffTable = -1;
    for (int t = 0; t < static_cast<int>(v.tableViews.size()) && callTable < 0; t++) {
        const keeper::TableState& s = v.tableViews[t].s;
        if ((s.seat[0] == v.self || s.seat[1] == v.self) && (s.phase == TablePhase::Connecting || s.phase == TablePhase::Playing)) {
            handoffTable = t;
        }
    }
    std::vector<HandoffStep> steps;
    double stepSeconds = 0;
    if (handoffTable >= 0) {
        steps = HandoffSteps(f, v, v.tableViews[handoffTable].s, g_handoff);
        int now = static_cast<int>(steps.size()) - 1;
        for (int i = 0; i < static_cast<int>(steps.size()); i++) {
            if (steps[i].state == 1) {
                now = i;
                break;
            }
        }
        if (handoffTable != g.handoffTable || now != g.handoffStep) g.handoffSince = ImGui::GetTime();
        g.handoffStep = now;
        stepSeconds = ImGui::GetTime() - g.handoffSince;
        g.typing = false;
    }
    g.handoffTable = handoffTable;
    // Cancel: until the match loads, and only before the set's first game.
    const bool canCancel = handoffTable >= 0 && !g_handoff.jumping && g_handoff.played == 0;
    const uint32_t callNumber = callTable >= 0 ? v.tableViews[callTable].s.call : 0;
    if (callTable != g.cardTable || callNumber != g.cardCall) {
        // A new call, also the same table calling the same two again: Ready
        // as the keeper has it (it keeps a Ready across a call again).
        if (callTable != g.cardTable) g.cardDelay = g_suggestedDelay;
        g.cardTable = callTable;
        g.cardCall = callNumber;
        g.cardSince = ImGui::GetTime();
        g.cardReady = callTable >= 0 && v.tableViews[callTable].s.ready[mySeat];
        if (callTable >= 0) g.typing = false;
    }

    if (const TextureView* bg = Texture(StageTex("CHN", "StageSelectBG"))) {
        Image(d, bg, g_v.P(-500, -40), g_v.P(2100, 720), ImVec2(0, 0), ImVec2(1, 1), Rgba(80, 80, 80));
    }
    d->AddRectFilledMultiColor(g_v.P(0, 315), g_v.P(1600, 630), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 210), Rgba(6, 8, 11, 210));
    d->AddRectFilledMultiColor(g_v.P(0, 630), g_v.P(1600, 900), Rgba(6, 8, 11, 210), Rgba(6, 8, 11, 210), Rgba(6, 8, 11), Rgba(6, 8, 11));

    const TableView* focused = g.table < static_cast<int>(v.tableViews.size()) ? &v.tableViews[g.table] : nullptr;
    const keeper::TableState fs = focused ? focused->s : keeper::TableState{};
    const bool atFocused = fs.seat[0] == v.self || fs.seat[1] == v.self;
    const auto inQueue = focused ? std::find(focused->queue.begin(), focused->queue.end(), v.self) : std::vector<uint32_t>::const_iterator{};
    const bool queuedHere = focused && inQueue != focused->queue.end();

    // ---- input (the card, a dialog or typing take it first) ----
    bool send = false;
    if (g.cardTable >= 0) {
        if (!g.cardReady) {
            if (Left()) g.cardDelay = std::max(0, g.cardDelay - 1);
            if (Right()) g.cardDelay = std::min(kMaxCardDelay, g.cardDelay + 1);
            if (Confirm()) {
                g.cardReady = true;
                Request(in, "ready", {std::to_string(g.cardTable)});
                in->delay = g.cardDelay;
                if (!v.tableNotice.empty()) in->clearNotice = true;
            }
            else if (Back()) {
                Request(in, "decline", {std::to_string(g.cardTable)});
            }
        }
    }
    else if (handoffTable >= 0) {
        if (canCancel && Back()) in->cancelHandoff = true;
    }
    else if (g.modal == Modal::Leaving) {
        if (Confirm()) {
            in->leave = true;
            g.modal = Modal::None;
        }
        else if (Back()) g.modal = Modal::None;
    }
    else if (g.modal == Modal::Remove) {
        if (Confirm()) {
            Request(in, "kick", {std::to_string(g.removing)});
            g.modal = Modal::None;
        }
        else if (Back()) g.modal = Modal::None;
    }
    else if (g.modal == Modal::Rules) {
        if (Pressed({ImGuiKey_GamepadL1}, false)) g.rulesTable = (g.rulesTable + tables - 1) % tables;
        if (Pressed({ImGuiKey_GamepadR1, ImGuiKey_Tab}, false)) g.rulesTable = (g.rulesTable + 1) % tables;
        if (Up()) g.rulesRow = std::max(0, g.rulesRow - 1);
        if (Down()) g.rulesRow = std::min(4, g.rulesRow + 1);
        if (Left()) StepRule(&g.rulesEdit[g.rulesTable], g.rulesRow, -1);
        if (Right()) StepRule(&g.rulesEdit[g.rulesTable], g.rulesRow, 1);
        if (Confirm() || Pressed({ImGuiKey_GamepadStart}, false)) {
            // Every table whose rules changed (sent one a frame, below).
            for (int t = 0; t < tables; t++) {
                Rules now;
                const std::string current = t < static_cast<int>(v.tableRules.size()) ? v.tableRules[t] : "";
                if (!(ParseRules(current, &now) && now == g.rulesEdit[t])) g.rulesPending.push_back(t);
            }
            g.modal = Modal::None;
        }
        else if (Back()) g.modal = Modal::None;
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
        // On a pad, Back + Y starts typing (a Deck's keyboard, Steam + X,
        // types into it); Back alone, let go without Y, copies the code.
        static bool backHeld = false, usedY = false;
        const bool back = g_inputOn && ImGui::IsKeyDown(ImGuiKey_GamepadBack);
        const bool y = Pressed({ImGuiKey_GamepadFaceUp}, false);
        if (back && y) {
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
        if (Pressed({ImGuiKey_GamepadR1}, false)) g.onPlayers = !g.onPlayers;
        if (g.onPlayers) {
            if (Up()) g.player = std::max(0, g.player - 1);
            if (Down()) g.player = std::min(static_cast<int>(players.size()) - 1, g.player + 1);
            const uint32_t who = players.empty() ? 0 : players[g.player].id;
            if (Confirm() && who && who != v.self) {
                if (!g.muted.erase(who)) g.muted.insert(who);
            }
            if (Pressed({ImGuiKey_GamepadFaceLeft}, false) && who && who != v.self && v.owner == v.self && !paused) {
                g.removing = who;
                g.modal = Modal::Remove;
            }
            if (Back()) g.onPlayers = false;
        }
        else {
            if (Up()) g.table = std::max(0, g.table - 1);
            if (Down()) g.table = std::min(tables - 1, g.table + 1);
            if (fs.phase == TablePhase::Open && !fs.seat[0] && !fs.seat[1]) {
                if (Left()) g.seatSide = 0;
                if (Right()) g.seatSide = 1;
            }
            // Enter is the chat's here (it starts typing below): A, Space or a
            // click confirm.
            const bool enter = Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false);
            const bool confirm = Confirm() && !enter;
            if (!v.tableNotice.empty() && confirm) {
                in->clearNotice = true; // A reads the notice, nothing more
            }
            else if (!paused && confirm) {
                const std::string t = std::to_string(g.table);
                if (fs.phase == TablePhase::Open && atFocused) Request(in, "stand");
                else if (fs.phase == TablePhase::Open && (!fs.seat[0] || !fs.seat[1])) {
                    const int seat = !fs.seat[0] && !fs.seat[1] ? g.seatSide : !fs.seat[0] ? 0 : 1;
                    Request(in, "seat", {t, std::to_string(seat)});
                }
                else if (queuedHere) Request(in, "unqueue");
                else if (!atFocused) Request(in, "queue", {t});
            }
            if (y && !back) in->myCharacter = true;
            if (Pressed({ImGuiKey_GamepadStart}, false) && v.owner == v.self && !paused) {
                g.modal = Modal::Rules;
                g.rulesTable = g.table;
                g.rulesRow = 0;
                g.rulesEdit.assign(tables, Rules{});
                for (int t = 0; t < tables && t < static_cast<int>(v.tableRules.size()); t++) ParseRules(v.tableRules[t], &g.rulesEdit[t]);
            }
            if (enter) g.typing = true;
            else if (Back()) g.modal = Modal::Leaving;
        }
        if (ImGui::GetIO().KeyCtrl && Pressed({ImGuiKey_C}, false)) {
            ImGui::SetClipboardText(v.room.c_str());
            in->copiedCode = true;
        }
    }
    if (in->request.empty() && !g.rulesPending.empty() && g.rulesPending.front() < static_cast<int>(g.rulesEdit.size())) {
        const int t = g.rulesPending.front();
        g.rulesPending.erase(g.rulesPending.begin());
        Request(in, "rules", {std::to_string(t), FormatRules(g.rulesEdit[t])});
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
    if (g.modal == Modal::None && Clicked() && MouseIn(g_v.P(x, 32), g_v.P(x + 34 + TextW(f.body, 17, "Copy code"), 68))) {
        ImGui::SetClipboardText(v.room.c_str());
        in->copiedCode = true;
    }

    // ---- the tables ----
    d->PushClipRect(g_v.P(48, 80), g_v.P(880, 832), true);
    for (int t = 0; t < tables; t++) {
        const float ty = 80 + (t - g.scroll) * kTablePitch;
        if (ty > 832 || ty + kTableH < 80) continue;
        Table(d, f, v, ty, t, t == g.table && !g.onPlayers, paused);
        if (g.modal == Modal::None && !g.typing && g.cardTable < 0 && Clicked() && MouseIn(g_v.P(48, ty), g_v.P(868, ty + kTableH))) {
            g.table = t;
            g.onPlayers = false;
        }
    }
    d->PopClipRect();

    // ---- players and chat ----
    Players(d, f, v);
    bool clickedBox = false, clickedSend = false;
    Chat(d, f, v, &clickedBox, &clickedSend);
    if (g.modal == Modal::None && g.cardTable < 0) {
        if (clickedBox) g.typing = true;
        if (clickedSend && g.typing) send = true;
    }
    if (send) {
        if (!g.text.empty()) in->chat = g.text;
        g.text.clear();
        g.typing = false;
    }

    // ---- the stripe, and the modals over everything ----
    int clicked = -1;
    std::vector<std::pair<const char*, std::string>> prompts;
    std::string lead, text;
    ImU32 leadCol = kGold;
    // This player's table, after its set or connecting.
    int myTable = -1;
    for (int t = 0; t < static_cast<int>(v.tableViews.size()); t++) {
        const keeper::TableState& s = v.tableViews[t].s;
        if (s.seat[0] == v.self || s.seat[1] == v.self || (s.phase == TablePhase::After && (s.lastWinner == v.self || s.lastLoser == v.self))) myTable = t;
    }
    const keeper::TableState* ms = myTable >= 0 ? &v.tableViews[myTable].s : nullptr;
    if (g.cardTable >= 0) {
        prompts = g.cardReady ? std::vector<std::pair<const char*, std::string>>{}
                              : std::vector<std::pair<const char*, std::string>>{{"button_a", "Ready"}, {"button_b", "Leave queue"}};
        text = g.cardReady ? "Ready. The match opens once both are." : "Left and right: the input delay for this match.";
        // The last call fell through (the opponent left during the handoff):
        // say so here, since this card covers the notice.
        if (!v.tableNotice.empty() && !g.cardReady) {
            lead = "NO MATCH";
            leadCol = kRed;
            text = v.tableNotice;
        }
    }
    else if (g.modal == Modal::Leaving) {
        prompts = {{"button_a", "Leave"}, {"button_b", "Stay"}};
    }
    else if (g.modal == Modal::Remove) {
        prompts = {{"button_a", "Remove"}, {"button_b", "Cancel"}};
    }
    else if (g.modal == Modal::Rules) {
        text = "LB and RB: the table. Up and down: the rule; left and right: its value.";
        prompts = {{"button_a", "Done"}, {"button_b", "Cancel"}};
    }
    else if (g.typing) {
        text = "Enter sends, Escape or B stops. On a Steam Deck, Steam + X opens the keyboard.";
    }
    else if (paused) {
        lead = "ROOM PAUSED";
        text = "The keeper is away; it comes back by itself. Chat carries on.";
        prompts = {{"button_b", "Leave"}};
    }
    else if (handoffTable >= 0) {
        const keeper::TableState& s = v.tableViews[handoffTable].s;
        lead = "TABLE " + std::to_string(handoffTable + 1);
        text = g_handoff.jumping ? "The match is loading." : "Getting the match ready with " + NameOf(v, s.seat[s.seat[0] == v.self ? 1 : 0]) + ".";
        if (canCancel) prompts = {{"button_b", "Cancel"}};
    }
    else if (ms && ms->phase == TablePhase::After && (ms->lastWinner == v.self || ms->lastLoser == v.self)) {
        const bool won = ms->lastWinner == v.self;
        lead = won ? "YOU WON THE SET" : "YOU LOST THE SET";
        leadCol = won ? kGreen : kSoft;
        text = std::to_string(won ? ms->lastScore[0] : ms->lastScore[1]) + " - " + std::to_string(won ? ms->lastScore[1] : ms->lastScore[0]) + ". ";
        const std::vector<uint32_t>& q = v.tableViews[myTable].queue;
        const auto at = std::find(q.begin(), q.end(), v.self);
        if (ms->seat[0] == v.self || ms->seat[1] == v.self) text += "You stay on Table " + std::to_string(myTable + 1) + ".";
        else if (at != q.end()) text += "Back in the queue \xC2\xB7 " + Ordinal(static_cast<int>(at - q.begin()) + 1) + ".";
        else text += "Back to the room.";
    }
    else if (!v.tableNotice.empty()) {
        lead = "NO MATCH";
        leadCol = kRed;
        text = v.tableNotice;
        prompts = {{"button_a", "OK"}};
    }
    else if (g.onPlayers) {
        const uint32_t who = players.empty() ? 0 : players[g.player].id;
        text = who == v.self ? "That is you." : "A: mute or unmute their chat for this session.";
        prompts = {};
        if (who != v.self) prompts.push_back({"button_a", g.muted.count(who) ? "Unmute" : "Mute"});
        if (who != v.self && v.owner == v.self) prompts.push_back({"button_x", "Remove"});
        prompts.push_back({"button_b", "Tables"});
    }
    else {
        const std::string place = RoomsPlace(v, v.self);
        if (v.members.size() <= 1) {
            lead = "ROOM OPEN";
            text = "Friends join with the code.";
        }
        else if (place != "Idle") {
            lead = Upper(place);
            text = "RB: the players. Y: My character.";
        }
        else {
            text = "Pick a table to sit at or queue for.";
        }
        if (fs.phase == TablePhase::Open && atFocused) prompts.push_back({"button_a", "Stand up"});
        else if (fs.phase == TablePhase::Open && (!fs.seat[0] || !fs.seat[1])) {
            const int seat = !fs.seat[0] && !fs.seat[1] ? g.seatSide : !fs.seat[0] ? 0 : 1;
            prompts.push_back({"button_a", seat ? "Sit as P2" : "Sit as P1"});
        }
        else if (queuedHere) prompts.push_back({"button_a", "Leave the queue"});
        else if (!atFocused) prompts.push_back({"button_a", "Join the queue"});
        prompts.push_back({"button_y", "My character"});
        if (v.owner == v.self) prompts.push_back({"button_start", "Table rules"});
        prompts.push_back({"button_b", "Leave"});
    }
    if (g.cardTable >= 0) Card(d, f, v, g.cardTable);
    else if (handoffTable >= 0) HandoffModal(d, f, v, handoffTable, steps, stepSeconds);
    else if (g.modal == Modal::Leaving) Dialog(d, f, "ROOM " + WeyveService::DisplayCode(v.room), "Leave the room?", LeaveText(v));
    else if (g.modal == Modal::Remove) {
        Dialog(d, f, "OWNER", "Remove " + NameOf(v, g.removing) + "?", "They are out of the room and cannot come back on this connection.", kRed);
    }
    else if (g.modal == Modal::Rules) RulesModal(d, f, v);
    Bar(d, f, lead, leadCol, text, prompts, &clicked);
    if (clicked >= 0) {
        const std::string what = prompts[clicked].second;
        const char* icon = prompts[clicked].first;
        if (g.modal == Modal::Leaving && what == "Leave") in->leave = true, g.modal = Modal::None;
        else if (std::strcmp(icon, "button_b") == 0 && g.modal != Modal::None) g.modal = Modal::None;
        else if (what == "Leave") g.modal = Modal::Leaving;
        else if (what == "Ready") {
            g.cardReady = true;
            if (!v.tableNotice.empty()) in->clearNotice = true;
            Request(in, "ready", {std::to_string(g.cardTable)});
            in->delay = g.cardDelay;
        }
        else if (what == "Leave queue") Request(in, "decline", {std::to_string(g.cardTable)});
        else if (what == "My character") in->myCharacter = true;
        else if (what == "OK") in->clearNotice = true;
        else if (what == "Cancel" && handoffTable >= 0) in->cancelHandoff = true;
    }
    (void)mySeat;
    g_inputOn = false;
    d->PopClipRect();
}

} // namespace sfv::mod
