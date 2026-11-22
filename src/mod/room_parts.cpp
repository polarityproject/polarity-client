// The room screen's parts: a table and its seats, the player list, the chat, a confirm.

#include "mod/room_internal.h"

#include "core/text.h"
#include "mod/ui_sound.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>

namespace sfv::mod {

using namespace roomui;
using namespace ui;
using keeper::TablePhase;

namespace {

std::string Roman(int n) {
    static const char* const kRoman[] = {"I", "II", "III", "IV", "V"};
    return n >= 1 && n <= 5 ? kRoman[n - 1] : std::to_string(n);
}

} // namespace

namespace roomui {

std::string SkillLabel(const std::string& code, bool trigger, int index) {
    if (index <= 0) return "";
    const auto it = g_skillNames.find(code);
    if (it != g_skillNames.end()) {
        const auto& names = trigger ? it->second.second : it->second.first;
        if (index - 1 < static_cast<int>(names.size()) && !names[index - 1].empty()) return Roman(index) + " " + names[index - 1];
    }
    return Roman(index);
}

int SkillCount(const std::string& code, bool trigger) {
    const auto it = g_skillNames.find(code);
    const int n = it == g_skillNames.end() ? 0 : static_cast<int>((trigger ? it->second.second : it->second.first).size());
    return std::max(2, n);
}

} // namespace roomui

namespace {

ImU32 Dim(ImU32 c, bool dim) { return dim ? (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * 0.45f) << IM_COL32_A_SHIFT) : c; }

} // namespace

namespace roomui {

const RoomMember* Member(const RoomsView& v, uint32_t id) {
    for (const RoomMember& m : v.members) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

std::string NameOf(const RoomsView& v, uint32_t id) {
    const RoomMember* m = Member(v, id);
    return m ? m->name : std::string("?");
}

} // namespace roomui

namespace {

void Panel(ImDrawList* d, float x0, float y0, float x1, float y1) { Pane(d, x0, y0, x1, y1); }

} // namespace

namespace roomui {

void Seat(ImDrawList* d, const HubFonts& f, float x, float y, const RoomMember* m, bool ready, bool sitHere, int seat, bool dim) {
    const ImVec2 a = g_v.P(x, y), b = g_v.P(x + 330, y + 120);
    if (sitHere) {
        d->AddRectFilled(a, b, Rgba(233, 196, 106, 36));
        d->AddRect(ImVec2(a.x + g_v.L(1), a.y + g_v.L(1)), ImVec2(b.x - g_v.L(1), b.y - g_v.L(1)), look::kRing, 0, 0, g_v.L(2));
        Text(d, f.bold ? f.bold : f.strong, g_v.L(18), g_v.P(x + 165, y + 48), kGold, TR(seat ? Str::room_table_sit_p2 : Str::room_table_sit_p1), 0.5f);
        return;
    }
    d->AddRectFilled(a, b, look::kControl);
    if (m) {
        const bool p2 = seat == 1;
        if (const TextureView* art = m->chara.empty() ? nullptr : TextureSized(CharaTex(m->chara, "PartCharacter"), g_v.L(250))) {
            static const FadeStop kP1[] = {{0, 1}, {0.72f, 1}, {0.84f, 0.6f}, {0.98f, 0}, {1, 0}};
            static const FadeStop kP2[] = {{0, 0}, {0.02f, 0}, {0.16f, 0.6f}, {0.28f, 1}, {1, 1}};
            const float ax = p2 ? x + 110 : x - 30;
            d->PushClipRect(a, b, true);
            ImageFadeX(d, art, g_v.P(ax, y - 18), g_v.P(ax + 250, y + 232), ImVec2(0, 0), ImVec2(1, 1), p2 ? kP2 : kP1, 5, p2,
                       Dim(Rgba(255, 255, 255), dim));
            d->PopClipRect();
        }
        const float tx = p2 ? x + 16 : x + 314, align = p2 ? 0.0f : 1.0f;
        const auto name = g_names.find(m->chara);
        if (name != g_names.end()) StageText(d, f.body, 16, tx, y + 60, Dim(kCream, dim), name->second, align);
        ImFont* bold = f.bold ? f.bold : f.strong;
        const bool flag = m->country.size() == 2;
        const float flagW = flag ? 28.0f : 0.0f;
        const std::string who = Ellipsize(bold, 21, Upper(m->name), 164 - flagW);
        const float nameW = TextW(bold, 21, who);
        const float left = p2 ? tx : tx - nameW - flagW;
        if (flag) RoomsScreenDrawFlag(d, f.strong, m->country, left, y + 89, 13, dim ? 0.45f : 1.0f);
        StageText(d, bold, 21, left + flagW, y + 82, Dim(kCream, dim), who);
        if (ready) StageText(d, bold, 15, tx, y + 11, Dim(kGreen, dim), TR(Str::common_ready), align);
    }
    d->AddRect(a, b, Dim(look::kEdgeControl, dim), 0, 0, std::max(1.0f, g_v.L(1)));
    if (!m) Text(d, f.body, g_v.L(18), g_v.P(x + 165, y + 49), Dim(kSoft, dim), TR(Str::common_open_seat), 0.5f);
}

void Table(ImDrawList* d, const HubFonts& f, const RoomsView& v, float y, int index, bool on, bool dim) {
    const TableView* tv = index < static_cast<int>(v.tableViews.size()) ? &v.tableViews[index] : nullptr;
    const keeper::TableState s = tv ? tv->s : keeper::TableState{};
    Pane(d, 48, y, 868, y + kTableH);
    const bool lit = on && !dim;
    Strip(d, 50, y + 2, 866, y + 40, lit);
    Text(d, f.display, g_v.L(21), g_v.P(64, y + 9), lit ? look::kInk : Dim(kCream, dim), TRF(Str::room_table_title, index + 1));
    Rules rules;
    const std::string line = tv && ParseRules(tv->rules, &rules) ? RoomsRulesText(rules) : std::string();
    const std::string queue = TRF(Str::room_table_in_queue, tv ? static_cast<int>(tv->queue.size()) : 0);
    ImFont* infoFont = f.bold ? f.bold : f.strong;
    const float queueW = TextW(infoFont, 19, queue);
    const float tableW = TextW(f.display, 21, TRF(Str::room_table_title, index + 1));
    Text(d, infoFont, g_v.L(19), g_v.P(852, y + 9), lit ? look::kInk : Dim(kCream, dim), queue, 1);
    Text(d, infoFont, g_v.L(19), g_v.P(852 - queueW - 18, y + 9), lit ? look::kInkSoft : Dim(kGold, dim),
         Ellipsize(infoFont, 19, line, 852 - queueW - 18 - (64 + tableW + 24)), 1);
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
    const bool live = s.phase == TablePhase::Playing || s.phase == TablePhase::Connecting;
    if (live) StageText(d, f.display, 40, 458, y + 78, Dim(Rgba(255, 255, 255), dim), std::to_string(s.wins[0]) + " - " + std::to_string(s.wins[1]), 0.5f);
    else Text(d, f.display, g_v.L(30), g_v.P(458, y + 88), Dim(Rgba(138, 129, 112), dim), TR(Str::common_vs), 0.5f);
    const char* doing = s.phase == TablePhase::Call         ? TR(mine ? Str::room_table_your_turn : Str::room_table_getting_ready)
                        : s.phase == TablePhase::Connecting ? TR(Str::room_table_connecting)
                        : s.phase == TablePhase::Playing    ? TR(Str::room_table_playing)
                        : s.phase == TablePhase::After      ? TR(Str::room_table_set_over)
                                                            : "";
    if (*doing) Text(d, f.body, g_v.L(16), g_v.P(458, y + 130), Dim(kCream, dim), doing, 0.5f);
    if (lit) Ring(d, 48, y, 868, y + kTableH);
}

std::vector<RoomMember> PlayerOrder(const RoomsView& v) {
    std::vector<RoomMember> list = v.members;
    std::stable_partition(list.begin(), list.end(), [&](const RoomMember& m) { return m.id == v.self; });
    return list;
}

} // namespace roomui

namespace {

struct FacePoint {
    const char* code;
    int8_t x, y;
};

constexpr FacePoint kFaces[] = {
    {"ALX", 75, 68},
    {"BLR", 83, 51},
    {"BRD", 81, 49},
    {"BSN", 86, 62},
    {"CMY", 83, 50},
    {"CNL", 98, 64},
    {"DSM", 75, 52},
    {"ELV", 90, 74},
    {"FAN", 84, 52},
    {"GUL", 94, 52},
    {"IBK", 98, 42},
    {"JRI", 110, 65},
    {"KEN", 80, 48},
    {"KRN", 69, 53},
    {"LAR", 85, 51},
    {"NCL", 101, 53},
    {"NSH", 73, 58},
    {"RMK", 102, 53},
    {"RSD", 78, 53},
    {"RYU", 87, 54},
    {"URN", 79, 44},
    {"VEG", 91, 44},
    {"Z20", 88, 49},
    {"Z21", 102, 64},
    {"Z22", 85, 44},
    {"Z23", 89, 46},
    {"Z24", 107, 46},
    {"Z25", 76, 66},
    {"Z26", 83, 54},
    {"Z27", 110, 56},
    {"Z28", 95, 52},
    {"Z29", 87, 50},
    {"Z30", 84, 52},
    {"Z31", 86, 50},
    {"Z32", 108, 50},
    {"Z33", 90, 52},
    {"Z34", 85, 53},
    {"Z35", 82, 58},
    {"Z36", 104, 46},
    {"Z37", 116, 53},
    {"Z38", 88, 53},
    {"Z39", 100, 53},
    {"Z40", 75, 54},
    {"Z41", 85, 49},
    {"Z42", 99, 54},
    {"ZGF", 126, 64},
};

constexpr int kRowFaceX = 80, kRowFaceY = 52;

ImVec2 RowArtShift(const std::string& code) {
    for (const FacePoint& p : kFaces) {
        if (code == p.code) return ImVec2(static_cast<float>(kRowFaceX - p.x), static_cast<float>(kRowFaceY - p.y));
    }
    return ImVec2(0, 0);
}

constexpr float kRowsTop = 112, kRowH = 50, kPlayersBottom = 526, kChatTop = 538;
constexpr float kPlaceX = 1215;

} // namespace

namespace roomui {

void Players(ImDrawList* d, const HubFonts& f, const RoomsView& v) {
    Panel(d, 892, 80, 1552, kPlayersBottom);
    Strip(d, 894, 82, 1550, 110, false);
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(910, 86), kGold, TRF(Str::room_players_title, static_cast<int>(v.members.size()), keeper::kRoomCap));
    const std::vector<RoomMember> list = PlayerOrder(v);
    const int count = static_cast<int>(list.size());
    if (g.modal == Modal::None && g.cardTable < 0)
        WheelList(g_v.P(894, kRowsTop), g_v.P(1550, kRowsTop + kPlayersInView * kRowH), count, kPlayersInView, &g.playersFirst, &g.player);
    if (g.player < g.playersFirst) g.playersFirst = static_cast<float>(g.player);
    if (g.player > g.playersFirst + kPlayersInView - 1) g.playersFirst = static_cast<float>(g.player - kPlayersInView + 1);
    g.playersFirst = std::clamp(g.playersFirst, 0.0f, static_cast<float>(std::max(0, count - kPlayersInView)));
    const int first = static_cast<int>(g.playersFirst);
    for (int i = first; i < static_cast<int>(list.size()) && i < first + kPlayersInView; i++) {
        const RoomMember& m = list[i];
        const float y = kRowsTop + (i - first) * kRowH, ty = y + 13;
        if (m.id == v.self) d->AddRectFilled(g_v.P(894, y), g_v.P(1550, y + kRowH - 1), look::kSelfRow);
        if (const TextureView* art = m.chara.empty() ? nullptr : TextureSized(CharaTex(m.chara, "PartCharacter"), g_v.L(200))) {
            static const FadeStop kRow[] = {{0, 0}, {0.06f, 0}, {0.22f, 1}, {0.38f, 1}, {0.60f, 0.5f}, {0.86f, 0}, {1, 0}};
            const ImVec2 shift = RowArtShift(m.chara);
            d->PushClipRect(g_v.P(893, y), g_v.P(1534, y + kRowH - 1), true);
            ImageFadeX(d, art, g_v.P(872 + shift.x, y - 28 + shift.y), g_v.P(1072 + shift.x, y + 172 + shift.y), ImVec2(0, 0), ImVec2(1, 1),
                       kRow, 7, false, IM_COL32_WHITE, g_v.P(892, 0).x, g_v.P(1082, 0).x);
            d->PopClipRect();
        }
        d->AddLine(g_v.P(894, y + kRowH - 1), g_v.P(1550, y + kRowH - 1), look::kHairline, g_v.L(1));
        if (g.onPlayers && i == g.player) Ring(d, 896, y + 1, 1536, y + kRowH - 2);
        if (g.modal == Modal::None && !g.typing && g.cardTable < 0 && Clicked() && MouseIn(g_v.P(894, y), g_v.P(1550, y + kRowH))) {
            g.onPlayers = true;
            g.player = i;
        }
        float x = 994;
        if (m.owner) {
            RoomsScreenDrawCrown(d, x, ty + 17, 0.85f, kGold);
            x += 20;
        }
        if (const float fw = RoomsScreenDrawFlag(d, f.strong, m.country, x, ty + 4, 15)) x += fw + 8;
        ImFont* bold = f.bold ? f.bold : f.strong;
        std::string who = Upper(m.name);
        const bool muted = g.muted.count(m.id) > 0;
        const float after = (m.id == v.self ? TextW(f.body, 18, TR(Str::room_players_you)) + 6 : 0) + (muted ? TextW(f.body, 16, TR(Str::room_players_muted)) + 6 : 0);
        const float room = kPlaceX - 10 - x - after;
        who = Ellipsize(bold, 19, who, room);
        StageText(d, bold, 19, x, ty, kCream, who);
        x += TextW(bold, 19, who) + 6;
        if (m.id == v.self) {
            Text(d, f.body, g_v.L(18), g_v.P(x, ty + 1), kSoft, TR(Str::room_players_you), 0, true);
            x += TextW(f.body, 18, TR(Str::room_players_you)) + 6;
        }
        if (muted) Text(d, f.body, g_v.L(16), g_v.P(x, ty + 2), kSoft, TR(Str::room_players_muted), 0, true);
        const RoomPlace place = RoomsPlaceOf(v, m.id);
        Text(d, f.body, g_v.L(18), g_v.P(kPlaceX, ty + 1), place.kind == RoomPlace::Kind::Idle ? kSoft : kGreen, RoomsPlaceText(place, true));
        if (m.pingMs >= 0) {
            const ImU32 col = PingColor(static_cast<unsigned>(m.pingMs));
            const std::string ms = TRF(Str::common_ms, m.pingMs);
            Text(d, bold, g_v.L(18), g_v.P(1428, ty + 1), col, ms, 1);
            if (m.relayed) IconCloud(d, g_v.P(1428 - TextW(bold, 18, ms) - 16, ty + 12), g_v.L(1.7f), col);
        }
        if (m.link) {
            const char* what = TR(m.link == 1 ? Str::room_players_wired : Str::room_players_wifi);
            Text(d, f.body, g_v.L(17), g_v.P(1520, ty + 2), kSoft, what, 1);
            IconLinkType(d, g_v.P(1520 - TextW(f.body, 17, what) - 14, ty + 12), g_v.L(1.5f), m.link, kSoft);
        }
    }
    ScrollBar(d, 1541, kRowsTop + 4, kRowsTop + kPlayersInView * kRowH - 4, static_cast<float>(first), kPlayersInView,
              static_cast<float>(list.size()), false);
}

void Chat(ImDrawList* d, const HubFonts& f, const RoomsView& v, bool* clickedBox, bool* clickedSend) {
    Panel(d, 892, kChatTop, 1552, 832);
    Strip(d, 894, kChatTop + 2, 1550, kChatTop + 30, false);
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(910, kChatTop + 6), kGold, TR(Str::room_chat_title));
    const float wrap = 610, size = 19, top = kChatTop + 36, viewBottom = 773;
    const ImVec2 ca = g_v.P(894, top), cb = g_v.P(1550, viewBottom);
    if (const int w = WheelSteps(ca, cb)) g.chatScroll += w * 40.0f;
    if (g.typing) {
        if (Pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp})) g.chatScroll += 40;
        if (Pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown})) g.chatScroll -= 40;
        if (Pressed({ImGuiKey_PageUp})) g.chatScroll += 160;
        if (Pressed({ImGuiKey_PageDown})) g.chatScroll -= 160;
    }
    if (!v.chat.empty()) {
        const ChatLine& last = v.chat.back();
        if (g.chatHeard && (last.from != g.chatHeardFrom || last.text != g.chatHeardText)) {
            if (last.from && last.from != v.self && !g.muted.count(last.from)) UiSoundPlay(UiSound::ChatPost);
            else if (last.kind == ChatLine::Kind::Joined) UiSoundPlay(UiSound::Joined);
            else if (last.kind == ChatLine::Kind::Left) UiSoundPlay(UiSound::Left);
        }
        g.chatHeard = true;
        g.chatHeardFrom = last.from;
        g.chatHeardText = last.text;
    }
    int unseen = 0;
    if (!v.chat.empty()) {
        const ChatLine& last = v.chat.back();
        if (g.chatScroll <= 0) {
            g.chatSeenFrom = last.from;
            g.chatSeenText = last.text;
        } else {
            for (int i = static_cast<int>(v.chat.size()) - 1; i >= 0; i--) {
                if (v.chat[i].from == g.chatSeenFrom && v.chat[i].text == g.chatSeenText) break;
                unseen++;
            }
        }
    }
    float bottom = viewBottom + std::max(0.0f, g.chatScroll), height = 0;
    d->PushClipRect(ca, cb, true);
    for (int i = static_cast<int>(v.chat.size()) - 1; i >= 0; i--) {
        const ChatLine& line = v.chat[i];
        if (line.from && g.muted.count(line.from)) continue;
        const std::string& name = v.wordFilter ? line.name : line.realName;
        const std::string& text = v.wordFilter ? line.censored : line.text;
        std::string all = text;
        if (line.from) {
            const float nameW = TextW(f.bold ? f.bold : f.strong, size, Upper(name)) + 8;
            std::string pad;
            while (TextW(f.body, size, pad) < nameW) pad += ' ';
            all = pad + text;
        }
        const float h = f.body->CalcTextSizeA(TextPx(size), FLT_MAX, g_v.L(wrap), all.c_str()).y / g_v.s;
        const float y = bottom - h;
        height += h + 4;
        if (y < viewBottom && y + h > top) {
            if (line.from) {
                d->AddText(f.body, TextPx(size), g_v.P(910, y), kCream, all.c_str(), nullptr, g_v.L(wrap));
                d->AddText(f.bold ? f.bold : f.strong, TextPx(size), g_v.P(910, y), kGold, Upper(name).c_str());
            } else {
                d->AddText(f.body, TextPx(size), g_v.P(910, y), kSoft, all.c_str(), nullptr, g_v.L(wrap));
            }
        }
        bottom = y - 4;
    }
    d->PopClipRect();
    const float view = viewBottom - top;
    g.chatScroll = std::clamp(g.chatScroll, 0.0f, std::max(0.0f, height - view));
    if (height > view) ScrollBar(d, 1536, top, viewBottom, height - view - g.chatScroll, view, height, false);
    if (g.chatScroll > 0 && unseen > 0) {
        const std::string label = TRF(unseen == 1 ? Str::room_chat_new_message : Str::room_chat_new_messages, unseen);
        ImFont* bold = f.bold ? f.bold : f.strong;
        const float w = TextW(bold, 16, label) + 34, x1 = 1530, x0 = x1 - w, y0 = 744, y1 = 772;
        DropShadow(d, x0, y0, x1, y1, 4, 10, 0.35f, false);
        d->AddRectFilled(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(10, 12, 16, 214));
        d->AddRect(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(236, 228, 206, 115), 0, 0, std::max(1.0f, g_v.L(1)));
        Text(d, bold, g_v.L(16), g_v.P(x0 + 10, y0 + 5), kCream, label);
        d->AddTriangleFilled(g_v.P(x1 - 18, y0 + 11), g_v.P(x1 - 8, y0 + 11), g_v.P(x1 - 13, y0 + 18), kCream);
        if (Clicked() && MouseIn(g_v.P(x0, y0), g_v.P(x1, y1))) g.chatScroll = 0;
    }
    const ImVec2 a = g_v.P(910, 781), b = g_v.P(1460, 819);
    d->AddRectFilled(a, b, Rgba(0, 0, 0, 115));
    d->AddRect(a, b, g.typing ? look::kRing : look::kEdgeControl, 0, 0, g_v.L(g.typing ? 2.0f : 1.0f));
    if (g.typing) {
        const std::string shown = g.text + ((static_cast<int>(ImGui::GetTime() * 2) & 1) ? "|" : " ");
        std::string tail = shown;
        while (tail.size() > 1 && TextW(f.body, 19, tail) > 520) tail.erase(0, 1);
        Text(d, f.body, g_v.L(19), g_v.P(924, 788), kCream, tail);
    } else {
        Text(d, f.body, g_v.L(19), g_v.P(924, 788), kSoft,
             ui::MappedPadKeys(0x0D /* Enter */) ? TR(Str::room_chat_say_back) : TR(Str::room_chat_say_enter));
    }
    d->AddRectFilled(g_v.P(1468, 781), g_v.P(1534, 819), kGold);
    Text(d, f.bold ? f.bold : f.strong, g_v.L(18), g_v.P(1501, 788), look::kInk, TR(Str::room_chat_send), 0.5f);
    *clickedBox = Clicked() && MouseIn(a, b);
    *clickedSend = Clicked() && MouseIn(g_v.P(1468, 781), g_v.P(1534, 819));
}

void Dialog(ImDrawList* d, const HubFonts& f, const std::string& kicker, const std::string& title, const std::string& text, bool danger, const char* yes, const char* no, const PromptList& prompts, int* clicked, int* pressed) {
    ImFont* bold = f.bold ? f.bold : f.strong;
    const float wrap = 544;
    const float textH = f.body->CalcTextSizeA(TextPx(19), FLT_MAX, g_v.L(wrap), text.c_str()).y / g_v.s;
    ModalSpec m;
    m.w = 600;
    m.h = 44 + 22 + 34 + 12 + textH + 24 + 48 + 26 + 48;
    m.x0 = 500;
    m.y0 = std::max(60.0f, (844 - m.h) / 2);
    m.kicker = kicker;
    m.danger = danger;
    m.prompts = prompts;
    float y = ui::Modal(d, bold, f.body, m, clicked) + 22;
    Text(d, f.display, g_v.L(28), g_v.P(528, y), kCream, title);
    y += 46;
    d->AddText(f.body, TextPx(19), g_v.P(528, y), kCream, text.c_str(), nullptr, g_v.L(wrap));
    y += textH + 24;
    if (Button(d, bold, 528, y, 806, y + 48, yes, g.choice == 0, danger)) *pressed = 0;
    if (Button(d, bold, 822, y, 1072, y + 48, no, g.choice == 1)) *pressed = 1;
}

} // namespace roomui

} // namespace sfv::mod
