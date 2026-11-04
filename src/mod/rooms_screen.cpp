#include "mod/rooms_screen.h"

#include "core/rules.h"
#include "mod/log.h"
#include "mod/ui_kit.h"
#include "mod/weyve_service.h"

#include <imgui.h>
#include <imgui_internal.h> // IM_PI

#include <algorithm>
#include <atomic>
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
const ImU32 kRed = IM_COL32(255, 107, 94, 255);
const ImU32 kLine = IM_COL32(201, 168, 92, 190);

constexpr const char kCodeChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // a room code's alphabet
constexpr int kKeyCols = 9;
constexpr int kKeys = 32;
constexpr float kRowPitch = 92, kRowH = 84, kListTop = 274;
constexpr int kRowsInView = 6;

enum Focus { kCreate = -2, kCode = -1 };

struct State {
    int focus = kCreate;    // kCreate, kCode, or a public room's index
    bool inPanel = false;   // working in the right panel (the form's rows, the keypad)
    int formRow = 0;        // 0 name, 1 visibility, 2 tables
    keeper::RoomSettings settings;
    std::string code;
    int key = 0;            // the keypad's cursor
    float scroll = 0;       // the list's first row shown (rows)
    std::string lastCode;   // the code a failed join was for
} g;

std::atomic<bool> g_open{false};

ImU32 Alpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT); }

std::string OpenSeatsText(const keeper::ListedRoom& r, ImU32* col)
{
    if (r.members >= keeper::kRoomCap) {
        *col = kRed;
        return "Full";
    }
    if (r.openSeats > 0) {
        *col = kGreen;
        return std::to_string(r.openSeats) + (r.openSeats == 1 ? " open seat" : " open seats");
    }
    *col = kGold;
    return "Queues only";
}

std::string FirstTo(const std::string& rulesText)
{
    Rules rules;
    if (!ParseRules(rulesText, &rules)) return "";
    return "first to " + std::to_string(rules.firstTo);
}

std::string Plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

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

void ActionRow(ImDrawList* d, const HubFonts& f, float y, const char* glyph, const char* title, const char* text, bool on)
{
    const ImVec2 a = g_v.P(48, y), b = g_v.P(868, y + 72);
    if (on) {
        d->AddRectFilledMultiColor(a, g_v.P(48 + 820 * 0.45f, y + 72), Rgba(240, 200, 90, 82), Rgba(20, 24, 30, 235), Rgba(20, 24, 30, 235),
                                   Rgba(240, 200, 90, 82));
        d->AddRectFilledMultiColor(g_v.P(48 + 820 * 0.45f, y), b, Rgba(20, 24, 30, 235), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240),
                                   Rgba(20, 24, 30, 235));
    }
    else {
        d->AddRectFilledMultiColor(a, b, Rgba(20, 24, 30, 235), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240), Rgba(20, 24, 30, 235));
    }
    d->AddLine(a, g_v.P(868, y), kLine, g_v.L(1));
    if (on) d->AddRect(ImVec2(a.x + g_v.L(1.5f), a.y + g_v.L(1.5f)), ImVec2(b.x - g_v.L(1.5f), b.y - g_v.L(1.5f)), kRing, 0, 0, g_v.L(3));
    d->AddCircle(g_v.P(88, y + 36), g_v.L(20), kGold, 32, g_v.L(2));
    Text(d, f.strong, g_v.L(26), g_v.P(88, y + 20), kGold, glyph, 0.5f);
    Text(d, f.display, g_v.L(21), g_v.P(130, y + 15), kCream, title);
    Text(d, f.body, g_v.L(15), g_v.P(130, y + 42), kMuted, text);
}

void RoomRow(ImDrawList* d, const HubFonts& f, float y, const keeper::ListedRoom& r, bool on)
{
    const bool full = r.members >= keeper::kRoomCap;
    const ImVec2 a = g_v.P(48, y), b = g_v.P(868, y + kRowH);
    d->AddRectFilledMultiColor(a, b, Rgba(20, 24, 30, 235), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240), Rgba(20, 24, 30, 235));
    d->AddLine(a, g_v.P(868, y), Rgba(201, 168, 92, 128), g_v.L(1));
    const int fade = full ? 158 : 255;
    Text(d, f.display, g_v.L(21), g_v.P(68, y + 17), Alpha(kCream, fade), keeper::PresetName(r.preset));
    DrawCrown(d, 68, y + 52, 1, Alpha(kGold, fade));
    Text(d, f.strong, g_v.L(15), g_v.P(90, y + 47), Alpha(kSoft, fade), Upper(r.owner));
    const float ox = 90 + TextW(f.strong, 15, Upper(r.owner));
    std::string detail = " \xC2\xB7 " + Plural(r.tables, "table", "tables");
    const std::string ft = FirstTo(r.rules);
    if (!ft.empty()) detail += " \xC2\xB7 " + ft;
    Text(d, f.body, g_v.L(15), g_v.P(ox, y + 47), Alpha(kMuted, fade), detail);
    float fx = 484;
    for (size_t i = 0; i < r.faces.size() && i < 6; i++) {
        DrawFace(d, r.faces[i], fx, y + 22, 30, 40);
        fx += 32;
    }
    const std::string count = std::to_string(r.members) + " / " + std::to_string(keeper::kRoomCap);
    const float cw = TextW(f.strong, 20, count);
    Text(d, f.strong, g_v.L(20), g_v.P(848, y + 18), Alpha(kCream, fade), count, 1);
    DrawPerson(d, 848 - cw - 24, y + 30, 16, Alpha(kSoft, fade));
    ImU32 col = kGreen;
    const std::string seats = OpenSeatsText(r, &col);
    Text(d, f.strong, g_v.L(15), g_v.P(848, y + 50), col, seats, 1);
    if (on) d->AddRect(ImVec2(a.x + g_v.L(1.5f), a.y + g_v.L(1.5f)), ImVec2(b.x - g_v.L(1.5f), b.y - g_v.L(1.5f)), kRing, 0, 0, g_v.L(3));
}

void PanelFrame(ImDrawList* d)
{
    const ImVec2 a = g_v.P(892, 80), b = g_v.P(1552, 832);
    d->AddRectFilledMultiColor(a, b, Rgba(8, 9, 11, 214), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 240), Rgba(8, 9, 11, 214));
    d->AddLine(a, g_v.P(1552, 80), kLine, g_v.L(1));
    d->AddLine(g_v.P(892, 832), b, kLine, g_v.L(1));
}

void RoomPanel(ImDrawList* d, const HubFonts& f, const keeper::ListedRoom& r, const keeper::RoomDetail* detail)
{
    const bool full = r.members >= keeper::kRoomCap;
    Text(d, f.strong, g_v.L(16), g_v.P(918, 104), full ? kRed : kGold, full ? "FULL \xC2\xB7 16 OF 16" : "PUBLIC ROOM");
    Text(d, f.display, g_v.L(30), g_v.P(918, 132), kCream, keeper::PresetName(r.preset));
    DrawCrown(d, 918, 196, 1, kGold);
    Text(d, f.strong, g_v.L(16), g_v.P(940, 186), kSoft, Upper(r.owner));
    Text(d, f.body, g_v.L(16), g_v.P(940 + TextW(f.strong, 16, Upper(r.owner)), 186), kMuted,
         " \xC2\xB7 " + std::to_string(r.members) + " / 16 players \xC2\xB7 " + Plural(r.tables, "table", "tables"));
    Text(d, f.body, g_v.L(13), g_v.P(918, 226), kMuted, "TABLES");
    // Each table's rules from the keeper's detail; before it comes, the
    // first table's from the list.
    const int shown = std::min(r.tables, 5);
    float y = 248;
    for (int t = 0; t < shown; t++) {
        d->AddRectFilled(g_v.P(918, y), g_v.P(1526, y + 52), Rgba(20, 24, 30, 217));
        d->AddRectFilled(g_v.P(918, y), g_v.P(921, y + 52), kGreen);
        Text(d, f.display, g_v.L(15), g_v.P(935, y + 16), kCream, "TABLE " + std::to_string(t + 1));
        const std::string text = detail && t < static_cast<int>(detail->tables.size()) ? detail->tables[t] : t == 0 ? r.rules : "";
        Rules rules;
        if (ParseRules(text, &rules)) {
            std::string line = DescribeRules(rules);
            while (line.size() > 4 && TextW(f.body, 14, line) > 330) line = line.substr(0, line.size() - 4) + "...";
            Text(d, f.body, g_v.L(14), g_v.P(1035, y + 17), kGold, line);
        }
        Text(d, f.strong, g_v.L(15), g_v.P(1512, y + 16), kGreen, "Open seat", 1);
        y += 58;
    }
    if (r.tables > shown) {
        Text(d, f.body, g_v.L(14), g_v.P(918, y), kMuted, "+" + Plural(r.tables - shown, "more table", "more tables"));
        y += 26;
    }
    Text(d, f.body, g_v.L(13), g_v.P(918, y + 10), kMuted, "PLAYERS \xC2\xB7 " + std::to_string(r.members) + " / 16");
    if (detail) {
        // Four to a row, up to twelve: face and name.
        const int count = static_cast<int>(detail->people.size());
        for (int i = 0; i < count && i < 12; i++) {
            const float px = 918 + (i % 4) * 152.0f, py = y + 34 + (i / 4) * 36.0f;
            DrawFace(d, detail->people[i].chara, px, py, 20, 28);
            std::string name = Upper(detail->people[i].name);
            while (name.size() > 1 && TextW(f.strong, 15, name) > 112) name.pop_back();
            Text(d, f.strong, g_v.L(15), g_v.P(px + 28, py + 5), kCream, name);
        }
        if (count > 12) Text(d, f.body, g_v.L(14), g_v.P(918, y + 34 + 3 * 36.0f), kMuted, "+" + std::to_string(count - 12) + " more");
        return;
    }
    float fx = 918;
    for (const std::string& face : r.faces) {
        DrawFace(d, face, fx, y + 36, 22, 30);
        fx += 28;
    }
}

const char* const kFormLabels[] = {"Name", "Visibility", "Tables"};

void CreatePanel(ImDrawList* d, const HubFonts& f, bool active, int* clickedRow, int* clickedStep)
{
    Text(d, f.strong, g_v.L(16), g_v.P(918, 104), kGold, "NEW ROOM");
    Text(d, f.display, g_v.L(30), g_v.P(918, 132), kCream, "Create a room");
    const std::string values[] = {keeper::PresetName(g.settings.preset), g.settings.isPublic ? "Public" : "Private",
                                  Plural(g.settings.tables, "table", "tables")};
    *clickedRow = *clickedStep = -1;
    for (int i = 0; i < 3; i++) {
        const float y = 196 + i * 60.0f;
        const bool on = g.formRow == i;
        const ImVec2 a = g_v.P(918, y), b = g_v.P(1526, y + 54);
        if (on) {
            d->AddRectFilledMultiColor(a, g_v.P(918 + 608 * 0.3f, y + 54), Rgba(240, 200, 90, active ? 128 : 70), Rgba(240, 200, 90, 26),
                                       Rgba(240, 200, 90, 26), Rgba(240, 200, 90, active ? 128 : 70));
            d->AddRectFilledMultiColor(g_v.P(918 + 608 * 0.3f, y), b, Rgba(240, 200, 90, 26), Rgba(8, 9, 11, 153), Rgba(8, 9, 11, 153),
                                       Rgba(240, 200, 90, 26));
            d->AddLine(a, g_v.P(1526, y), kLine, g_v.L(1));
            d->AddLine(g_v.P(918, y + 54), b, kLine, g_v.L(1));
        }
        else {
            d->AddRectFilled(a, b, Rgba(20, 24, 30, 179));
        }
        Text(d, f.strong, g_v.L(19), g_v.P(940, y + 15), kCream, kFormLabels[i]);
        Text(d, f.body, g_v.L(19), g_v.P(1323, y + 15), kCream, values[i], 0.5f);
        if (on) {
            // ◀ and ▶ around the value.
            d->AddTriangleFilled(g_v.P(1146, y + 21), g_v.P(1146, y + 33), g_v.P(1136, y + 27), kGold);
            d->AddTriangleFilled(g_v.P(1500, y + 21), g_v.P(1510, y + 27), g_v.P(1500, y + 33), kGold);
        }
        if (Clicked() && MouseIn(a, b)) {
            *clickedRow = i;
            if (MouseIn(g_v.P(1120, y), g_v.P(1170, y + 54))) *clickedStep = 0;
            else if (MouseIn(g_v.P(1480, y), g_v.P(1526, y + 54))) *clickedStep = 1;
        }
    }
    const char* visibility = g.settings.isPublic
                                 ? "Public: listed here for everyone, and anyone can join. Friends can still use the code."
                                 : "Private: not listed. Only people you send the code to can join.";
    Text(d, f.body, g_v.L(17), g_v.P(918, 390), kSoft, visibility);
    Text(d, f.body, g_v.L(17), g_v.P(918, 425), kMuted, "Up to 16 players. Each table has its own queue and rules; the owner");
    Text(d, f.body, g_v.L(17), g_v.P(918, 450), kMuted, "sets them in the room with Start.");
}

void CodePanel(ImDrawList* d, const HubFonts& f, bool active, const std::string& notice, int* clickedKey)
{
    Text(d, f.strong, g_v.L(16), g_v.P(918, 104), kGold, "ROOM CODE");
    Text(d, f.display, g_v.L(30), g_v.P(918, 132), kCream, "Join with a code");
    const float slotW = (608 - 50) / 6.0f;
    for (int i = 0; i < 6; i++) {
        const float x = 918 + i * (slotW + 10);
        d->AddRectFilledMultiColor(g_v.P(x, 200), g_v.P(x + slotW, 292), Rgba(20, 24, 30, 242), Rgba(20, 24, 30, 242), Rgba(8, 9, 11, 242),
                                   Rgba(8, 9, 11, 242));
        const bool next = active && static_cast<int>(g.code.size()) == i;
        d->AddRectFilled(g_v.P(x, 292), g_v.P(x + slotW, 296), next ? kGold : Rgba(74, 68, 54));
        if (i < static_cast<int>(g.code.size())) Text(d, f.display, g_v.L(52), g_v.P(x + slotW / 2, 214), Rgba(255, 255, 255), std::string(1, g.code[i]), 0.5f);
    }
    *clickedKey = -1;
    if (!notice.empty()) {
        const bool full = notice == "Room full";
        const ImU32 line = full ? kGold : kRed;
        const ImVec2 a = g_v.P(918, 324), b = g_v.P(1526, 470);
        d->AddRectFilledMultiColor(a, b, Rgba(20, 24, 30), Rgba(20, 24, 30), Rgba(8, 9, 11), Rgba(8, 9, 11));
        d->AddRectFilled(a, g_v.P(1526, 326), line);
        d->AddRectFilled(g_v.P(918, 468), b, line);
        Text(d, f.strong, g_v.L(16), g_v.P(944, 346), line, full ? "ROOM FULL" : "NO SUCH ROOM");
        const std::string code = g.lastCode.empty() ? g.code : g.lastCode;
        Text(d, f.display, g_v.L(26), g_v.P(944, 374), kCream, full ? code + " has 16 players" : "Nothing open on " + code);
        Text(d, f.body, g_v.L(17), g_v.P(944, 418), kSoft,
             full ? "That is the most a room holds. Try again once someone leaves."
                  : "Check the code with whoever sent it. A room closes once everyone has left it.");
        return;
    }
    const float keyW = (608 - 8 * (kKeyCols - 1)) / float(kKeyCols);
    for (int k = 0; k < kKeys; k++) {
        const float x = 918 + (k % kKeyCols) * (keyW + 8), y = 324 + (k / kKeyCols) * 62.0f;
        const ImVec2 a = g_v.P(x, y), b = g_v.P(x + keyW, y + 54);
        d->AddRectFilled(a, b, Rgba(20, 24, 30, 235));
        const bool on = active && g.key == k;
        d->AddRect(a, b, on ? kRing : Rgba(20, 24, 30), 0, 0, g_v.L(2));
        Text(d, f.strong, g_v.L(22), g_v.P(x + keyW / 2, y + 13), kCream, std::string(1, kCodeChars[k]), 0.5f);
        if (Clicked() && MouseIn(a, b)) *clickedKey = k;
    }
}

void WaitPanel(ImDrawList* d, const HubFonts& f, const char* kicker, const std::string& title, const std::string& text)
{
    Text(d, f.strong, g_v.L(16), g_v.P(918, 104), kGold, kicker);
    Text(d, f.display, g_v.L(30), g_v.P(918, 132), kCream, title);
    Text(d, f.body, g_v.L(17), g_v.P(918, 190), kSoft, text);
    // A turning arc.
    const float t = static_cast<float>(ImGui::GetTime()) * 5.0f;
    d->PathArcTo(g_v.P(1222, 330), g_v.L(26), t, t + 4.2f, 32);
    d->PathStroke(kGold, 0, g_v.L(4));
}

} // namespace

void DrawFace(ImDrawList* d, const std::string& chara, float x, float y, float w, float h)
{
    // The character's grid icon (CharaSelectIconLeft: 116 x 144, its art a
    // strip 62 wide that leans 27.1 px over its 140, the select screen's
    // kArtLeft) in a frame leaning at the same angle, so the picture is
    // neither sheared nor stretched: a part of the strip as wide as fits the
    // frame's proportions, from near its top (the head and shoulders).
    constexpr float kLean = 27.1f / 140.0f, kStripW = 62, kStripX = 6.45f, kStripY = 2, kTexW = 116, kTexH = 144;
    constexpr float kCropW = 46, kCropY = 4; // the part shown: 46 of the strip's 62, from 4 px down
    const float s = h * kLean, top = w - s;
    const ImVec2 q[4] = {g_v.P(x, y), g_v.P(x + top, y), g_v.P(x + w, y + h), g_v.P(x + s, y + h)};
    d->AddQuadFilled(q[0], q[1], q[2], q[3], Rgba(111, 108, 100));
    const float in = 1, inLean = in * kLean;
    const ImVec2 r[4] = {g_v.P(x + in + inLean, y + in), g_v.P(x + top - in + inLean, y + in), g_v.P(x + w - in - inLean, y + h - in),
                         g_v.P(x + s + in - inLean, y + h - in)};
    d->AddQuadFilled(r[0], r[1], r[2], r[3], Rgba(20, 22, 26));
    if (const TextureView* t = chara.empty() ? nullptr : Texture(CharaTex(chara, "CharaSelectIconLeft"))) {
        const float cropH = std::min(140.0f - kCropY, kCropW * (h - 2 * in) / (top - 2 * in)); // the frame's proportions
        const float x0 = kStripX + (kStripW - kCropW) / 2 + kLean * (kCropY - kStripY), y0 = kCropY;
        auto uv = [&](float tx, float ty) { return ImVec2(tx / kTexW, ty / kTexH); };
        d->AddImageQuad(Id(t), r[0], r[1], r[2], r[3], uv(x0, y0), uv(x0 + kCropW, y0), uv(x0 + kCropW + kLean * cropH, y0 + cropH),
                        uv(x0 + kLean * cropH, y0 + cropH), FadeIn(t));
    }
}

void DrawCrown(ImDrawList* d, float x, float y, float scale, ImU32 col)
{
    // The mock-up's crown, 16 x 12, its baseline at `y`.
    static const ImVec2 kPts[] = {{1, 11}, {2, 3}, {6, 7}, {8, 1}, {10, 7}, {14, 3}, {15, 11}};
    ImVec2 pts[7];
    for (int i = 0; i < 7; i++) pts[i] = g_v.P(x + kPts[i].x * scale, y - 12 * scale + kPts[i].y * scale);
    d->AddConcavePolyFilled(pts, 7, col);
}

void DrawPerson(ImDrawList* d, float x, float y, float size, ImU32 col)
{
    const float k = size / 16.0f;
    d->AddCircleFilled(g_v.P(x + 8 * k, y - 8 + 4.5f * k), g_v.L(3.5f * k), col);
    d->PathArcTo(g_v.P(x + 8 * k, y - 8 + 16 * k), g_v.L(7 * k), IM_PI, IM_PI * 2, 16);
    d->PathFillConvex(col);
}

void OpenRoomsScreen()
{
    if (!g_open.exchange(true)) {
        g.inPanel = false;
        Log("rooms screen: open");
    }
}

void CloseRoomsScreen()
{
    if (g_open.exchange(false)) Log("rooms screen: closed");
}

bool RoomsScreenOpen() { return g_open; }

void DrawRoomsScreen(const HubFonts& f, const RoomsView& v, bool takeInput, RoomsScreenInput* in)
{
    *in = RoomsScreenInput{};
    if (!g_open || !f.display || !f.strong || !f.body) return;
    BeginScreen();
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(6, 8, 11));
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    using S = RoomsView::State;
    const std::vector<keeper::ListedRoom>& rooms = v.publicRooms;
    const int count = static_cast<int>(rooms.size());
    if (g.focus >= count) g.focus = count ? count - 1 : kCode;
    const bool busy = v.state == S::Creating || v.state == S::Joining;
    const bool failed = !v.notice.empty() && (v.notice == "No room on that code" || v.notice == "Room full") && g.focus == kCode;

    // Background: a dark stage across the top.
    if (const TextureView* bg = Texture(StageTex("CHN", "StageSelectBG"))) {
        Image(d, bg, g_v.P(-500, -40), g_v.P(2100, 720), ImVec2(0, 0), ImVec2(1, 1), Rgba(97, 97, 97));
    }
    d->AddRectFilledMultiColor(g_v.P(0, 0), g_v.P(1600, 315), Rgba(6, 8, 11, 140), Rgba(6, 8, 11, 140), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 64));
    d->AddRectFilledMultiColor(g_v.P(0, 315), g_v.P(1600, 630), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 64), Rgba(6, 8, 11, 204), Rgba(6, 8, 11, 204));
    d->AddRectFilledMultiColor(g_v.P(0, 630), g_v.P(1600, 900), Rgba(6, 8, 11, 204), Rgba(6, 8, 11, 204), Rgba(6, 8, 11), Rgba(6, 8, 11));

    // ---- input ----
    int barClick = -1;
    if (!busy) {
        if (!g.inPanel) {
            if (Up()) g.focus = g.focus == kCode ? kCreate : g.focus == 0 ? kCode : std::max<int>(kCreate, g.focus - 1);
            if (Down()) g.focus = g.focus == kCreate ? kCode : g.focus == kCode ? (count ? 0 : kCode) : std::min(count - 1, g.focus + 1);
            if (g.focus == kCreate && (Left() || Right())) {
                // The focused form row changes from the list too (the name first).
                const int dir = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)
                                    ? -1
                                    : 1;
                if (g.formRow == 0) g.settings.preset = (g.settings.preset + dir + keeper::kPresetCount) % keeper::kPresetCount;
                else if (g.formRow == 1) g.settings.isPublic = !g.settings.isPublic;
                else g.settings.tables = std::clamp(g.settings.tables + dir, 1, keeper::kMaxTables);
            }
            if (g.focus == kCode && Right() && !failed) g.inPanel = true;
        }
        else if (g.focus == kCreate) {
            if (Up()) g.formRow = std::max(0, g.formRow - 1);
            if (Down()) g.formRow = std::min(2, g.formRow + 1);
            const bool left = Left(), right = !left && Right();
            if (left || right) {
                const int dir = left ? -1 : 1;
                if (g.formRow == 0) g.settings.preset = (g.settings.preset + dir + keeper::kPresetCount) % keeper::kPresetCount;
                else if (g.formRow == 1) g.settings.isPublic = !g.settings.isPublic;
                else g.settings.tables = std::clamp(g.settings.tables + dir, 1, keeper::kMaxTables);
            }
        }
        else if (g.focus == kCode && !failed) {
            if (Left()) g.key = g.key % kKeyCols == 0 ? g.key : g.key - 1;
            if (Right()) g.key = std::min(kKeys - 1, g.key % kKeyCols == kKeyCols - 1 ? g.key : g.key + 1);
            if (Up()) g.key = g.key >= kKeyCols ? g.key - kKeyCols : g.key;
            if (Down()) g.key = std::min(kKeys - 1, g.key + kKeyCols);
        }
        // Typing on a keyboard goes straight into the code.
        if (g.focus == kCode && takeInput && !failed) {
            for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
                const char up = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
                if (c < 128 && std::strchr(kCodeChars, up) && up && g.code.size() < 6) {
                    g.code += up;
                    g.inPanel = true;
                }
            }
            if (Pressed({ImGuiKey_Backspace}, true) && !g.code.empty()) g.code.pop_back();
        }
        // Mouse: rows focus on click.
        if (Clicked()) {
            if (MouseIn(g_v.P(48, 80), g_v.P(868, 152))) g.focus = kCreate, g.inPanel = false;
            else if (MouseIn(g_v.P(48, 160), g_v.P(868, 232))) g.focus = kCode, g.inPanel = !failed;
            for (int i = 0; i < count; i++) {
                const float y = kListTop + (i - g.scroll) * kRowPitch;
                if (y >= kListTop - 1 && y + kRowH <= 832 && MouseIn(g_v.P(48, y), g_v.P(868, y + kRowH))) {
                    if (g.focus == i) in->join = rooms[i].code;
                    g.focus = i;
                    g.inPanel = false;
                }
            }
        }
    }
    // The list keeps the focused room in view.
    if (g.focus >= 0) {
        if (g.focus < g.scroll) g.scroll = static_cast<float>(g.focus);
        if (g.focus > g.scroll + kRowsInView - 1) g.scroll = static_cast<float>(g.focus - kRowsInView + 1);
    }
    g.scroll = std::clamp(g.scroll, 0.0f, static_cast<float>(std::max(0, count - kRowsInView)));

    // ---- the list ----
    int players = 0;
    for (const auto& r : rooms) players += r.members;
    Text(d, f.display, g_v.L(40), g_v.P(48, 22), Rgba(255, 255, 255), "CUSTOM ROOMS", 0, true);
    const std::string summary = v.keeper ? Plural(count, "public room", "public rooms") + " \xC2\xB7 " + Plural(players, "player", "players")
                                         : v.state == S::Failed ? "Cannot reach the rooms server" : "Custom rooms are offline right now";
    Text(d, f.body, g_v.L(16), g_v.P(48 + TextW(f.display, 40, "CUSTOM ROOMS") + 28, 42), kSoft, summary);
    ActionRow(d, f, 80, "+", "CREATE A ROOM", "Public or private, 1 to 8 tables", g.focus == kCreate);
    ActionRow(d, f, 160, "#", "JOIN WITH A CODE", "For a private room, or any room a friend sent you", g.focus == kCode);
    Text(d, f.body, g_v.L(13), g_v.P(48, 250), kMuted, "PUBLIC ROOMS \xC2\xB7 " + std::to_string(count));
    d->PushClipRect(g_v.P(48, kListTop - 2), g_v.P(880, 832), true);
    if (!count) {
        d->AddRectFilled(g_v.P(48, kListTop), g_v.P(868, kListTop + 160), Rgba(8, 9, 11, 179));
        d->AddLine(g_v.P(48, kListTop), g_v.P(868, kListTop), Rgba(201, 168, 92, 90), g_v.L(1));
        Text(d, f.strong, g_v.L(20), g_v.P(72, kListTop + 52), kCream, "No public rooms right now");
        Text(d, f.body, g_v.L(16), g_v.P(72, kListTop + 84), kMuted,
             "Create one and it shows here for everyone. Private rooms are never listed.");
    }
    for (int i = 0; i < count; i++) {
        const float y = kListTop + (i - g.scroll) * kRowPitch;
        if (y > 832 || y + kRowH < kListTop) continue;
        RoomRow(d, f, y, rooms[i], g.focus == i);
    }
    if (count > kRowsInView) {
        const float trackTop = kListTop, trackH = 832 - kListTop;
        d->AddRectFilled(g_v.P(874, trackTop), g_v.P(879, trackTop + trackH), Rgba(255, 255, 255, 20), g_v.L(3));
        const float thumb = trackH * kRowsInView / count, top = trackTop + (trackH - thumb) * g.scroll / (count - kRowsInView);
        d->AddRectFilled(g_v.P(874, top), g_v.P(879, top + thumb), Rgba(233, 196, 106, 190), g_v.L(3));
    }
    d->PopClipRect();

    // ---- the panel ----
    PanelFrame(d);
    int formRow = -1, formStep = -1, key = -1;
    if (v.state == S::Creating) {
        WaitPanel(d, f, "NEW ROOM", "Making your room", "The keeper is opening it; you go straight in.");
    }
    else if (v.state == S::Joining) {
        WaitPanel(d, f, "JOINING", "Into the room", "A moment.");
    }
    else if (v.state == S::Room) {
        WaitPanel(d, f, "IN A ROOM", "Room " + WeyveService::DisplayCode(v.room), "The room screen opens.");
    }
    else if (g.focus == kCreate) {
        CreatePanel(d, f, g.inPanel, &formRow, &formStep);
    }
    else if (g.focus == kCode) {
        CodePanel(d, f, g.inPanel, failed ? v.notice : "", &key);
    }
    else if (g.focus >= 0 && g.focus < count) {
        RoomPanel(d, f, rooms[g.focus], v.peek.code == rooms[g.focus].code ? &v.peek : nullptr);
    }
    if (formRow >= 0) {
        g.focus = kCreate;
        g.inPanel = true;
        g.formRow = formRow;
        if (formStep >= 0) {
            const int dir = formStep == 0 ? -1 : 1;
            if (g.formRow == 0) g.settings.preset = (g.settings.preset + dir + keeper::kPresetCount) % keeper::kPresetCount;
            else if (g.formRow == 1) g.settings.isPublic = !g.settings.isPublic;
            else g.settings.tables = std::clamp(g.settings.tables + dir, 1, keeper::kMaxTables);
        }
    }

    // ---- the stripe, and the buttons ----
    std::string lead, text;
    ImU32 leadCol = kGold;
    std::vector<std::pair<const char*, std::string>> prompts;
    const bool roomFull = g.focus >= 0 && g.focus < count && rooms[g.focus].members >= keeper::kRoomCap;
    if (busy || v.state == S::Room) {
        text = v.state == S::Creating ? "Making your room." : "Joining.";
    }
    else if (!v.keeper && (g.focus == kCreate || g.focus >= 0)) {
        lead = "OFFLINE";
        leadCol = kMuted;
        text = v.state == S::Failed ? "The rooms server cannot be reached." : "Custom rooms are not running right now; try again later.";
        prompts = {{"button_b", "Back"}};
    }
    else if (g.focus == kCreate) {
        if (!v.notice.empty() && v.notice != "No room on that code" && v.notice != "Room full") {
            lead = "NOT MADE";
            leadCol = kRed;
            text = v.notice;
        }
        else if (!count && g.formRow == 0) {
            lead = "NO PUBLIC ROOMS";
            leadCol = kMuted;
            text = "Nobody has a public room open.";
        }
        else {
            text = g.formRow == 0 ? "Left and right: what the room is for. Twelve names to pick from."
                   : g.formRow == 1 ? "Left and right: public or private."
                                    : "Left and right: 1 to 8 tables. With 8, a full room can all play at once.";
        }
        prompts = {{"button_start", "Create"}, {"button_b", "Back"}};
        if (!g.inPanel) prompts.insert(prompts.begin(), {"button_a", "Change"});
    }
    else if (g.focus == kCode) {
        if (failed) prompts = {{"button_a", "Try again"}, {"button_b", "Back"}};
        else {
            text = "Type the code you were sent.";
            prompts = {{"button_a", "Type"}, {"button_x", "Delete"}, {"button_y", "Paste"}, {"button_start", "Join"}, {"button_b", "Back"}};
        }
    }
    else if (roomFull) {
        lead = "ROOM FULL";
        leadCol = kRed;
        text = "Try again once someone leaves. The list updates by itself.";
        prompts = {{"button_b", "Back"}};
    }
    else if (g.focus >= 0) {
        text = rooms[g.focus].openSeats ? "Open seats: you can sit down as soon as you are in."
                                        : "Every seat is taken: you can queue or watch once you are in.";
        prompts = {{"button_a", "Join"}, {"button_b", "Back"}};
    }
    Bar(d, f, lead, leadCol, text, prompts, &barClick);
    const char* clicked = barClick >= 0 && barClick < static_cast<int>(prompts.size()) ? prompts[barClick].first : "";
    auto is = [&](const char* icon) { return std::strcmp(clicked, icon) == 0; };

    if (!busy && v.state != S::Room) {
        const bool a = Confirm() || is("button_a"), b = Back() || is("button_b");
        const bool start = Pressed({ImGuiKey_GamepadStart}, false) || is("button_start") ||
                           (g.inPanel && Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false) && g.focus == kCode);
        const bool x = Pressed({ImGuiKey_GamepadFaceLeft}, false) || is("button_x");
        const bool y = Pressed({ImGuiKey_GamepadFaceUp}, false) || is("button_y");
        if (g.focus == kCreate) {
            if (start && v.keeper) {
                in->create = true;
                in->settings = g.settings;
            }
            else if (a && !g.inPanel) g.inPanel = true;
            else if (b) {
                if (g.inPanel) g.inPanel = false;
                else in->back = true;
            }
        }
        else if (g.focus == kCode) {
            if (failed) {
                if (a) {
                    in->clearNotice = true;
                    g.code = g.lastCode;
                    g.inPanel = true;
                }
                else if (b) {
                    in->clearNotice = true;
                    g.inPanel = false;
                }
            }
            else {
                if (key >= 0) {
                    g.key = key;
                    g.inPanel = true;
                    if (g.code.size() < 6) g.code += kCodeChars[key];
                }
                if (a && !g.inPanel) g.inPanel = true;
                else if (a && g.code.size() < 6 && !Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space}, false)) g.code += kCodeChars[g.key];
                if (x && !g.code.empty()) g.code.pop_back();
                if (y) {
                    std::string pasted;
                    if (const char* clip = ImGui::GetClipboardText(); clip && WeyveService::NormaliseCode(clip, &pasted)) g.code = pasted;
                }
                if (start && g.code.size() == 6) {
                    g.lastCode = g.code;
                    in->join = g.code;
                }
                if (b) {
                    if (g.inPanel) g.inPanel = false;
                    else in->back = true;
                }
            }
        }
        else if (g.focus >= 0 && g.focus < count) {
            if (a && !roomFull) in->join = rooms[g.focus].code;
            if (b) in->back = true;
        }
        else if (b) {
            in->back = true;
        }
    }
    if (g.focus >= 0 && g.focus < count && v.state == S::Lobby) in->focusedRoom = rooms[g.focus].code;
    g_inputOn = false;
    d->PopClipRect();
}

} // namespace sfv::mod
