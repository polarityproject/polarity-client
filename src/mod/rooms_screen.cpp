// The rooms screen: create a room, join with a code, the public list (docs/overlay.md).

#include "mod/rooms_screen.h"

#include "core/text.h"
#include "core/country_flags.h"
#include "core/rules.h"
#include "mod/flow.h"
#include "mod/log.h"
#include "mod/room_screen.h"
#include "mod/ui_kit.h"
#include "mod/weyve_service.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace sfv::mod {

using namespace ui;

namespace {

const ImU32 kGold = IM_COL32(233, 196, 106, 255);
const ImU32 kCream = IM_COL32(244, 241, 232, 255);
const ImU32 kSoft = IM_COL32(228, 224, 212, 255);
const ImU32 kMuted = IM_COL32(198, 193, 180, 255);
const ImU32 kGreen = IM_COL32(127, 216, 139, 255);
const ImU32 kRed = IM_COL32(255, 107, 94, 255);

constexpr const char kCodeChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int kKeyCols = 9;
constexpr int kKeys = 32;
constexpr float kRowPitch = 92, kRowH = 84, kListTop = 274;
constexpr int kRowsInView = 6;

enum Focus { kCreate = -2,
             kCode = -1 };

struct State {
    int focus = kCreate;
    bool inPanel = false;
    int formRow = 0;
    keeper::RoomSettings settings;
    int title = 0;
    int chara[2] = {0, 3};
    std::string code;
    int key = 0;
    float scroll = 0;
    std::string lastCode;
    TableRulesEditor rules;
    bool rulesOpen = false;
    std::vector<Rules> rulesBefore;
} g;

std::atomic<bool> g_open{false};

ImU32 Alpha(ImU32 c, int a) { return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT); }

std::string OpenSeatsText(const keeper::ListedRoom& r, ImU32* col) {
    if (r.members >= keeper::kRoomCap) {
        *col = kRed;
        return TR(Str::rooms_row_full);
    }
    if (r.openSeats > 0) {
        *col = kGreen;
        return TRF(r.openSeats == 1 ? Str::rooms_row_open_seat : Str::rooms_row_open_seats, r.openSeats);
    }
    *col = kGold;
    return TR(Str::rooms_row_queues_only);
}

std::string FirstTo(const std::string& rulesText) {
    Rules rules;
    if (!ParseRules(rulesText, &rules)) return "";
    return TRF(Str::rooms_row_first_to, rules.firstTo);
}

std::string Count(int n, Str one, Str many) { return TRF(n == 1 ? one : many, n); }

} // namespace

float RoomsScreenFitText(ImDrawList* d, ImFont* f, float size, float minSize, float maxW, ImVec2 at, ImU32 c, std::string t, float align, bool stage) {
    float s = size;
    while (s > minSize && TextW(f, s, t) > maxW) s -= 1;
    if (TextW(f, s, t) > maxW) {
        while (t.size() > 1 && TextW(f, s, t + "...") > maxW) {
            t.pop_back();
            while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
            if (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0xC0) t.pop_back();
        }
        t += "...";
    }
    const ImVec2 shifted(at.x, at.y + g_v.L((size - s) * 0.5f));
    if (stage) StageText(d, f, s, (shifted.x - g_v.ox) / g_v.s, (shifted.y - g_v.oy) / g_v.s, c, t, align);
    else Text(d, f, g_v.L(s), shifted, c, t, align);
    return TextW(f, s, t);
}

namespace {

enum FormRow { kRowTitle,
               kRowChara1,
               kRowChara2,
               kRowVisibility,
               kRowTables,
               kRowRules };

std::vector<FormRow> FormRows() {
    std::vector<FormRow> rows = {kRowTitle};
    const int n = keeper::TitleCharacters(keeper::TitleId(g.title));
    if (n > 0) rows.push_back(kRowChara1);
    if (n > 1) rows.push_back(kRowChara2);
    rows.push_back(kRowVisibility);
    rows.push_back(kRowTables);
    rows.push_back(kRowRules);
    return rows;
}

void SizeRules() {
    g.rules.rules.resize(static_cast<size_t>(std::clamp(g.settings.tables, 1, keeper::kMaxTables)), Rules{});
    g.rules.table = std::clamp(g.rules.table, 0, static_cast<int>(g.rules.rules.size()) - 1);
}

std::string RulesSummary() {
    SizeRules();
    int changed = 0;
    bool same = true;
    for (const Rules& r : g.rules.rules) {
        if (!(r == Rules{})) changed++;
        if (!(r == g.rules.rules.front())) same = false;
    }
    if (!changed) return TR(Str::rooms_rules_default);
    if (same && g.rules.rules.size() > 1) return TR(Str::rooms_rules_same);
    if (g.rules.rules.size() == 1) return TR(Str::rooms_rules_customized);
    return TRF(Str::rooms_rules_customized_tables, Count(changed, Str::common_table, Str::common_tables).c_str());
}

void OpenRulesEditor() {
    SizeRules();
    g.rulesBefore = g.rules.rules;
    g.rules.row = 0;
    g.rulesOpen = true;
}

void SyncTitle() {
    keeper::RoomTitle t;
    t.id = keeper::TitleId(g.title);
    for (int i = 0; i < keeper::TitleCharacters(t.id); i++) t.chara[i] = keeper::CharacterCode(g.chara[i]);
    g.settings.title = t;
}

void StepRow(FormRow row, int dir) {
    if (row == kRowTitle) g.title = (g.title + dir + keeper::kTitleCount) % keeper::kTitleCount;
    else if (row == kRowChara1 || row == kRowChara2) {
        int& c = g.chara[row == kRowChara1 ? 0 : 1];
        c = (c + dir + keeper::kCharacterCount) % keeper::kCharacterCount;
    } else if (row == kRowVisibility) g.settings.isPublic = !g.settings.isPublic;
    else if (row == kRowTables) g.settings.tables = std::clamp(g.settings.tables + dir, 1, keeper::kMaxTables);
    else return;
    SyncTitle();
    g.formRow = std::min(g.formRow, static_cast<int>(FormRows().size()) - 1);
}

void ActionRow(ImDrawList* d, const HubFonts& f, float y, const char* glyph, const char* title, const char* text, bool on) {
    (void)text;
    if (on) {
        DropShadow(d, 48, y, 868, y + 72, 6, 18, 0.32f, true);
        Control(d, 48, y, 868, y + 72, true);
    } else Pane(d, 48, y, 868, y + 72);
    const ImU32 ink = on ? look::kInk : kGold;
    d->AddCircle(g_v.P(90, y + 36), g_v.L(20), ink, 32, g_v.L(2));
    Text(d, f.bold ? f.bold : f.strong, g_v.L(24), g_v.P(90, y + 21), ink, glyph, 0.5f);
    Text(d, f.display, g_v.L(21), g_v.P(130, y + 24), on ? look::kInk : kCream, title);
}

void RoomRow(ImDrawList* d, const HubFonts& f, float y, const keeper::ListedRoom& r, bool on) {
    const bool full = r.members >= keeper::kRoomCap;
    if (on) {
        DropShadow(d, 48, y, 868, y + kRowH, 6, 18, 0.32f, true);
        Control(d, 48, y, 868, y + kRowH, true);
    } else Pane(d, 48, y, 868, y + kRowH);
    const int fade = full && !on ? 158 : 255;
    const ImU32 title = on ? look::kInk : Alpha(kCream, fade), sub = on ? look::kInkSoft : Alpha(kMuted, fade);
    RoomsScreenFitText(d, f.display, 21, 15, 400, g_v.P(68, y + 15), title, RoomsScreenTitleText(r.title, r.owner));
    RoomsScreenDrawCrown(d, 68, y + 61, 1, on ? look::kInk : Alpha(kGold, fade));
    float nx = 90;
    if (const float fw = RoomsScreenDrawFlag(d, f.strong, r.ownerCountry, nx, y + 50, 12, fade / 255.0f)) nx += fw + 6;
    ImFont* bold = f.bold ? f.bold : f.strong;
    Text(d, bold, g_v.L(17), g_v.P(nx, y + 46), on ? look::kInk : Alpha(kSoft, fade), Upper(r.owner));
    const float ox = nx + TextW(bold, 17, Upper(r.owner));
    std::string detail = TRF(Str::rooms_row_detail, Count(r.tables, Str::common_table, Str::common_tables).c_str());
    const std::string ft = FirstTo(r.rules);
    if (!ft.empty()) detail += TRF(Str::rooms_row_detail, ft.c_str());
    Text(d, f.body, g_v.L(17), g_v.P(ox, y + 46), sub, detail);
    float fx = 484;
    for (size_t i = 0; i < r.faces.size() && i < 6; i++) {
        RoomsScreenDrawFace(d, r.faces[i], fx, y + 22, 30, 40);
        fx += 32;
    }
    const std::string count = std::to_string(r.members) + " / " + std::to_string(keeper::kRoomCap);
    const float cw = TextW(bold, 20, count);
    Text(d, bold, g_v.L(20), g_v.P(848, y + 14), title, count, 1);
    RoomsScreenDrawPerson(d, 848 - cw - 24, y + 26, 16, on ? look::kInk : Alpha(kSoft, fade));
    ImU32 col = kGreen;
    const std::string seats = OpenSeatsText(r, &col);
    if (on) col = col == kGreen ? Rgba(31, 74, 39) : look::kInkSoft;
    Text(d, bold, g_v.L(17), g_v.P(848, y + 48), col, seats, 1);
}

void PanelFrame(ImDrawList* d) {
    Pane(d, 892, 80, 1552, 832);
    Strip(d, 894, 82, 1550, 124, false);
}

void Kicker(ImDrawList* d, const HubFonts& f, const char* kicker, ImU32 col = kGold) {
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(918, 94), col, kicker);
}

void RoomPanel(ImDrawList* d, const HubFonts& f, const keeper::ListedRoom& r, const keeper::RoomDetail* detail) {
    const bool full = r.members >= keeper::kRoomCap;
    Kicker(d, f, (full ? TRF(Str::rooms_panel_full, keeper::kRoomCap, keeper::kRoomCap) : std::string(TR(Str::rooms_panel_public))).c_str(), full ? kRed : kGold);
    Text(d, f.body, g_v.L(17), g_v.P(1526, 95), kSoft, r.code, 1);
    RoomsScreenFitText(d, f.display, 28, 20, 608, g_v.P(918, 138), kCream, RoomsScreenTitleText(r.title, r.owner));
    RoomsScreenDrawCrown(d, 918, 202, 1, kGold);
    float ox = 940;
    if (const float fw = RoomsScreenDrawFlag(d, f.strong, r.ownerCountry, ox, 189, 13)) ox += fw + 8;
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(ox, 186), kCream, Upper(r.owner));
    Text(d, f.body, g_v.L(16), g_v.P(ox + TextW(f.bold ? f.bold : f.strong, 16, Upper(r.owner)), 186), kSoft,
         TRF(Str::rooms_panel_players_tables, r.members, keeper::kRoomCap, Count(r.tables, Str::common_table, Str::common_tables).c_str()));
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(918, 224), kGold, TR(Str::rooms_panel_tables));
    const int shown = std::min(r.tables, 5);
    float y = 248;
    for (int t = 0; t < shown; t++) {
        Control(d, 918, y, 1526, y + 52, false);
        Text(d, f.display, g_v.L(17), g_v.P(935, y + 16), kCream, TRF(Str::rooms_panel_table, t + 1));
        const std::string text = detail && t < static_cast<int>(detail->tables.size()) ? detail->tables[t] : t == 0 ? r.rules
                                                                                                                    : "";
        Rules rules;
        if (ParseRules(text, &rules)) {
            std::string line = RoomsRulesText(rules);
            while (line.size() > 4 && TextW(f.body, 16, line) > 330) line = line.substr(0, line.size() - 4) + "...";
            Text(d, f.body, g_v.L(16), g_v.P(1035, y + 17), kGold, line);
        }
        Text(d, f.strong, g_v.L(17), g_v.P(1512, y + 16), kGreen, TR(Str::rooms_panel_open_seat), 1);
        y += 58;
    }
    if (r.tables > shown) {
        Text(d, f.body, g_v.L(16), g_v.P(918, y), kMuted, Count(r.tables - shown, Str::rooms_panel_more_table, Str::rooms_panel_more_tables));
        y += 26;
    }
    Text(d, f.bold ? f.bold : f.strong, g_v.L(16), g_v.P(918, y + 10), kGold, TRF(Str::rooms_panel_players, r.members, keeper::kRoomCap));
    if (detail) {
        ImFont* bold = f.bold ? f.bold : f.strong;
        const int count = static_cast<int>(detail->people.size());
        const float top = y + 36;
        const int rows = std::max(1, static_cast<int>((812 - top) / 48));
        const int shownPeople = count > rows * 2 ? rows * 2 - 1 : count;
        for (int i = 0; i < shownPeople; i++) {
            const float px = 918 + (i % 2) * 308.0f, py = top + (i / 2) * 48.0f;
            Control(d, px, py, px + 300, py + 40, false);
            RoomsScreenDrawFace(d, detail->people[i].chara, px + 6, py + 4, 26, 32);
            float nx = px + 44;
            if (const float fw = RoomsScreenDrawFlag(d, f.strong, detail->people[i].country, nx, py + 14, 13)) nx += fw + 8;
            std::string name = Upper(detail->people[i].name);
            while (name.size() > 1 && TextW(bold, 17, name) > px + 288 - nx) name.pop_back();
            Text(d, bold, g_v.L(17), g_v.P(nx, py + 11), kCream, name);
        }
        if (shownPeople < count) {
            const float px = 918 + (shownPeople % 2) * 308.0f, py = top + (shownPeople / 2) * 48.0f;
            Text(d, f.body, g_v.L(17), g_v.P(px + 14, py + 11), kMuted, TRF(Str::rooms_panel_more_people, count - shownPeople));
        }
        return;
    }
    float fx = 918;
    for (const std::string& face : r.faces) {
        RoomsScreenDrawFace(d, face, fx, y + 36, 22, 30);
        fx += 28;
    }
}

void CreatePanel(ImDrawList* d, const HubFonts& f, bool active, int* clickedRow, int* clickedStep) {
    Kicker(d, f, TR(Str::rooms_create_kicker));
    Text(d, f.display, g_v.L(28), g_v.P(918, 138), kCream, TR(Str::rooms_create_title));
    const std::vector<FormRow> rows = FormRows();
    auto label = [](FormRow row) {
        return TR(row == kRowTitle        ? Str::rooms_create_row_title
                  : row == kRowChara1     ? Str::rooms_create_row_chara1
                  : row == kRowChara2     ? Str::rooms_create_row_chara2
                  : row == kRowVisibility ? Str::rooms_create_row_visibility
                  : row == kRowTables     ? Str::rooms_create_row_tables
                                          : Str::rooms_create_row_rules);
    };
    auto value = [](FormRow row) -> std::string {
        if (row == kRowTitle) return g.title == 0 ? std::string(TR(Str::rooms_create_no_title)) : RoomsScreenTitleText(g.settings.title, "");
        if (row == kRowChara1 || row == kRowChara2) return keeper::CharacterName(keeper::CharacterCode(g.chara[row == kRowChara1 ? 0 : 1]));
        if (row == kRowVisibility) return TR(g.settings.isPublic ? Str::rooms_create_public : Str::rooms_create_private);
        if (row == kRowTables) return Count(g.settings.tables, Str::common_table, Str::common_tables);
        return RulesSummary();
    };
    *clickedRow = *clickedStep = -1;
    const int n = static_cast<int>(rows.size());
    for (int i = 0; i < n; i++) {
        const float y = 196 + i * 60.0f;
        const bool on = g.formRow == i;
        const ImVec2 a = g_v.P(918, y), b = g_v.P(1526, y + 54);
        const bool lit = on && active;
        Control(d, 918, y, 1526, y + 54, lit);
        if (on && !active) d->AddRect(a, b, look::kRing, 0, 0, g_v.L(2));
        ImFont* bold = f.bold ? f.bold : f.strong;
        Text(d, lit ? bold : f.strong, g_v.L(19), g_v.P(940, y + 15), lit ? look::kInk : kCream, label(rows[i]));
        RoomsScreenFitText(d, lit ? bold : f.body, 19, 13, 330, g_v.P(1323, y + 15), lit ? look::kInk : kCream, value(rows[i]), 0.5f);
        if (on && rows[i] != kRowRules) {
            d->AddTriangleFilled(g_v.P(1146, y + 21), g_v.P(1146, y + 33), g_v.P(1136, y + 27), lit ? look::kInk : kGold);
            d->AddTriangleFilled(g_v.P(1500, y + 21), g_v.P(1510, y + 27), g_v.P(1500, y + 33), lit ? look::kInk : kGold);
        }
        if (Clicked() && MouseIn(a, b)) {
            *clickedRow = i;
            if (MouseIn(g_v.P(1120, y), g_v.P(1170, y + 54))) *clickedStep = 0;
            else if (MouseIn(g_v.P(1480, y), g_v.P(1526, y + 54))) *clickedStep = 1;
        }
    }
    const char* visibility = TR(g.settings.isPublic ? Str::rooms_create_public_text : Str::rooms_create_private_text);
    const float below = 196 + n * 60.0f + 14;
    Text(d, f.body, g_v.L(17), g_v.P(918, below), kSoft, visibility);
    Text(d, f.body, g_v.L(17), g_v.P(918, below + 35), kMuted, TRF(Str::rooms_create_up_to, keeper::kRoomCap));
}

void CodePanel(ImDrawList* d, const HubFonts& f, bool active, RoomsView::Notice kind, const std::string& notice, int* clickedKey) {
    Kicker(d, f, TR(Str::rooms_code_kicker));
    Text(d, f.display, g_v.L(28), g_v.P(918, 138), kCream, TR(Str::rooms_code_title));
    const float slotW = (608 - 50) / 6.0f;
    for (int i = 0; i < 6; i++) {
        const float x = 918 + i * (slotW + 10);
        d->AddRectFilled(g_v.P(x, 200), g_v.P(x + slotW, 296), Rgba(0, 0, 0, 90));
        d->AddRect(g_v.P(x, 200), g_v.P(x + slotW, 296), look::kEdgeControl, 0, 0, std::max(1.0f, g_v.L(1)));
        const bool next = active && static_cast<int>(g.code.size()) == i;
        d->AddRectFilled(g_v.P(x, 292), g_v.P(x + slotW, 296), next ? kGold : look::kEdgeControl);
        if (i < static_cast<int>(g.code.size())) {
            if (RoomsScreenStreamerMode()) d->AddCircleFilled(g_v.P(x + slotW / 2, 246), g_v.L(9), Rgba(255, 255, 255), 20);
            else Text(d, f.display, g_v.L(52), g_v.P(x + slotW / 2, 214), Rgba(255, 255, 255), std::string(1, g.code[i]), 0.5f);
        }
    }
    *clickedKey = -1;
    if (kind != RoomsView::Notice::None) {
        const bool full = kind == RoomsView::Notice::Full;
        const bool version = kind == RoomsView::Notice::OtherVersion;
        const ImU32 line = full ? kGold : kRed;
        const ImVec2 a = g_v.P(918, 324), b = g_v.P(1526, 470);
        d->AddRectFilled(a, b, look::kControl);
        d->AddRect(a, b, line, 0, 0, g_v.L(2));
        Text(d, f.strong, g_v.L(16), g_v.P(944, 346), line, TR(full ? Str::rooms_code_full_lead : version ? Str::rooms_code_version_lead
                                                                                                          : Str::rooms_code_none_lead));
        const std::string code = g.lastCode.empty() ? g.code : g.lastCode;
        const bool hide = RoomsScreenStreamerMode() || code.empty();
        Text(d, f.display, g_v.L(26), g_v.P(944, 374), kCream,
             full      ? (hide ? TRF(Str::rooms_code_full_that, keeper::kRoomCap) : TRF(Str::rooms_code_full_code, code.c_str(), keeper::kRoomCap))
             : version ? (hide ? std::string(TR(Str::rooms_code_version_that)) : TRF(Str::rooms_code_version_code, code.c_str()))
                       : (hide ? std::string(TR(Str::rooms_code_none_that)) : TRF(Str::rooms_code_none_code, code.c_str())));
        Text(d, f.body, g_v.L(17), g_v.P(944, 418), kSoft,
             full      ? TRF(Str::rooms_code_full_text, keeper::kRoomCap)
             : version ? notice
                       : std::string(TR(Str::rooms_code_none_text)));
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

void WaitPanel(ImDrawList* d, const HubFonts& f, const char* kicker, const std::string& title, const std::string& text) {
    Kicker(d, f, kicker);
    Text(d, f.display, g_v.L(28), g_v.P(918, 138), kCream, title);
    Text(d, f.body, g_v.L(17), g_v.P(918, 190), kSoft, text);
    const float t = static_cast<float>(ImGui::GetTime()) * 5.0f;
    d->PathArcTo(g_v.P(1222, 330), g_v.L(26), t, t + 4.2f, 32);
    d->PathStroke(kGold, 0, g_v.L(4));
}

} // namespace

void RoomsScreenDrawFace(ImDrawList* d, const std::string& chara, float x, float y, float w, float h) {
    constexpr float kLean = 27.1f / 140.0f, kStripW = 62, kStripX = 6.45f, kStripY = 2, kTexW = 116, kTexH = 144;
    constexpr float kCropW = 46, kCropY = 4;
    const float s = h * kLean, top = w - s;
    const ImVec2 q[4] = {g_v.P(x, y), g_v.P(x + top, y), g_v.P(x + w, y + h), g_v.P(x + s, y + h)};
    d->AddQuadFilled(q[0], q[1], q[2], q[3], Rgba(111, 108, 100));
    const float in = 1, inLean = in * kLean;
    const ImVec2 r[4] = {g_v.P(x + in + inLean, y + in), g_v.P(x + top - in + inLean, y + in), g_v.P(x + w - in - inLean, y + h - in),
                         g_v.P(x + s + in - inLean, y + h - in)};
    d->AddQuadFilled(r[0], r[1], r[2], r[3], Rgba(20, 22, 26));
    if (const TextureView* t = chara.empty() ? nullptr : Texture(CharaTex(chara, "CharaSelectIconLeft"))) {
        const float cropH = std::min(140.0f - kCropY, kCropW * (h - 2 * in) / (top - 2 * in));
        const float x0 = kStripX + (kStripW - kCropW) / 2 + kLean * (kCropY - kStripY), y0 = kCropY;
        auto uv = [&](float tx, float ty) { return ImVec2(tx / kTexW, ty / kTexH); };
        d->AddImageQuad(Id(t), r[0], r[1], r[2], r[3], uv(x0, y0), uv(x0 + kCropW, y0), uv(x0 + kCropW + kLean * cropH, y0 + cropH),
                        uv(x0 + kLean * cropH, y0 + cropH), FadeIn(t));
    }
}

std::string RoomsScreenTitleText(const keeper::RoomTitle& title, const std::string& owner) {
    if (title.id == 0 || !keeper::ValidTitle(title)) return owner.empty() ? std::string(TR(Str::rooms_title_room)) : TRF(Str::rooms_title_owners, owner.c_str());
    std::string s = FlowLoungeCommentText(title.id);
    if (s.empty()) s = keeper::TitleTemplate(title.id);
    for (int i = 0; i < 2; i++) {
        const std::string key = "{common" + std::to_string(i + 1) + "}";
        const size_t at = s.find(key);
        if (at != std::string::npos) s.replace(at, key.size(), keeper::CharacterName(title.chara[i]));
    }
    return s;
}

float RoomsScreenDrawFlag(ImDrawList* d, ImFont* tagFont, const std::string& country, float x, float y, float h, float alpha) {
    if (country.size() != 2) return 0;
    const float w = h * kFlagAspect;
    const ImU32 edge = Rgba(0, 0, 0, static_cast<int>(150 * alpha));
    const std::string_view asset = FlagAsset(country);
    if (asset.empty()) {
        const float size = h * 1.05f, pw = std::max(w, TextW(tagFont, size, country) + h * 0.7f);
        d->AddRectFilled(g_v.P(x, y), g_v.P(x + pw, y + h), Rgba(70, 74, 82, static_cast<int>(230 * alpha)), g_v.L(2));
        Text(d, tagFont, g_v.L(size), g_v.P(x + pw / 2, y - h * 0.08f), Rgba(236, 232, 220, static_cast<int>(255 * alpha)), country, 0.5f);
        return pw;
    }
    d->AddRectFilled(g_v.P(x - 1, y - 1), g_v.P(x + w + 1, y + h + 1), edge, g_v.L(1));
    constexpr float kU0 = 16.0f / 128, kU1 = 112.0f / 128;
    if (const TextureView* t = TextureSized("/Game/CommonAsset/CountryFlags/" + std::string(asset), g_v.L(w) / (kU1 - kU0)))
        Image(d, t, g_v.P(x, y), g_v.P(x + w, y + h), ImVec2(kU0, 0), ImVec2(kU1, 1), Rgba(255, 255, 255, static_cast<int>(255 * alpha)));
    return w;
}

namespace {
std::atomic<bool> g_streamerMode{false};
}

void RoomsScreenSetStreamerMode(bool on) { g_streamerMode = on; }

bool RoomsScreenStreamerMode() { return g_streamerMode; }

float RoomsScreenDrawHiddenCode(ImDrawList* d, float x, float y, float size, ImU32 col) {
    const float r = size * 0.16f, step = size * 0.5f;
    for (int i = 0; d && i < 6; i++) d->AddCircleFilled(g_v.P(x + r + i * step, y + size * 0.62f), g_v.L(r), col, 12);
    return 5 * step + 2 * r;
}

void RoomsScreenDrawCrown(ImDrawList* d, float x, float y, float scale, ImU32 col) {
    static const ImVec2 kPts[] = {{1, 11}, {2, 3}, {6, 7}, {8, 1}, {10, 7}, {14, 3}, {15, 11}};
    ImVec2 pts[7];
    for (int i = 0; i < 7; i++) pts[i] = g_v.P(x + kPts[i].x * scale, y - 12 * scale + kPts[i].y * scale);
    d->AddConcavePolyFilled(pts, 7, col);
}

void RoomsScreenDrawPerson(ImDrawList* d, float x, float y, float size, ImU32 col) {
    const float k = size / 16.0f;
    d->AddCircleFilled(g_v.P(x + 8 * k, y - 8 + 4.5f * k), g_v.L(3.5f * k), col);
    d->PathArcTo(g_v.P(x + 8 * k, y - 8 + 16 * k), g_v.L(7 * k), IM_PI, IM_PI * 2, 16);
    d->PathFillConvex(col);
}

void RoomsScreenShow() {
    if (!g_open.exchange(true)) {
        g.inPanel = false;
        Log("rooms screen: open");
    }
}

void RoomsScreenHide() {
    if (g_open.exchange(false)) Log("rooms screen: closed");
}

bool RoomsScreenOpen() { return g_open; }

void RoomsScreenDraw(const HubFonts& f, const RoomsView& v, bool takeInput, RoomsScreenInput* in) {
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
    using N = RoomsView::Notice;
    const bool codeNotice = v.noticeKind == N::NoRoom || v.noticeKind == N::Full || v.noticeKind == N::OtherVersion;
    const bool failed = codeNotice && g.focus == kCode;

    int barClick = -1;
    if (g.rulesOpen && (busy || g.focus != kCreate)) g.rulesOpen = false;
    if (g.rulesOpen) {
        RoomStepTableRules(g.rules);
    } else if (!busy) {
        if (!g.inPanel) {
            if (Up()) g.focus = g.focus == kCode ? kCreate : g.focus == 0 ? kCode
                                                                          : std::max<int>(kCreate, g.focus - 1);
            if (Down()) g.focus = g.focus == kCreate ? kCode : g.focus == kCode ? (count ? 0 : kCode)
                                                                                : std::min(count - 1, g.focus + 1);
            if (g.focus == kCreate && (Left() || Right())) {
                const int dir = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)
                                    ? -1
                                    : 1;
                StepRow(FormRows()[std::min<size_t>(g.formRow, FormRows().size() - 1)], dir);
            }
            if (g.focus == kCode && Right() && !failed) g.inPanel = true;
        } else if (g.focus == kCreate) {
            if (Up()) g.formRow = std::max(0, g.formRow - 1);
            if (Down()) g.formRow = std::min(static_cast<int>(FormRows().size()) - 1, g.formRow + 1);
            const bool left = Left(), right = !left && Right();
            if (left || right) StepRow(FormRows()[g.formRow], left ? -1 : 1);
        } else if (g.focus == kCode && !failed) {
            if (Left()) g.key = g.key % kKeyCols == 0 ? g.key : g.key - 1;
            if (Right()) g.key = std::min(kKeys - 1, g.key % kKeyCols == kKeyCols - 1 ? g.key : g.key + 1);
            if (Up()) g.key = g.key >= kKeyCols ? g.key - kKeyCols : g.key;
            if (Down()) g.key = std::min(kKeys - 1, g.key + kKeyCols);
        }
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
        int focus = g.focus;
        WheelList(g_v.P(48, kListTop), g_v.P(868, 832), count, kRowsInView, &g.scroll, &focus);
        if (focus != g.focus) g.focus = focus, g.inPanel = false;
    }
    if (g.focus >= 0) {
        if (g.focus < g.scroll) g.scroll = static_cast<float>(g.focus);
        if (g.focus > g.scroll + kRowsInView - 1) g.scroll = static_cast<float>(g.focus - kRowsInView + 1);
    }
    g.scroll = std::clamp(g.scroll, 0.0f, static_cast<float>(std::max(0, count - kRowsInView)));

    StageText(d, f.display, 40, 48, 22, Rgba(255, 255, 255), TR(Str::rooms_screen_title));
    const std::string summary =
        v.keeper ? TRF(Str::rooms_screen_summary, Count(v.summary.publicRooms, Str::rooms_screen_public_room, Str::rooms_screen_public_rooms).c_str(),
                       Count(v.summary.players, Str::common_player_count, Str::common_players_count).c_str())
                 : std::string(TR(v.state == S::Failed ? Str::rooms_screen_cant_reach : Str::rooms_screen_unavailable));
    StageText(d, f.body, 20, 48 + TextW(f.display, 40, TR(Str::rooms_screen_title)) + 28, 39, kCream, summary);
    ActionRow(d, f, 80, "+", TR(Str::rooms_screen_create), TR(Str::rooms_screen_create_info), g.focus == kCreate);
    ActionRow(d, f, 160, "#", TR(Str::rooms_screen_join), TR(Str::rooms_screen_join_info), g.focus == kCode);
    StageText(d, f.bold ? f.bold : f.strong, 16, 48, 246, kGold, TRF(Str::rooms_screen_public_list, count));
    d->PushClipRect(g_v.P(30, kListTop - 2), g_v.P(886, 840), true);
    if (!count) {
        Pane(d, 48, kListTop, 868, kListTop + 160);
        Text(d, f.strong, g_v.L(20), g_v.P(72, kListTop + 52), kCream, TR(Str::rooms_screen_none));
        Text(d, f.body, g_v.L(16), g_v.P(72, kListTop + 84), kMuted,
             TR(Str::rooms_screen_none_text));
    }
    for (int i = 0; i < count; i++) {
        const float y = kListTop + (i - g.scroll) * kRowPitch;
        if (y > 832 || y + kRowH < kListTop) continue;
        RoomRow(d, f, y, rooms[i], g.focus == i);
    }
    d->PopClipRect();

    ScrollBar(d, 874, kListTop, 832, g.scroll, kRowsInView, static_cast<float>(count), true);

    PanelFrame(d);
    int formRow = -1, formStep = -1, key = -1;
    if (v.state == S::Creating) {
        WaitPanel(d, f, TR(Str::rooms_wait_new), TR(Str::rooms_wait_creating), TR(Str::rooms_wait_creating_text));
    } else if (v.state == S::Joining) {
        WaitPanel(d, f, TR(Str::rooms_wait_joining_kicker), TR(Str::rooms_wait_joining), TR(Str::rooms_wait_one_moment));
    } else if (v.state == S::Room) {
        WaitPanel(d, f, TR(Str::rooms_wait_in_room),
                  RoomsScreenStreamerMode() ? std::string(TR(Str::rooms_wait_your_room)) : TRF(Str::rooms_wait_room_code, WeyveService::DisplayCode(v.room).c_str()),
                  TR(Str::rooms_wait_opening));
    } else if (g.focus == kCreate) {
        CreatePanel(d, f, g.inPanel, &formRow, &formStep);
    } else if (g.focus == kCode) {
        CodePanel(d, f, g.inPanel, failed ? v.noticeKind : N::None, failed ? v.notice : "", &key);
    } else if (g.focus >= 0 && g.focus < count) {
        RoomPanel(d, f, rooms[g.focus], v.peek.code == rooms[g.focus].code ? &v.peek : nullptr);
    }
    if (formRow >= 0 && !g.rulesOpen) {
        g.focus = kCreate;
        g.inPanel = true;
        g.formRow = formRow;
        if (formStep >= 0) {
            StepRow(FormRows()[g.formRow], formStep == 0 ? -1 : 1);
        } else if (FormRows()[g.formRow] == kRowRules) OpenRulesEditor();
    }

    std::string lead, text;
    ImU32 leadCol = kGold;
    std::vector<std::pair<const char*, std::string>> prompts;
    const bool roomFull = g.focus >= 0 && g.focus < count && rooms[g.focus].members >= keeper::kRoomCap;
    if (busy || v.state == S::Room) {
        text = TR(v.state == S::Creating ? Str::rooms_bar_creating : Str::rooms_bar_joining);
    } else if (!v.otherBuild.empty() && !v.keeper) {
        lead = TR(Str::rooms_bar_update_lead);
        leadCol = kRed;
        text = TR(Str::rooms_bar_update);
        prompts = {{"button_start", TR(Str::common_prompt_settings)}, {"button_b", TR(Str::common_prompt_back)}};
    } else if (!v.keeper && (g.focus == kCreate || g.focus >= 0)) {
        lead = TR(v.state == S::Failed ? Str::rooms_bar_cannot_connect : Str::rooms_bar_offline);
        leadCol = v.state == S::Failed ? kRed : kMuted;
        text = TR(v.state == S::Failed ? Str::rooms_bar_cant_reach : Str::rooms_bar_unavailable);
        prompts = {};
        if (v.state == S::Failed) prompts.push_back({"button_start", TR(Str::common_prompt_settings)});
        prompts.push_back({"button_b", TR(Str::common_prompt_back)});
    } else if (g.focus == kCreate) {
        if (v.noticeKind != N::None && !codeNotice) {
            lead = TR(v.noticeKind == N::Removed ? Str::rooms_bar_removed : v.noticeKind == N::CannotJoin ? Str::rooms_bar_not_joined
                                                                                                          : Str::rooms_bar_not_made);
            leadCol = kRed;
            text = v.notice;
        } else if (!count && g.formRow == 0) {
            lead = TR(Str::rooms_bar_no_public);
            leadCol = kMuted;
            text = TR(Str::rooms_bar_no_public_text);
        } else {
            const FormRow row = FormRows()[std::min<size_t>(g.formRow, FormRows().size() - 1)];
            text = !g.inPanel              ? ""
                   : row == kRowTitle      ? TR(Str::rooms_bar_title_help)
                   : row == kRowChara1     ? TR(Str::rooms_bar_chara1_help)
                   : row == kRowChara2     ? TR(Str::rooms_bar_chara2_help)
                   : row == kRowVisibility ? ""
                   : row == kRowTables     ? TR(Str::rooms_bar_tables_help)
                                           : TR(Str::rooms_bar_rules_help);
        }
        prompts = {{"button_start", TR(Str::common_prompt_create)}, {"button_b", TR(Str::common_prompt_back)}};
        if (!g.inPanel) prompts.insert(prompts.begin(), {"button_a", TR(Str::common_prompt_change)});
        else if (FormRows()[std::min<size_t>(g.formRow, FormRows().size() - 1)] == kRowRules) prompts.insert(prompts.begin(), {"button_a", TR(Str::rooms_bar_customize)});
    } else if (g.focus == kCode) {
        if (failed) prompts = {{"button_a", TR(Str::common_prompt_try_again)}, {"button_b", TR(Str::common_prompt_back)}};
        else {
            text = TR(Str::rooms_bar_enter_code);
            prompts = {{"button_a", TR(Str::common_prompt_type)}, {"button_x", TR(Str::common_prompt_delete)}, {"button_y", TR(Str::common_prompt_paste)}, {"button_start", TR(Str::common_prompt_join)}, {"button_b", TR(Str::common_prompt_back)}};
        }
    } else if (roomFull) {
        lead = TR(Str::rooms_bar_full);
        leadCol = kRed;
        text = TR(Str::rooms_bar_full_text);
        prompts = {{"button_b", TR(Str::common_prompt_back)}};
    } else if (g.focus >= 0) {
        text = TR(rooms[g.focus].openSeats ? Str::rooms_bar_open_seats : Str::rooms_bar_no_seats);
        prompts = {{"button_a", TR(Str::common_prompt_join)}, {"button_b", TR(Str::common_prompt_back)}};
    }
    if (g.rulesOpen) lead.clear(), text.clear(), prompts.clear();
    PromptLine(d, f.body, f.bold ? f.bold : f.strong, lead, leadCol, text, prompts, &barClick);

    if (g.rulesOpen) {
        int clicked = -1;
        const PromptList modalPrompts = {{"button_a", TR(Str::common_prompt_done)}, {"button_b", TR(Str::common_prompt_cancel)}};
        RoomDrawTableRules(f, g.rules, TR(Str::rooms_create_kicker), TR(Str::rooms_rules_modal_text), kSoft, modalPrompts, &clicked);
        if (Confirm() || Pressed({ImGuiKey_GamepadStart}, false)) g.rulesOpen = false;
        else if (Back()) {
            g.rules.rules = g.rulesBefore;
            g.rulesOpen = false;
        }
    } else if (!busy && v.state != S::Room) {
        const bool typingCode = g.focus == kCode && g.inPanel && !failed;
        const bool a = Confirm(), b = Back() || (typingCode && Pressed({ImGuiKey_Escape}, false));
        const bool start = Pressed({ImGuiKey_GamepadStart}, false) ||
                           (g.inPanel && Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter}, false) && g.focus == kCode);
        const bool x = Pressed({ImGuiKey_GamepadFaceLeft}, false);
        const bool y = Pressed({ImGuiKey_GamepadFaceUp}, false);
        if (!v.keeper && start && (v.state == S::Failed || !v.otherBuild.empty())) {
            in->settings_ = true;
        } else if (g.focus == kCreate) {
            if (start && v.keeper) {
                in->create = true;
                in->settings = g.settings;
                SizeRules();
                for (const Rules& r : g.rules.rules) in->tableRules.push_back(FormatRules(r));
            } else if (a && !g.inPanel) g.inPanel = true;
            else if (a && FormRows()[std::min<size_t>(g.formRow, FormRows().size() - 1)] == kRowRules) OpenRulesEditor();
            else if (b) {
                if (g.inPanel) g.inPanel = false;
                else in->back = true;
            }
        } else if (g.focus == kCode) {
            if (failed) {
                if (a) {
                    in->clearNotice = true;
                    g.code = g.lastCode;
                    g.inPanel = true;
                } else if (b) {
                    in->clearNotice = true;
                    g.inPanel = false;
                }
            } else {
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
        } else if (g.focus >= 0 && g.focus < count) {
            if (a && !roomFull) in->join = rooms[g.focus].code;
            if (b) in->back = true;
        } else if (b) {
            in->back = true;
        }
    }
    if (g.focus >= 0 && g.focus < count && v.state == S::Lobby) in->focusedRoom = rooms[g.focus].code;
    g_inputOn = false;
    d->PopClipRect();
}

} // namespace sfv::mod
