#include "mod/rooms_screen.h"

#include "core/country_flags.h"
#include "core/rules.h"
#include "mod/flow.h"
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
const ImU32 kSoft = IM_COL32(228, 224, 212, 255);
const ImU32 kMuted = IM_COL32(198, 193, 180, 255);
const ImU32 kGreen = IM_COL32(127, 216, 139, 255);
const ImU32 kRed = IM_COL32(255, 107, 94, 255);

constexpr const char kCodeChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // a room code's alphabet
constexpr int kKeyCols = 9;
constexpr int kKeys = 32;
constexpr float kRowPitch = 92, kRowH = 84, kListTop = 274;
constexpr int kRowsInView = 6;

enum Focus { kCreate = -2, kCode = -1 };

struct State {
    int focus = kCreate;    // kCreate, kCode, or a public room's index
    bool inPanel = false;   // working in the right panel (the form's rows, the keypad)
    int formRow = 0;        // the form's row (FormRows)
    keeper::RoomSettings settings;
    int title = 0;          // the offered title (keeper::TitleId)
    int chara[2] = {0, 3};  // its characters, when it names them (keeper::CharacterCode)
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

} // namespace

float FitText(ImDrawList* d, ImFont* f, float size, float minSize, float maxW, ImVec2 at, ImU32 c, std::string t, float align, bool stage)
{
    float s = size;
    while (s > minSize && TextW(f, s, t) > maxW) s -= 1;
    if (TextW(f, s, t) > maxW) {
        while (t.size() > 1 && TextW(f, s, t + "...") > maxW) {
            t.pop_back();
            while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back(); // whole characters
            if (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0xC0) t.pop_back();
        }
        t += "...";
    }
    // Kept on the baseline of the full size.
    const ImVec2 shifted(at.x, at.y + g_v.L((size - s) * 0.5f));
    if (stage) StageText(d, f, s, (shifted.x - g_v.ox) / g_v.s, (shifted.y - g_v.oy) / g_v.s, c, t, align);
    else Text(d, f, g_v.L(s), shifted, c, t, align);
    return TextW(f, s, t);
}

namespace {

// The form's rows: the title, its characters when it names them, then
// visibility and tables.
enum FormRow { kRowTitle, kRowChara1, kRowChara2, kRowVisibility, kRowTables };

std::vector<FormRow> FormRows()
{
    std::vector<FormRow> rows = {kRowTitle};
    const int n = keeper::TitleCharacters(keeper::TitleId(g.title));
    if (n > 0) rows.push_back(kRowChara1);
    if (n > 1) rows.push_back(kRowChara2);
    rows.push_back(kRowVisibility);
    rows.push_back(kRowTables);
    return rows;
}

// The settings' title from the form's choices.
void SyncTitle()
{
    keeper::RoomTitle t;
    t.id = keeper::TitleId(g.title);
    for (int i = 0; i < keeper::TitleCharacters(t.id); i++) t.chara[i] = keeper::CharacterCode(g.chara[i]);
    g.settings.title = t;
}

void StepRow(FormRow row, int dir)
{
    if (row == kRowTitle) g.title = (g.title + dir + keeper::kTitleCount) % keeper::kTitleCount;
    else if (row == kRowChara1 || row == kRowChara2) {
        int& c = g.chara[row == kRowChara1 ? 0 : 1];
        c = (c + dir + keeper::kCharacterCount) % keeper::kCharacterCount;
    }
    else if (row == kRowVisibility) g.settings.isPublic = !g.settings.isPublic;
    else g.settings.tables = std::clamp(g.settings.tables + dir, 1, keeper::kMaxTables);
    SyncTitle();
    g.formRow = std::min(g.formRow, static_cast<int>(FormRows().size()) - 1);
}

// An action row (Create a room, Join with a code): its glyph and its name,
// nothing under it (the hint at the bottom says what it does). A glass pane,
// solid gold with the focus.
void ActionRow(ImDrawList* d, const HubFonts& f, float y, const char* glyph, const char* title, const char* text, bool on)
{
    (void)text;
    if (on) {
        DropShadow(d, 48, y, 868, y + 72, 6, 18, 0.32f, true);
        Control(d, 48, y, 868, y + 72, true);
    }
    else Pane(d, 48, y, 868, y + 72);
    const ImU32 ink = on ? look::kInk : kGold;
    d->AddCircle(g_v.P(90, y + 36), g_v.L(20), ink, 32, g_v.L(2));
    Text(d, f.bold ? f.bold : f.strong, g_v.L(24), g_v.P(90, y + 21), ink, glyph, 0.5f);
    Text(d, f.display, g_v.L(21), g_v.P(130, y + 24), on ? look::kInk : kCream, title);
}

// A public room: a glass pane, solid gold with the focus (its text in ink).
void RoomRow(ImDrawList* d, const HubFonts& f, float y, const keeper::ListedRoom& r, bool on)
{
    const bool full = r.members >= keeper::kRoomCap;
    if (on) {
        DropShadow(d, 48, y, 868, y + kRowH, 6, 18, 0.32f, true);
        Control(d, 48, y, 868, y + kRowH, true);
    }
    else Pane(d, 48, y, 868, y + kRowH);
    const int fade = full && !on ? 158 : 255;
    const ImU32 title = on ? look::kInk : Alpha(kCream, fade), sub = on ? look::kInkSoft : Alpha(kMuted, fade);
    FitText(d, f.display, 21, 15, 400, g_v.P(68, y + 15), title, RoomTitleText(r.title, r.owner));
    DrawCrown(d, 68, y + 61, 1, on ? look::kInk : Alpha(kGold, fade)); // centred on the flag
    // The owner's flag (the rooms server's lookup), then the name.
    float nx = 90;
    if (const float fw = DrawFlag(d, f.strong, r.ownerCountry, nx, y + 50, 12, fade / 255.0f)) nx += fw + 6;
    ImFont* bold = f.bold ? f.bold : f.strong;
    Text(d, bold, g_v.L(17), g_v.P(nx, y + 46), on ? look::kInk : Alpha(kSoft, fade), Upper(r.owner));
    const float ox = nx + TextW(bold, 17, Upper(r.owner));
    std::string detail = " \xC2\xB7 " + Plural(r.tables, "table", "tables");
    const std::string ft = FirstTo(r.rules);
    if (!ft.empty()) detail += " \xC2\xB7 " + ft;
    Text(d, f.body, g_v.L(17), g_v.P(ox, y + 46), sub, detail);
    float fx = 484;
    for (size_t i = 0; i < r.faces.size() && i < 6; i++) {
        DrawFace(d, r.faces[i], fx, y + 22, 30, 40);
        fx += 32;
    }
    const std::string count = std::to_string(r.members) + " / " + std::to_string(keeper::kRoomCap);
    const float cw = TextW(bold, 20, count);
    Text(d, bold, g_v.L(20), g_v.P(848, y + 14), title, count, 1);
    DrawPerson(d, 848 - cw - 24, y + 26, 16, on ? look::kInk : Alpha(kSoft, fade));
    ImU32 col = kGreen;
    const std::string seats = OpenSeatsText(r, &col);
    if (on) col = col == kGreen ? Rgba(31, 74, 39) : look::kInkSoft;
    Text(d, bold, g_v.L(17), g_v.P(848, y + 48), col, seats, 1);
}

// The right column: a glass pane with a dark strip for its kicker.
void PanelFrame(ImDrawList* d)
{
    Pane(d, 892, 80, 1552, 832);
    Strip(d, 894, 82, 1550, 124, false);
}

// The panel's kicker, in its strip.
void Kicker(ImDrawList* d, const HubFonts& f, const char* kicker, ImU32 col = kGold)
{
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(918, 94), col, kicker);
}

void RoomPanel(ImDrawList* d, const HubFonts& f, const keeper::ListedRoom& r, const keeper::RoomDetail* detail)
{
    const bool full = r.members >= keeper::kRoomCap;
    Kicker(d, f, full ? "FULL \xC2\xB7 16 OF 16" : "PUBLIC ROOM", full ? kRed : kGold);
    Text(d, f.body, g_v.L(17), g_v.P(1526, 95), kSoft, r.code, 1);
    FitText(d, f.display, 28, 20, 608, g_v.P(918, 138), kCream, RoomTitleText(r.title, r.owner));
    DrawCrown(d, 918, 202, 1, kGold);
    float ox = 940;
    if (const float fw = DrawFlag(d, f.strong, r.ownerCountry, ox, 189, 13)) ox += fw + 8;
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(ox, 186), kCream, Upper(r.owner));
    Text(d, f.body, g_v.L(16), g_v.P(ox + TextW(f.bold ? f.bold : f.strong, 16, Upper(r.owner)), 186), kSoft,
         " \xC2\xB7 " + std::to_string(r.members) + " / 16 players \xC2\xB7 " + Plural(r.tables, "table", "tables"));
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(918, 224), kGold, "TABLES");
    // Each table's rules from the keeper's detail; before it comes, the
    // first table's from the list.
    const int shown = std::min(r.tables, 5);
    float y = 248;
    for (int t = 0; t < shown; t++) {
        Control(d, 918, y, 1526, y + 52, false);
        Text(d, f.display, g_v.L(17), g_v.P(935, y + 16), kCream, "TABLE " + std::to_string(t + 1));
        const std::string text = detail && t < static_cast<int>(detail->tables.size()) ? detail->tables[t] : t == 0 ? r.rules : "";
        Rules rules;
        if (ParseRules(text, &rules)) {
            std::string line = DescribeRules(rules);
            while (line.size() > 4 && TextW(f.body, 16, line) > 330) line = line.substr(0, line.size() - 4) + "...";
            Text(d, f.body, g_v.L(16), g_v.P(1035, y + 17), kGold, line);
        }
        Text(d, f.strong, g_v.L(17), g_v.P(1512, y + 16), kGreen, "Open seat", 1);
        y += 58;
    }
    if (r.tables > shown) {
        Text(d, f.body, g_v.L(16), g_v.P(918, y), kMuted, "+" + Plural(r.tables - shown, "more table", "more tables"));
        y += 26;
    }
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(918, y + 10), kGold, "PLAYERS \xC2\xB7 " + std::to_string(r.members) + " / 16");
    if (detail) {
        // Two to a row, as many rows as the panel has room for: a cell with
        // the face, the flag and the name.
        ImFont* bold = f.bold ? f.bold : f.strong;
        const int count = static_cast<int>(detail->people.size());
        const float top = y + 36;
        const int rows = std::max(1, static_cast<int>((812 - top) / 48));
        const int shownPeople = count > rows * 2 ? rows * 2 - 1 : count; // the last cell says how many more
        for (int i = 0; i < shownPeople; i++) {
            const float px = 918 + (i % 2) * 308.0f, py = top + (i / 2) * 48.0f;
            Control(d, px, py, px + 300, py + 40, false);
            DrawFace(d, detail->people[i].chara, px + 6, py + 4, 26, 32);
            float nx = px + 44;
            if (const float fw = DrawFlag(d, f.strong, detail->people[i].country, nx, py + 14, 13)) nx += fw + 8;
            std::string name = Upper(detail->people[i].name);
            while (name.size() > 1 && TextW(bold, 17, name) > px + 288 - nx) name.pop_back();
            Text(d, bold, g_v.L(17), g_v.P(nx, py + 11), kCream, name);
        }
        if (shownPeople < count) {
            const float px = 918 + (shownPeople % 2) * 308.0f, py = top + (shownPeople / 2) * 48.0f;
            Text(d, f.body, g_v.L(17), g_v.P(px + 14, py + 11), kMuted, "+" + std::to_string(count - shownPeople) + " more");
        }
        return;
    }
    float fx = 918;
    for (const std::string& face : r.faces) {
        DrawFace(d, face, fx, y + 36, 22, 30);
        fx += 28;
    }
}

void CreatePanel(ImDrawList* d, const HubFonts& f, bool active, int* clickedRow, int* clickedStep)
{
    Kicker(d, f, "NEW ROOM");
    Text(d, f.display, g_v.L(28), g_v.P(918, 138), kCream, "Create a room");
    const std::vector<FormRow> rows = FormRows();
    auto label = [](FormRow row) {
        return row == kRowTitle ? "Title" : row == kRowChara1 ? "Character 1" : row == kRowChara2 ? "Character 2" : row == kRowVisibility ? "Visibility" : "Tables";
    };
    auto value = [](FormRow row) -> std::string {
        if (row == kRowTitle) return g.title == 0 ? std::string("No title") : RoomTitleText(g.settings.title, "");
        if (row == kRowChara1 || row == kRowChara2) return keeper::CharacterName(keeper::CharacterCode(g.chara[row == kRowChara1 ? 0 : 1]));
        if (row == kRowVisibility) return g.settings.isPublic ? "Public" : "Private";
        return Plural(g.settings.tables, "table", "tables");
    };
    *clickedRow = *clickedStep = -1;
    const int n = static_cast<int>(rows.size());
    for (int i = 0; i < n; i++) {
        const float y = 196 + i * 60.0f;
        const bool on = g.formRow == i;
        const ImVec2 a = g_v.P(918, y), b = g_v.P(1526, y + 54);
        // Focused while the form has the pad: gold; the row the list's left
        // and right change: gold at half strength.
        const bool lit = on && active;
        Control(d, 918, y, 1526, y + 54, lit);
        if (on && !active) d->AddRect(a, b, look::kRing, 0, 0, g_v.L(2));
        ImFont* bold = f.bold ? f.bold : f.strong;
        Text(d, lit ? bold : f.strong, g_v.L(19), g_v.P(940, y + 15), lit ? look::kInk : kCream, label(rows[i]));
        // Between the arrows (1146 to 1500), shrunk to fit.
        FitText(d, lit ? bold : f.body, 19, 13, 330, g_v.P(1323, y + 15), lit ? look::kInk : kCream, value(rows[i]), 0.5f);
        if (on) {
            // ◀ and ▶ around the value.
            d->AddTriangleFilled(g_v.P(1146, y + 21), g_v.P(1146, y + 33), g_v.P(1136, y + 27), lit ? look::kInk : kGold);
            d->AddTriangleFilled(g_v.P(1500, y + 21), g_v.P(1510, y + 27), g_v.P(1500, y + 33), lit ? look::kInk : kGold);
        }
        if (Clicked() && MouseIn(a, b)) {
            *clickedRow = i;
            if (MouseIn(g_v.P(1120, y), g_v.P(1170, y + 54))) *clickedStep = 0;
            else if (MouseIn(g_v.P(1480, y), g_v.P(1526, y + 54))) *clickedStep = 1;
        }
    }
    const char* visibility = g.settings.isPublic
                                 ? "Listed for anyone to join. Friends can also use the code."
                                 : "Not listed. Only people with the code can join.";
    const float below = 196 + n * 60.0f + 14;
    Text(d, f.body, g_v.L(17), g_v.P(918, below), kSoft, visibility);
    Text(d, f.body, g_v.L(17), g_v.P(918, below + 35), kMuted, "Up to 16 players. Set each table's rules from the room.");
}

void CodePanel(ImDrawList* d, const HubFonts& f, bool active, const std::string& notice, int* clickedKey)
{
    Kicker(d, f, "ROOM CODE");
    Text(d, f.display, g_v.L(28), g_v.P(918, 138), kCream, "Join with a code");
    const float slotW = (608 - 50) / 6.0f;
    for (int i = 0; i < 6; i++) {
        const float x = 918 + i * (slotW + 10);
        d->AddRectFilled(g_v.P(x, 200), g_v.P(x + slotW, 296), Rgba(0, 0, 0, 90));
        d->AddRect(g_v.P(x, 200), g_v.P(x + slotW, 296), look::kEdgeControl, 0, 0, std::max(1.0f, g_v.L(1)));
        const bool next = active && static_cast<int>(g.code.size()) == i;
        d->AddRectFilled(g_v.P(x, 292), g_v.P(x + slotW, 296), next ? kGold : look::kEdgeControl);
        if (i < static_cast<int>(g.code.size())) {
            // Streamer mode: a dot for each letter typed (a friend's code
            // stays off the stream too).
            if (StreamerMode()) d->AddCircleFilled(g_v.P(x + slotW / 2, 246), g_v.L(9), Rgba(255, 255, 255), 20);
            else Text(d, f.display, g_v.L(52), g_v.P(x + slotW / 2, 214), Rgba(255, 255, 255), std::string(1, g.code[i]), 0.5f);
        }
    }
    *clickedKey = -1;
    if (!notice.empty()) {
        const bool full = notice == "Room full";
        const bool version = notice.rfind("That room is on SFVR", 0) == 0;
        const ImU32 line = full ? kGold : kRed;
        const ImVec2 a = g_v.P(918, 324), b = g_v.P(1526, 470);
        d->AddRectFilled(a, b, look::kControl);
        d->AddRect(a, b, line, 0, 0, g_v.L(2));
        Text(d, f.strong, g_v.L(16), g_v.P(944, 346), line, full ? "ROOM FULL" : version ? "OTHER VERSION" : "NO SUCH ROOM");
        const std::string code = g.lastCode.empty() ? g.code : g.lastCode;
        const bool hide = StreamerMode() || code.empty(); // no code to show: the plain wording
        Text(d, f.display, g_v.L(26), g_v.P(944, 374), kCream,
             full      ? (hide ? std::string("That room has 16 players") : code + " has 16 players")
             : version ? (hide ? std::string("That room is on another version") : code + " is on another version")
                       : (hide ? std::string("Nothing open on that code") : "Nothing open on " + code));
        Text(d, f.body, g_v.L(17), g_v.P(944, 418), kSoft,
             full      ? std::string("Rooms hold 16 players. Try again when someone leaves.")
             : version ? notice
                       : std::string("Check the code. Rooms close once everyone leaves."));
        return;
    }
    const float keyW = (608 - 8 * (kKeyCols - 1)) / float(kKeyCols);
    for (int k = 0; k < kKeys; k++) {
        const float x = 918 + (k % kKeyCols) * (keyW + 8), y = 324 + (k / kKeyCols) * 62.0f;
        const ImVec2 a = g_v.P(x, y), b = g_v.P(x + keyW, y + 54);
        const bool on = active && g.key == k;
        Control(d, x, y, x + keyW, y + 54, on);
        Text(d, f.bold ? f.bold : f.strong, g_v.L(22), g_v.P(x + keyW / 2, y + 13), on ? look::kInk : kCream, std::string(1, kCodeChars[k]), 0.5f);
        if (Clicked() && MouseIn(a, b)) *clickedKey = k;
    }
}

void WaitPanel(ImDrawList* d, const HubFonts& f, const char* kicker, const std::string& title, const std::string& text)
{
    Kicker(d, f, kicker);
    Text(d, f.display, g_v.L(28), g_v.P(918, 138), kCream, title);
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

std::string RoomTitleText(const keeper::RoomTitle& title, const std::string& owner)
{
    if (title.id == 0 || !keeper::ValidTitle(title)) return owner.empty() ? std::string("Room") : owner + "'s room";
    std::string s = LoungeCommentText(title.id);
    if (s.empty()) s = keeper::TitleTemplate(title.id);
    for (int i = 0; i < 2; i++) {
        const std::string key = "{common" + std::to_string(i + 1) + "}";
        const size_t at = s.find(key);
        if (at != std::string::npos) s.replace(at, key.size(), keeper::CharacterName(title.chara[i]));
    }
    return s;
}

float DrawFlag(ImDrawList* d, ImFont* tagFont, const std::string& country, float x, float y, float h, float alpha)
{
    if (country.size() != 2) return 0;
    const float w = h * kFlagAspect;
    const ImU32 edge = Rgba(0, 0, 0, static_cast<int>(150 * alpha));
    const std::string_view asset = FlagAsset(country);
    if (asset.empty()) {
        // No flag in the game for it: the two letters in a plate as wide as
        // they need.
        const float size = h * 1.05f, pw = std::max(w, TextW(tagFont, size, country) + h * 0.7f);
        d->AddRectFilled(g_v.P(x, y), g_v.P(x + pw, y + h), Rgba(70, 74, 82, static_cast<int>(230 * alpha)), g_v.L(2));
        Text(d, tagFont, g_v.L(size), g_v.P(x + pw / 2, y - h * 0.08f), Rgba(236, 232, 220, static_cast<int>(255 * alpha)), country, 0.5f);
        return pw;
    }
    d->AddRectFilled(g_v.P(x - 1, y - 1), g_v.P(x + w + 1, y + h + 1), edge, g_v.L(1));
    // The textures are 128 x 64 with the flag (96 x 64, 3:2) in the middle
    // and clear margins at the sides: those are left out.
    constexpr float kU0 = 16.0f / 128, kU1 = 112.0f / 128;
    if (const TextureView* t = TextureSized("/Game/CommonAsset/CountryFlags/" + std::string(asset), g_v.L(w) / (kU1 - kU0)))
        Image(d, t, g_v.P(x, y), g_v.P(x + w, y + h), ImVec2(kU0, 0), ImVec2(kU1, 1), Rgba(255, 255, 255, static_cast<int>(255 * alpha)));
    return w;
}

namespace {
std::atomic<bool> g_streamerMode{false};
}

void SetStreamerMode(bool on) { g_streamerMode = on; }
bool StreamerMode() { return g_streamerMode; }

float DrawHiddenCode(ImDrawList* d, float x, float y, float size, ImU32 col)
{
    // Six dots where the code's six letters would be.
    const float r = size * 0.16f, step = size * 0.5f;
    for (int i = 0; d && i < 6; i++) d->AddCircleFilled(g_v.P(x + r + i * step, y + size * 0.62f), g_v.L(r), col, 12);
    return 5 * step + 2 * r;
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
    Backdrop(d);
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    using S = RoomsView::State;
    const std::vector<keeper::ListedRoom>& rooms = v.publicRooms;
    const int count = static_cast<int>(rooms.size());
    if (g.focus >= count) g.focus = count ? count - 1 : kCode;
    const bool busy = v.state == S::Creating || v.state == S::Joining;
    const bool versionNotice = v.notice.rfind("That room is on SFVR", 0) == 0;
    const bool failed = !v.notice.empty() && (v.notice == "No room on that code" || v.notice == "Room full" || versionNotice) && g.focus == kCode;

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
                StepRow(FormRows()[std::min<size_t>(g.formRow, FormRows().size() - 1)], dir);
            }
            if (g.focus == kCode && Right() && !failed) g.inPanel = true;
        }
        else if (g.focus == kCreate) {
            if (Up()) g.formRow = std::max(0, g.formRow - 1);
            if (Down()) g.formRow = std::min(static_cast<int>(FormRows().size()) - 1, g.formRow + 1);
            const bool left = Left(), right = !left && Right();
            if (left || right) StepRow(FormRows()[g.formRow], left ? -1 : 1);
        }
        else if (g.focus == kCode && !failed) {
            if (Left()) g.key = g.key % kKeyCols == 0 ? g.key : g.key - 1;
            if (Right()) g.key = std::min(kKeys - 1, g.key % kKeyCols == kKeyCols - 1 ? g.key : g.key + 1);
            if (Up()) g.key = g.key >= kKeyCols ? g.key - kKeyCols : g.key;
            if (Down()) g.key = std::min(kKeys - 1, g.key + kKeyCols);
        }
        // Typing on a keyboard goes straight into the code; in its panel
        // every key is itself (not the game's keys: a code has their letters).
        if (g.focus == kCode && g.inPanel && takeInput && !failed) ui::g_textEntry = true;
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
    // The desk's summary (it comes before the list itself).
    StageText(d, f.display, 40, 48, 22, Rgba(255, 255, 255), "CUSTOM ROOMS");
    const std::string summary = v.keeper ? Plural(v.summary.publicRooms, "public room", "public rooms") + " \xC2\xB7 " +
                                               Plural(v.summary.players, "player", "players")
                                         : v.state == S::Failed ? "Can't reach the rooms server" : "Custom Rooms are unavailable right now";
    StageText(d, f.body, 20, 48 + TextW(f.display, 40, "CUSTOM ROOMS") + 28, 39, kCream, summary);
    ActionRow(d, f, 80, "+", "CREATE A ROOM", "Public or private, 1 to 8 tables", g.focus == kCreate);
    ActionRow(d, f, 160, "#", "JOIN WITH A CODE", "For private rooms, or a code from a friend", g.focus == kCode);
    StageText(d, f.bold ? f.bold : f.strong, 16, 48, 246, kGold, "PUBLIC ROOMS \xC2\xB7 " + std::to_string(count));
    d->PushClipRect(g_v.P(30, kListTop - 2), g_v.P(886, 840), true);
    if (!count) {
        Pane(d, 48, kListTop, 868, kListTop + 160);
        Text(d, f.strong, g_v.L(20), g_v.P(72, kListTop + 52), kCream, "No public rooms right now");
        Text(d, f.body, g_v.L(16), g_v.P(72, kListTop + 84), kMuted,
             "Create one and it shows up here.");
    }
    for (int i = 0; i < count; i++) {
        const float y = kListTop + (i - g.scroll) * kRowPitch;
        if (y > 832 || y + kRowH < kListTop) continue;
        RoomRow(d, f, y, rooms[i], g.focus == i);
    }
    d->PopClipRect();

    // More rooms than fit: the bar in the gutter (dark track, gold thumb).
    ScrollBar(d, 874, kListTop, 832, g.scroll, kRowsInView, static_cast<float>(count), true);

    // ---- the panel ----
    PanelFrame(d);
    int formRow = -1, formStep = -1, key = -1;
    if (v.state == S::Creating) {
        WaitPanel(d, f, "NEW ROOM", "Creating your room", "You go straight in once it's ready.");
    }
    else if (v.state == S::Joining) {
        WaitPanel(d, f, "JOINING", "Joining the room", "One moment.");
    }
    else if (v.state == S::Room) {
        WaitPanel(d, f, "IN A ROOM", StreamerMode() ? std::string("Your room") : "Room " + WeyveService::DisplayCode(v.room),
                  "Opening the room.");
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
            StepRow(FormRows()[g.formRow], formStep == 0 ? -1 : 1);
        }
    }

    // ---- the stripe, and the buttons ----
    std::string lead, text;
    ImU32 leadCol = kGold;
    std::vector<std::pair<const char*, std::string>> prompts;
    const bool roomFull = g.focus >= 0 && g.focus < count && rooms[g.focus].members >= keeper::kRoomCap;
    if (busy || v.state == S::Room) {
        text = v.state == S::Creating ? "Creating your room..." : "Joining the room...";
    }
    else if (!v.otherBuild.empty() && !v.keeper) {
        lead = "UPDATE NEEDED";
        leadCol = kRed;
        text = "Custom Rooms need a different SFVR version. Check for updates in SFVR settings.";
        prompts = {{"button_start", "SFVR settings"}, {"button_b", "Back"}};
    }
    else if (!v.keeper && (g.focus == kCreate || g.focus >= 0)) {
        lead = v.state == S::Failed ? "CANNOT CONNECT" : "OFFLINE";
        leadCol = v.state == S::Failed ? kRed : kMuted;
        text = v.state == S::Failed ? "Can't reach the rooms server. Check your connection, or the server in SFVR settings."
                                    : "Custom Rooms are unavailable right now. Try again later.";
        prompts = {};
        if (v.state == S::Failed) prompts.push_back({"button_start", "SFVR settings"});
        prompts.push_back({"button_b", "Back"});
    }
    else if (g.focus == kCreate) {
        if (!v.notice.empty() && v.notice != "No room on that code" && v.notice != "Room full" && !versionNotice) {
            // The notice is about the room just left (the owner's removal) or
            // about a room that was asked for.
            lead = v.notice.rfind("Removed", 0) == 0 ? "REMOVED" : v.notice.rfind("Cannot join", 0) == 0 ? "NOT JOINED" : "NOT MADE";
            leadCol = kRed;
            text = v.notice;
        }
        else if (!count && g.formRow == 0) {
            lead = "NO PUBLIC ROOMS";
            leadCol = kMuted;
            text = "Create one, or join with a code.";
        }
        else {
            const FormRow row = FormRows()[std::min<size_t>(g.formRow, FormRows().size() - 1)];
            text = !g.inPanel ? ""
                   : row == kRowTitle ? "Titles come from the game's Battle Lounge comments."
                   : row == kRowChara1 ? "The character you play."
                   : row == kRowChara2 ? "The character you want to face."
                   : row == kRowVisibility ? ""
                                           : "With 8 tables, all 16 players can play at once.";
        }
        prompts = {{"button_start", "Create"}, {"button_b", "Back"}};
        if (!g.inPanel) prompts.insert(prompts.begin(), {"button_a", "Change"});
    }
    else if (g.focus == kCode) {
        if (failed) prompts = {{"button_a", "Try again"}, {"button_b", "Back"}};
        else {
            text = "Enter the room code.";
            prompts = {{"button_a", "Type"}, {"button_x", "Delete"}, {"button_y", "Paste"}, {"button_start", "Join"}, {"button_b", "Back"}};
        }
    }
    else if (roomFull) {
        lead = "ROOM FULL";
        leadCol = kRed;
        text = "Try again when someone leaves.";
        prompts = {{"button_b", "Back"}};
    }
    else if (g.focus >= 0) {
        text = rooms[g.focus].openSeats ? "There are open seats." : "All seats are taken. You can still queue or watch.";
        prompts = {{"button_a", "Join"}, {"button_b", "Back"}};
    }
    PromptLine(d, f.body, f.bold ? f.bold : f.strong, lead, leadCol, text, prompts, &barClick);

    if (!busy && v.state != S::Room) {
        const bool typingCode = g.focus == kCode && g.inPanel && !failed;
        const bool a = Confirm(), b = Back() || (typingCode && Pressed({ImGuiKey_Escape}, false));
        const bool start = Pressed({ImGuiKey_GamepadStart}, false) ||
                           (g.inPanel && Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false) && g.focus == kCode);
        const bool x = Pressed({ImGuiKey_GamepadFaceLeft}, false);
        const bool y = Pressed({ImGuiKey_GamepadFaceUp}, false);
        if (!v.keeper && start && (v.state == S::Failed || !v.otherBuild.empty())) {
            in->settings_ = true;
        }
        else if (g.focus == kCreate) {
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
