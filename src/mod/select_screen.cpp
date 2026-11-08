#include "mod/select_screen.h"

#include "core/random_stage.h"

#include "mod/flow.h"
#include "mod/log.h"
#include "mod/pak_textures.h"
#include "mod/textures.h"
#include "mod/ui_kit.h"

#include <unknwn.h>

#include <imgui.h>
#include <imgui_internal.h> // IM_PI, the draw list's white pixel

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <utility>

namespace sfv::mod {
namespace {

using namespace ui;

void DrawPrompts(ImDrawList* d, const SelectFonts& f, const std::vector<Prompt>& prompts) { ui::DrawPrompts(d, f.text, prompts); }


// ----- data and the screen's own state ----------------------------------------

std::mutex g_dataMutex;
SelectData g_data; // the live screen's lists (SetSelectData)
bool g_myCharacter = false; // Render's live.myCharacter (DrawLoadout's banner)

const char* const kRowLabels[kSelectRows] = {"Costume", "Color", "Variant", "V-Skill", "V-Trigger", "V-Comment", "Input delay"};

// What the player is doing on the screen (a session only knows their pick
// and whether they are ready).
struct Ui {
    bool loadout = false;  // past the grid
    int row = 0;           // the loadout row
    int cursorRow = 0, cursorX = 0;
    bool onRandom = false; // the cursor on the random tile
    int lastChara = -2;    // the pick the cursor last followed (it starts there, and moves when the pick changes)
    std::string lastStage; // the stage behind the screen (the last real one while random is chosen)
    bool wasLocked = false;
    bool leaving = false;     // the leave dialog is up
    bool leaveChosen = false; // ... on Leave (it opens on Stay)
    bool cameFromStage = false; // My character: back from its stage select (the grid or loadout as left)
    // My character's Random stages list (X on the random tile).
    bool randomList = false;
    int listCursor = 0;       // its tile
    int variantsOf = -1;      // the tile whose settings list is up; -1 none
    int variantCursor = 0;    // ... its row
    std::string listNote;     // a note in the footer for a while ("Keep at least one stage on")
    double listNoteUntil = 0;
} g_ui;

// The game's grid, from the middle outwards (the final roster's screen):
// top row 11 a side, bottom row 12 a side.
const char* const kTopLeft[] = {"RYU", "CNL", "RMK", "RSD", "NSH", "Z21", "Z23", "Z28", "Z32", "Z36", "Z39"};
const char* const kTopRight[] = {"KEN", "CMY", "BLR", "NCL", "GUL", "Z20", "Z25", "Z29", "Z33", "Z37", "Z41"};
const char* const kBottomLeft[] = {"ALX", "KRN", "ZGF", "LAR", "IBK", "JRI", "Z22", "Z26", "Z30", "Z34", "Z38", "Z42"};
const char* const kBottomRight[] = {"VEG", "BRD", "DSM", "FAN", "BSN", "URN", "Z24", "Z27", "Z31", "Z35", "Z40", "ELV"};


int CharaAt(const SelectData& data, const std::string& code)
{
    for (size_t i = 0; i < data.charas.size(); i++) {
        if (data.charas[i].code == code) return static_cast<int>(i);
    }
    return -1;
}

// A grid slot: its character (-1 the random tile), position and lean.
struct Slot {
    int chara = -1;
    bool random = false;
    bool right = false; // leans the other way (the right half)
    ImVec2 q[4];        // corners in design coordinates: top left, top right, bottom right, bottom left
};
std::vector<Slot> g_rows[2]; // left to right
const SelectData* g_gridFor = nullptr;

// The tiles lean as the portraits do: CharaSelectIconLeft/Right (116x144) hold
// a parallelogram 62 px wide over rows 2..142 leaning 12 px every 62 rows.
constexpr float kArtSlope = 12.0f / 62.0f;
constexpr float kC = 800, kY0 = 606, kTH = 112, kY1 = 726, kSlant = kTH * kArtSlope, kTopW = 52, kPitch = 60, kHalf = 58;
constexpr float kK = kSlant / kTH;
// The portrait's corners in the texture (pixels): top left, top right, bottom
// right, bottom left; the right-hand version leans the other way.
constexpr ImVec2 kArtLeft[4] = {{6.45f, 2}, {68.45f, 2}, {95.55f, 142}, {33.55f, 142}};
constexpr ImVec2 kArtRight[4] = {{47.55f, 2}, {109.55f, 2}, {82.45f, 142}, {20.45f, 142}};

void BuildGrid(const SelectData& data)
{
    g_gridFor = &data;
    auto quad = [](Slot& t, float x, float y, bool right) {
        t.right = right;
        if (right) {
            t.q[0] = ImVec2(x + kSlant, y);
            t.q[1] = ImVec2(x + kSlant + kTopW, y);
            t.q[2] = ImVec2(x + kTopW, y + kTH);
            t.q[3] = ImVec2(x, y + kTH);
        }
        else {
            t.q[0] = ImVec2(x, y);
            t.q[1] = ImVec2(x + kTopW, y);
            t.q[2] = ImVec2(x + kTopW + kSlant, y + kTH);
            t.q[3] = ImVec2(x + kSlant, y + kTH);
        }
    };
    auto half = [&](std::vector<Slot>& row, const char* const* codes, size_t n, int r, bool right) {
        const float y = r ? kY1 : kY0;
        std::vector<Slot> slots;
        for (size_t j = 0; j < n; j++) {
            Slot t;
            t.chara = CharaAt(data, codes[j]);
            const float x = right ? kC + kHalf - kK * (y - kY0) + j * kPitch - kSlant : kC - kHalf + kK * (y - kY0) - j * kPitch - kTopW;
            quad(t, x, y, right);
            slots.push_back(t);
        }
        if (!right) std::reverse(slots.begin(), slots.end());
        row.insert(row.end(), slots.begin(), slots.end());
    };
    for (auto& row : g_rows) row.clear();
    half(g_rows[0], kTopLeft, std::size(kTopLeft), 0, false);
    Slot random;
    random.random = true;
    const float ry = kY0 + 40, rb = kY1 + kTH - 6;
    const float xt = kC - kHalf + 5 + kK * (ry - kY0), xb = kC - kHalf + 5 + kK * (rb - kY0);
    random.q[0] = ImVec2(xt, ry);
    random.q[1] = ImVec2(2 * kC - xt, ry);
    random.q[2] = ImVec2(2 * kC - xb, rb);
    random.q[3] = ImVec2(xb, rb);
    g_rows[0].push_back(random);
    half(g_rows[0], kTopRight, std::size(kTopRight), 0, true);
    // The random tile spans both rows: left and right reach it from either.
    half(g_rows[1], kBottomLeft, std::size(kBottomLeft), 1, false);
    g_rows[1].push_back(random);
    half(g_rows[1], kBottomRight, std::size(kBottomRight), 1, true);
}

bool CursorTo(int chara)
{
    for (int r = 0; r < 2; r++) {
        for (int x = 0; x < static_cast<int>(g_rows[r].size()); x++) {
            if (g_rows[r][x].chara == chara && chara >= 0) {
                g_ui.cursorRow = r;
                g_ui.cursorX = x;
                return true;
            }
        }
    }
    return false;
}



// ----- the parts ----------------------------------------------------------------

const SelectData* g_draw = nullptr; // the data of the screen being drawn

const SelectChara* Chara(int i)
{
    if (!g_draw || i < 0 || i >= static_cast<int>(g_draw->charas.size())) return nullptr;
    return &g_draw->charas[i];
}

// Stage `stage` in setting `setting` (0: the stage itself); null for random.
const SelectStage* Shown(int stage, int setting)
{
    if (!g_draw || stage < 0 || stage >= static_cast<int>(g_draw->stages.size())) return nullptr;
    const SelectStage& s = g_draw->stages[stage];
    return setting > 0 && setting <= static_cast<int>(s.settings.size()) ? &s.settings[setting - 1] : &s;
}

// The stage's picture over the whole screen (cropped at the sides: it is 4:1),
// blurred and darkened, the grain over it; the screen draws over it. A
// stage just moved to shows once its blurred copy is made (a frame or
// two): the last one stays till then.
void DrawBackdrop(ImDrawList* d, int stage, int setting, ImU32 tint)
{
    if (const SelectStage* s = Shown(stage, setting)) g_ui.lastStage = s->code;
    if (g_ui.lastStage.empty() && !g_draw->stages.empty()) g_ui.lastStage = g_draw->stages[0].code;
    if (g_ui.lastStage.empty()) return;
    // shown: the newest drawn; under: the one before (drawn under its fade-in).
    static std::string shown, under;
    auto get = [](const std::string& path) {
        bool failed = false;
        const TextureView* t = BlurredStage(path, 6, 1.1f, &failed);
        return failed ? Texture(path) : t;
    };
    const std::string path = StageTex(g_ui.lastStage, "StageSelectBG");
    if (path != shown && get(path)) {
        under = shown;
        shown = path;
    }
    auto draw = [&](const TextureView* bg) {
        const float aspect = bg->height ? static_cast<float>(bg->width) / bg->height : 4.0f;
        const float visible = (1600.0f / 900.0f) / aspect; // the share of the width that fits
        Image(d, bg, g_v.P(0, 0), g_v.P(1600, 900), ImVec2(0.5f - visible / 2, 0), ImVec2(0.5f + visible / 2, 1), tint);
    };
    const TextureView* top = shown.empty() ? nullptr : get(shown);
    if (!top) return;
    if (FadeIn(top) != IM_COL32_WHITE && !under.empty()) {
        if (const TextureView* below = get(under)) {
            const float aspect = below->height ? static_cast<float>(below->width) / below->height : 4.0f;
            const float visible = (1600.0f / 900.0f) / aspect;
            d->AddImage(ui::Id(below), g_v.P(0, 0), g_v.P(1600, 900), ImVec2(0.5f - visible / 2, 0), ImVec2(0.5f + visible / 2, 1), tint);
        }
    }
    draw(top);
    Grain(d);
}

// `text` cut to `width` (ui units) at this size, with an ellipsis.
std::string FitText(ImFont* font, float size, std::string text, float width)
{
    if (TextW(font, size, text) <= width) return text;
    while (!text.empty() && TextW(font, size, text + "...") > width) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
    }
    return text + "...";
}

// `highlight`: the row lit up (this player's, or the row the other is on), -1 none.
void DrawLoadout(ImDrawList* d, const SelectFonts& f, bool right, const SelectSide& side, int highlight, bool interactive,
                 SelectInput* in)
{
    const float x0 = right ? 980.0f : 0.0f, top = 108, rowH = 42, gap = 4, w = 620;
    // The medium weight (rows over the character's art read faintly in the
    // light one, on a Steam Deck especially), bold on the focused row.
    ImFont* font = f.title ? f.title : f.text;
    ImFont* bold = f.bold ? f.bold : font;
    for (int r = 0; r < kSelectRows; r++) {
        const SelectRow& row = side.rows[r];
        const float y = top + r * (rowH + gap);
        const ImVec2 a = g_v.P(x0, y), b = g_v.P(x0 + w, y + rowH);
        // This player's focused row: the gold fill SFVR's other screens use
        // for focus; the other side's row: a gold edge on its dark row.
        const bool lit = r == highlight && interactive;
        if (lit) d->AddRectFilled(a, b, look::kGold);
        else {
            d->AddRectFilled(a, b, Rgba(8, 9, 11, 228));
            if (r == highlight) {
                const float ex = right ? x0 + w - 4 : x0;
                d->AddRectFilled(g_v.P(ex, y), g_v.P(ex + 4, y + rowH), look::kGold);
            }
            d->AddLine(ImVec2(a.x, b.y - 0.5f), ImVec2(b.x, b.y - 0.5f), look::kHairline, std::max(1.0f, g_v.L(1)));
        }
        const ImU32 ink = lit ? look::kInk : (row.fixed && interactive ? Rgba(244, 241, 232, 150) : look::kCream);
        ImFont* rf = lit ? bold : font;
        Text(d, rf, g_v.L(20), g_v.P(x0 + 44, y + 9), ink, kRowLabels[r]);
        const float mid = x0 + 190 + (w - 190) / 2;
        const float tw = TextW(rf, 20, row.value);
        const float sw = row.swatch1 ? 48.0f : 0.0f;
        const float tx = mid - (tw + sw) / 2;
        Text(d, rf, g_v.L(20), g_v.P(tx, y + 9), ink, row.value);
        if (row.swatch1) {
            const float sx = tx + tw + 10;
            d->AddRectFilled(g_v.P(sx, y + 11), g_v.P(sx + 18, y + 31), FromArgb(row.swatch1));
            d->AddRect(g_v.P(sx, y + 11), g_v.P(sx + 18, y + 31), Rgba(0, 0, 0));
            d->AddRectFilled(g_v.P(sx + 22, y + 11), g_v.P(sx + 40, y + 31), FromArgb(row.swatch2));
            d->AddRect(g_v.P(sx + 22, y + 11), g_v.P(sx + 40, y + 31), Rgba(0, 0, 0));
        }
        if (interactive && !row.fixed) {
            // Drawn arrows (the "<" and ">" characters were hairlines).
            const ImU32 arrow = lit ? look::kInk : look::kGold;
            const float cy = y + rowH / 2, lx = x0 + 212, rx = x0 + w - 24;
            d->AddTriangleFilled(g_v.P(lx + 5, cy - 7), g_v.P(lx + 5, cy + 7), g_v.P(lx - 6, cy), arrow);
            d->AddTriangleFilled(g_v.P(rx - 5, cy - 7), g_v.P(rx + 6, cy), g_v.P(rx - 5, cy + 7), arrow);
            if (Clicked() && MouseIn(g_v.P(x0 + 190, y), g_v.P(x0 + 234, y + rowH))) {
                g_ui.row = r;
                in->row = r;
                in->delta = -1;
            }
            else if (Clicked() && MouseIn(g_v.P(x0 + w - 44, y), g_v.P(x0 + w, y + rowH))) {
                g_ui.row = r;
                in->row = r;
                in->delta = 1;
            }
        }
        if (interactive && Clicked() && MouseIn(a, b) && in->row < 0) g_ui.row = r;
    }
    // The strip under the rows: the chosen row's description (the other side:
    // its V-Comment), gold READY once that side is ready.
    const float y = top + kSelectRows * (rowH + gap);
    // (Clear of the name line under it: its ping pill starts at 474.)
    const ImVec2 a = g_v.P(x0, y), b = g_v.P(x0 + w, y + 32);
    if (side.ready) {
        const ImU32 g1 = Rgba(217, 166, 50), g2 = Rgba(150, 108, 28, 230);
        d->AddRectFilledMultiColor(a, b, right ? g2 : g1, right ? g1 : g2, right ? g1 : g2, right ? g2 : g1);
        Text(d, f.name, g_v.L(20), g_v.P(x0 + 44, y + 4), Rgba(26, 20, 5), g_myCharacter ? "SAVED" : "READY");
    }
    else if (!g_myCharacter) {
        const std::string& hint = side.rows[interactive ? g_ui.row : 5].hint;
        if (!hint.empty()) {
            d->AddRectFilled(a, b, Rgba(8, 9, 11, 228));
            Text(d, f.text, g_v.L(17), g_v.P(x0 + 44, y + 6), look::kCream, FitText(f.text, 17, hint, w - 64));
        }
    }
}

void DrawTile(ImDrawList* d, const Slot& t, bool p1, bool p2, const SelectFonts& f)
{
    ImVec2 q[4];
    for (int i = 0; i < 4; i++) q[i] = g_v.P(t.q[i].x, t.q[i].y);
    const bool on = p1 || p2;
    if (t.random) {
        d->AddQuadFilled(q[0], q[1], q[2], q[3], on ? Rgba(240, 194, 74) : Rgba(157, 153, 143));
        ImVec2 in[4] = {ImVec2(q[0].x + g_v.L(2.5f), q[0].y + g_v.L(2)), ImVec2(q[1].x - g_v.L(2.5f), q[1].y + g_v.L(2)),
                        ImVec2(q[2].x - g_v.L(2), q[2].y - g_v.L(2)), ImVec2(q[3].x + g_v.L(2), q[3].y - g_v.L(2))};
        GradientQuad(d, in, Rgba(44, 44, 44), Rgba(6, 6, 6));
        Text(d, f.title, g_v.L(58), ImVec2((q[0].x + q[1].x) / 2, q[0].y + g_v.L(26)), Rgba(255, 255, 255), "?", 0.5f, true);
        return;
    }
    d->AddQuadFilled(q[0], q[1], q[2], q[3], on ? Rgba(240, 194, 74) : Rgba(157, 153, 143));
    // The picture inside a 2 px frame, cut to the same shape.
    const float k = kK, in = 2;
    ImVec2 iq[4];
    if (t.right) {
        iq[0] = g_v.P(t.q[0].x + in - k * in, t.q[0].y + in);
        iq[1] = g_v.P(t.q[1].x - in - k * in, t.q[1].y + in);
        iq[2] = g_v.P(t.q[2].x - in + k * in, t.q[2].y - in);
        iq[3] = g_v.P(t.q[3].x + in + k * in, t.q[3].y - in);
    }
    else {
        iq[0] = g_v.P(t.q[0].x + in + k * in, t.q[0].y + in);
        iq[1] = g_v.P(t.q[1].x - in + k * in, t.q[1].y + in);
        iq[2] = g_v.P(t.q[2].x - in - k * in, t.q[2].y - in);
        iq[3] = g_v.P(t.q[3].x + in - k * in, t.q[3].y - in);
    }
    d->AddQuadFilled(iq[0], iq[1], iq[2], iq[3], Rgba(20, 22, 26));
    if (const SelectChara* c = Chara(t.chara)) {
        if (const TextureView* tex = Texture(CharaTex(c->code, t.right ? "CharaSelectIconRight" : "CharaSelectIconLeft"))) {
            // The portrait corner to corner: same angle as the frame, no gaps.
            const ImVec2* art = t.right ? kArtRight : kArtLeft;
            auto uv = [&](int i) { return ImVec2(art[i].x / 116.0f, art[i].y / 144.0f); };
            const ImU32 tint = c->locked ? Rgba(70, 70, 70) : on ? IM_COL32_WHITE : Rgba(205, 205, 205);
            d->AddImageQuad(Id(tex), iq[0], iq[1], iq[2], iq[3], uv(0), uv(1), uv(2), uv(3), FadeIn(tex, tint));
        }
    }
    // The bottom fade.
    const float mid = 0.55f;
    ImVec2 lower[4] = {ImVec2(iq[0].x + (iq[3].x - iq[0].x) * mid, iq[0].y + (iq[3].y - iq[0].y) * mid),
                       ImVec2(iq[1].x + (iq[2].x - iq[1].x) * mid, iq[1].y + (iq[2].y - iq[1].y) * mid), iq[2], iq[3]};
    GradientQuad(d, lower, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, on ? 40 : 150));
    if (p1) {
        const ImVec2 at(iq[0].x + g_v.L(t.right ? kSlant - 6 : 2.0f), iq[0].y + g_v.L(2));
        d->AddRectFilled(at, ImVec2(at.x + g_v.L(20), at.y + g_v.L(15)), Rgba(47, 111, 228));
        Text(d, f.text, g_v.L(12), ImVec2(at.x + g_v.L(10), at.y + g_v.L(1)), Rgba(255, 255, 255), "1P", 0.5f);
    }
    if (p2) {
        const ImVec2 at(iq[2].x - g_v.L(t.right ? kSlant - 6 : 2.0f), iq[2].y - g_v.L(2));
        d->AddRectFilled(ImVec2(at.x - g_v.L(20), at.y - g_v.L(15)), at, Rgba(210, 59, 49));
        Text(d, f.text, g_v.L(12), ImVec2(at.x - g_v.L(10), at.y - g_v.L(14)), Rgba(255, 255, 255), "2P", 0.5f);
    }
}

// One side's line over its character name: the player, (you), and a pill
// with their connection type; on the other player's, the link between the
// two (path, ping) too. A spectator knows each player's connection only.
void DrawPlayerLine(ImDrawList* d, const SelectFonts& f, const SelectLive& live, int i)
{
    const float y = 470;
    const ImU32 white = Rgba(244, 241, 232), dim = Rgba(168, 162, 147), icon = Rgba(217, 212, 198);
    const bool left = i == 0;
    const SelectSide& s = live.sides[i];
    if (!s.present) {
        Text(d, f.text, g_v.L(16), g_v.P(left ? 120.0f : 1480.0f, y + 4), dim, "WAITING FOR A PLAYER...", left ? 0.0f : 1.0f);
        return;
    }
    const std::string player = Upper(s.player.empty() ? "Player" : s.player);
    const bool you = live.you == i;
    const bool link = live.you >= 0 && !you;
    char ping[32] = "";
    if (link && live.pingMs) std::snprintf(ping, sizeof(ping), "%u ms", live.pingMs);
    const float pw = (s.link ? 34.0f : 0.0f) + (link ? 34.0f : 0.0f) + (ping[0] ? TextW(f.text, 18, ping) + 18 : 0.0f);
    // Laid out from the screen's edge inwards.
    const float dir = left ? 1.0f : -1.0f;
    float x = left ? 120.0f : 1480.0f;
    Text(d, f.text, g_v.L(16), g_v.P(x, y + 4), white, player, left ? 0.0f : 1.0f);
    x += dir * (TextW(f.text, 16, player) + 10);
    if (you) {
        Text(d, f.text, g_v.L(16), g_v.P(x, y + 4), dim, "(you)", left ? 0.0f : 1.0f);
        x += dir * (TextW(f.text, 16, "(you)") + 10);
    }
    if (pw <= 0) return;
    const float px = left ? x : x - pw;
    Pill(d, g_v.P(px, y), g_v.P(px + pw, y + 26));
    float cx = px;
    if (s.link) {
        IconLinkType(d, g_v.P(cx + 17, y + 13), g_v.s, s.link, icon);
        cx += 34;
    }
    if (link) {
        if (live.relayed) IconCloud(d, g_v.P(cx + 17, y + 13), g_v.s, Rgba(233, 196, 106));
        else IconLink(d, g_v.P(cx + 17, y + 13), g_v.s, icon);
        cx += 34;
    }
    // The ping, colored by the delay it suggests (PingColor), a size up
    // from the pill's 15 (user: hard to see).
    if (ping[0]) Text(d, f.text, g_v.L(18), g_v.P(cx + 4, y + 3), PingColor(live.pingMs), ping);
}

void DrawRender(ImDrawList* d, int chara, bool right)
{
    const SelectChara* c = Chara(chara);
    if (!c) return;
    const TextureView* t = Texture(CharaTex(c->code, "PartCharacter"));
    if (!t) return;
    if (right) Image(d, t, g_v.P(1600 + 60 - 780, -70), g_v.P(1660, 710), ImVec2(1, 0), ImVec2(0, 1));
    else Image(d, t, g_v.P(-60, -70), g_v.P(720, 710));
}

// ----- My character's Random stages ----------------------------------------------

// One tile of the list: a stage of the stage select (never one of the
// never-random stages) with its variants as random draws them, the stage
// itself ("Setting 1") first, then its settings.
struct ListVariant {
    std::string code, name;
    bool locked = false; // not owned: random never draws it
};
struct ListStage {
    std::string name;
    std::vector<ListVariant> variants;
    bool locked = true; // no variant owned
};

std::vector<ListStage> RandomList()
{
    std::vector<ListStage> list;
    if (!g_draw) return list;
    for (const SelectStage& s : g_draw->stages) {
        if (IsNeverRandomStage(s.code)) continue;
        ListStage l;
        l.name = s.name;
        l.variants.push_back({s.code, s.name, s.locked});
        for (const SelectStage& k : s.settings) {
            if (!IsNeverRandomStage(k.code)) l.variants.push_back({k.code, k.name, k.locked});
        }
        for (const ListVariant& v : l.variants) l.locked = l.locked && v.locked;
        list.push_back(std::move(l));
    }
    return list;
}

bool IsOff(const std::vector<std::string>& off, const std::string& code) { return std::find(off.begin(), off.end(), code) != off.end(); }

// A tile's owned variants that are on, and all its owned ones.
std::pair<int, int> VariantsOn(const ListStage& l, const std::vector<std::string>& off)
{
    int on = 0, owned = 0;
    for (const ListVariant& v : l.variants) {
        if (v.locked) continue;
        owned++;
        if (!IsOff(off, v.code)) on++;
    }
    return {on, owned};
}

std::pair<int, int> StagesOn(const std::vector<ListStage>& list, const std::vector<std::string>& off)
{
    int on = 0, owned = 0;
    for (const ListStage& l : list) {
        const auto [a, b] = VariantsOn(l, off);
        on += a;
        owned += b;
    }
    return {on, owned};
}

std::vector<std::string> OwnedCodes(const ListStage& l)
{
    std::vector<std::string> codes;
    for (const ListVariant& v : l.variants) {
        if (!v.locked) codes.push_back(v.code);
    }
    return codes;
}

// `codes` turned on or off in `off`; false, with nothing changed, when no
// stage would be left on.
bool SetVariants(const std::vector<ListStage>& list, std::vector<std::string>* off, const std::vector<std::string>& codes, bool on)
{
    std::vector<std::string> next = *off;
    for (const std::string& c : codes) {
        next.erase(std::remove(next.begin(), next.end(), c), next.end());
        if (!on) next.push_back(c);
    }
    if (StagesOn(list, next).first == 0) return false;
    *off = ParseStageMask(FormatStageMask(next));
    return true;
}

void ListNote(std::string text)
{
    g_ui.listNote = std::move(text);
    g_ui.listNoteUntil = ImGui::GetTime() + 3.5;
}

constexpr const char* kKeepOne = "Keep at least one stage on.";

// A tile's or row's box: gold with a check when all its variants are on,
// gold with a bar when some are, an empty frame when none are.
void CheckBox(ImDrawList* d, float x, float y, int on, int of)
{
    const ImVec2 a = g_v.P(x, y), b = g_v.P(x + 24, y + 24);
    if (on <= 0) {
        d->AddRectFilled(a, b, Rgba(0, 0, 0, 120));
        d->AddRect(a, b, Rgba(236, 228, 206, 150), 0, 0, std::max(1.0f, g_v.L(1.5f)));
        return;
    }
    d->AddRectFilled(a, b, look::kGold);
    const float t = std::max(1.5f, g_v.L(3));
    if (on < of) {
        d->AddLine(g_v.P(x + 6, y + 12), g_v.P(x + 18, y + 12), look::kInk, t);
        return;
    }
    const ImVec2 tick[3] = {g_v.P(x + 5.5f, y + 12.5f), g_v.P(x + 10, y + 17), g_v.P(x + 18.5f, y + 7)};
    d->AddPolyline(tick, 3, look::kInk, 0, t);
}

// A stage's picture (its stage select background, 4:1) cropped to a box.
void StagePicture(ImDrawList* d, const std::string& code, float x0, float y0, float x1, float y1, bool dim)
{
    const TextureView* t = TextureSized(StageTex(code, "StageSelectBG"), g_v.L(x1 - x0));
    if (!t) {
        d->AddRectFilled(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(20, 22, 26));
        return;
    }
    const float aspect = t->height ? static_cast<float>(t->width) / t->height : 4.0f;
    const float visible = std::min(1.0f, ((x1 - x0) / (y1 - y0)) / aspect);
    Image(d, t, g_v.P(x0, y0), g_v.P(x1, y1), ImVec2(0.5f - visible / 2, 0), ImVec2(0.5f + visible / 2, 1), dim ? Rgba(95, 95, 95) : IM_COL32_WHITE);
}

// The list's grid moves: left and right along a row (around), up and down
// a column (around).
int MoveInGrid(int at, int n, int cols)
{
    const int row = at / cols, col = at % cols, rowLen = std::min(cols, n - row * cols);
    if (Left()) return row * cols + (col + rowLen - 1) % rowLen;
    if (Right()) return row * cols + (col + 1) % rowLen;
    if (Up()) {
        int to = at - cols;
        if (to < 0) {
            to = ((n - 1) / cols) * cols + col;
            if (to >= n) to -= cols;
        }
        return to;
    }
    if (Down()) return at + cols < n ? at + cols : col;
    return at;
}

// A stage's settings, as the game's own random stage menu has them: each
// on or off. Over the list, with the input.
void DrawVariantList(ImDrawList* d, const SelectFonts& f, const std::vector<ListStage>& list, std::vector<std::string>* off, bool* changed)
{
    const ListStage& l = list[g_ui.variantsOf];
    const int n = static_cast<int>(l.variants.size());
    ImFont* bold = f.bold ? f.bold : f.text;
    ImFont* medium = f.title ? f.title : f.text;
    g_ui.variantCursor = std::clamp(g_ui.variantCursor, 0, n - 1);
    constexpr float kRowH = 72, kGap = 8, kPic = 144;
    const auto [on, owned] = VariantsOn(l, *off);
    ui::ModalSpec m;
    m.w = 660;
    m.h = 44 + 20 + n * kRowH + (n - 1) * kGap + 24 + 48;
    m.x0 = (1600 - m.w) / 2;
    m.y0 = (900 - m.h) / 2;
    m.kicker = Upper(l.name);
    m.aside = std::to_string(on) + " of " + std::to_string(owned) + " on";
    const bool noted = !g_ui.listNote.empty() && ImGui::GetTime() < g_ui.listNoteUntil;
    m.hint = noted ? g_ui.listNote : "Random rolls the settings that are on.";
    m.hintCol = noted ? look::kGold : look::kSoft;
    m.prompts = {{"button_a", "On / off"}, {"button_y", on == owned ? "All off" : "All on"}, {"button_b", "Back"}};
    const float top = ui::Modal(d, bold, f.text, m, nullptr) + 20;
    const float x0 = m.x0 + 28, x1 = m.x0 + m.w - 28;
    for (int k = 0; k < n; k++) {
        const ListVariant& v = l.variants[k];
        const float y = top + k * (kRowH + kGap);
        const bool vOn = !v.locked && !IsOff(*off, v.code);
        ui::Control(d, x0, y, x1, y + kRowH, false);
        StagePicture(d, v.code, x0 + 1, y + 1, x0 + 1 + kPic, y + kRowH - 1, !vOn);
        const float tx = x0 + kPic + 16, room = x1 - 52 - tx;
        Text(d, bold, g_v.L(17), g_v.P(tx, y + 11), vOn ? look::kGold : look::kMuted, "Setting " + std::to_string(k + 1));
        Text(d, medium, g_v.L(18), g_v.P(tx, y + 37), vOn ? look::kCream : look::kMuted, FitText(medium, 18, v.name, room));
        if (v.locked) {
            d->AddRectFilled(g_v.P(x0, y), g_v.P(x1, y + kRowH), Rgba(0, 0, 0, 110));
            Text(d, f.text, g_v.L(15), g_v.P(x1 - 18, y + 27), look::kMuted, "Not owned", 1.0f);
        }
        else CheckBox(d, x1 - 40, y + (kRowH - 24) / 2, vOn ? 1 : 0, 1);
        if (k == g_ui.variantCursor) ui::Ring(d, x0, y, x1, y + kRowH);
        if (Clicked() && MouseIn(g_v.P(x0, y), g_v.P(x1, y + kRowH))) {
            g_ui.variantCursor = k;
            ui::ClickPad(ImGuiKey_GamepadFaceDown);
        }
    }
    if (Up()) g_ui.variantCursor = (g_ui.variantCursor + n - 1) % n;
    if (Down()) g_ui.variantCursor = (g_ui.variantCursor + 1) % n;
    const ListVariant& cur = l.variants[g_ui.variantCursor];
    if (Confirm()) {
        if (cur.locked) ListNote("You don't own " + cur.name + ".");
        else if (SetVariants(list, off, {cur.code}, IsOff(*off, cur.code))) *changed = true;
        else ListNote(kKeepOne);
    }
    if (NextSetting()) {
        if (SetVariants(list, off, OwnedCodes(l), on != owned)) *changed = true;
        else ListNote(kKeepOne);
    }
    if (Back()) g_ui.variantsOf = -1;
}

// My character's Random stages: a grid of the stages random can land on,
// each on or off; a stage with settings opens their list (as the game's own
// menu does). Every change goes to `in` at once; B closes it.
void DrawRandomList(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in)
{
    const std::vector<ListStage> list = RandomList();
    const int n = static_cast<int>(list.size());
    if (n == 0) {
        g_ui.randomList = false;
        return;
    }
    std::vector<std::string> off = live.randomOff;
    bool changed = false;
    ImFont* bold = f.bold ? f.bold : f.text;
    ImFont* medium = f.title ? f.title : f.text;
    if (g_ui.variantsOf >= n) g_ui.variantsOf = -1;
    const bool sub = g_ui.variantsOf >= 0;
    const bool inputOn = g_inputOn;
    if (sub) g_inputOn = false; // the settings list over it takes the input
    g_ui.listCursor = std::clamp(g_ui.listCursor, 0, n - 1);
    constexpr int kCols = 4;
    constexpr float kTileW = 350, kTileH = 58, kGapX = 12, kGapY = 8, kPic = 104;
    const int rows = (n + kCols - 1) / kCols;
    const auto [on, owned] = StagesOn(list, off);
    const ListStage& cur = list[g_ui.listCursor];
    ui::ModalSpec m;
    m.w = kCols * kTileW + (kCols - 1) * kGapX + 2 * 28;
    m.x0 = (1600 - m.w) / 2;
    const float gridY = 44 + 22;
    m.h = gridY + rows * kTileH + (rows - 1) * kGapY + 24 + 48;
    m.y0 = std::max(8.0f, (900 - m.h) / 2);
    m.kicker = "RANDOM STAGES";
    m.aside = std::to_string(on) + " of " + std::to_string(owned) + " stages on";
    const bool noted = !g_ui.listNote.empty() && ImGui::GetTime() < g_ui.listNoteUntil && !sub;
    m.hint = noted ? g_ui.listNote : "Choose which stages can be chosen from at random.";
    m.hintCol = noted ? look::kGold : look::kSoft;
    m.prompts = {{"button_a", cur.variants.size() > 1 ? "Settings" : "On / off"}, {"button_y", on == owned ? "All off" : "All on"}, {"button_b", "Done"}};
    ui::Modal(d, bold, f.text, m, nullptr);
    const float gx = m.x0 + 28, gy = m.y0 + gridY;
    for (int i = 0; i < n; i++) {
        const ListStage& l = list[i];
        const float x = gx + (i % kCols) * (kTileW + kGapX), y = gy + (i / kCols) * (kTileH + kGapY);
        const auto [von, vowned] = VariantsOn(l, off);
        const bool lit = von > 0;
        ui::Control(d, x, y, x + kTileW, y + kTileH, false);
        StagePicture(d, l.variants[0].code, x + 1, y + 1, x + 1 + kPic, y + kTileH - 1, !lit);
        const float tx = x + kPic + 14, room = kTileW - kPic - 14 - 48;
        const bool second = l.variants.size() > 1 || l.locked;
        Text(d, medium, g_v.L(17), g_v.P(tx, y + (second ? 8 : 18)), lit ? look::kCream : look::kMuted, FitText(medium, 17, l.name, room));
        if (l.locked) {
            d->AddRectFilled(g_v.P(x, y), g_v.P(x + kTileW, y + kTileH), Rgba(0, 0, 0, 110));
            Text(d, f.text, g_v.L(15), g_v.P(tx, y + 33), look::kMuted, "Not owned");
        }
        else {
            if (l.variants.size() > 1) {
                Text(d, f.text, g_v.L(15), g_v.P(tx, y + 33), lit ? look::kGold : look::kMuted,
                     std::to_string(von) + " of " + std::to_string(vowned) + " settings");
            }
            CheckBox(d, x + kTileW - 38, y + (kTileH - 24) / 2, von, vowned);
        }
        if (i == g_ui.listCursor) ui::Ring(d, x, y, x + kTileW, y + kTileH);
        if (Clicked() && MouseIn(g_v.P(x, y), g_v.P(x + kTileW, y + kTileH))) {
            g_ui.listCursor = i;
            ui::ClickPad(ImGuiKey_GamepadFaceDown);
        }
    }
    if (!sub) {
        g_ui.listCursor = MoveInGrid(g_ui.listCursor, n, kCols);
        const ListStage& at = list[g_ui.listCursor];
        if (Confirm()) {
            const auto [von, vowned] = VariantsOn(at, off);
            if (at.locked) ListNote("You don't own " + at.name + ".");
            else if (at.variants.size() > 1) {
                g_ui.variantsOf = g_ui.listCursor;
                g_ui.variantCursor = 0;
            }
            else if (SetVariants(list, &off, OwnedCodes(at), von == 0)) changed = true;
            else ListNote(kKeepOne);
        }
        if (NextSetting()) {
            if (on != owned) {
                off.clear();
                changed = true;
            }
            else {
                // All off but the stage under the cursor (random needs one).
                int keep = g_ui.listCursor;
                for (int i = 0; list[keep].locked && i < n; i++) keep = i;
                std::vector<std::string> codes;
                for (int i = 0; i < n; i++) {
                    if (i == keep) continue;
                    for (const std::string& c : OwnedCodes(list[i])) codes.push_back(c);
                }
                if (SetVariants(list, &off, codes, false)) {
                    changed = true;
                    ListNote("All off but " + list[keep].name + ": random needs one stage.");
                }
            }
        }
        if (Back()) g_ui.randomList = false;
    }
    else {
        g_inputOn = inputOn;
        DrawVariantList(d, f, list, &off, &changed);
    }
    g_inputOn = inputOn;
    if (changed) {
        in->randomOffChanged = true;
        in->randomOff = off;
    }
}

// The stage select: the stage's picture inside the game's curved frame, its
// name, and the strip of stages. Only the host moves it; up, down and Y
// change a stage's setting (The Grid: The Grid Alternative), shown in its
// place.
void DrawStageSelect(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in)
{
    const int count = static_cast<int>(g_draw->stages.size());
    const bool picking = live.host && live.you >= 0;
    int stage = std::clamp(live.stage, 0, count);
    int setting = live.setting;
    // The cursor steps over stages (and settings) this profile does not own;
    // random (the last place) is always there.
    auto lockedAt = [&](int i, int k) {
        const SelectStage* at = Shown(i, k);
        return at && at->locked;
    };
    auto step = [&](int from, int by) {
        int i = from;
        for (int tries = 0; tries <= count; tries++) {
            i = (i + by + count + 1) % (count + 1);
            if (!lockedAt(i, 0)) return i;
        }
        return from;
    };
    if (picking && Left()) in->stage = stage = step(stage, -1), setting = 0;
    if (picking && Right()) in->stage = stage = step(stage, 1), setting = 0;
    const int settings = stage < count ? static_cast<int>(g_draw->stages[stage].settings.size()) : 0;
    setting = std::clamp(setting, 0, settings);
    if (picking && settings) {
        const bool up = Up(), down = Down(), next = NextSetting();
        const int by = up ? -1 : (down || next) ? 1 : 0;
        for (int tries = 0; by && tries <= settings; tries++) {
            setting = (setting + by + settings + 1) % (settings + 1);
            if (!lockedAt(stage, setting)) break;
        }
        if (by) in->setting = setting;
    }
    const SelectStage* shown = Shown(stage, setting);
    Text(d, f.name, g_v.L(42), g_v.P(800, 76), Rgba(255, 255, 255), "STAGE SELECT", 0.5f, true);
    // The frame across the whole screen (its texture is 1920x534).
    const float bx = 0, by = 132, bw = 1600, bh = 1600.0f * 534 / 1920;
    if (const TextureView* shadow = Texture(MenuTex("StagePreviewWindowShadow"))) Image(d, shadow, g_v.P(bx, by), g_v.P(bx + bw, by + bh));
    const bool random = stage >= count;
    const std::string code = shown ? shown->code : "";
    if (const TextureView* bg = code.empty() ? nullptr : Texture(StageTex(code, "StageSelectBG"))) {
        // The mask (StagePreviewBG, 1920x512, 11 px into the frame) curves 2 px
        // from the edge at the sides and 43 px at the middle, top and bottom
        // alike; slices follow it. The picture covers it, cropped at the sides.
        const float my = by + bh * 11 / 534, mh = bh * 512 / 534;
        const float visible = (bw / mh) / (static_cast<float>(bg->width) / std::max(1u, bg->height));
        const float u0 = 0.5f - visible / 2, u1 = 0.5f + visible / 2;
        const int slices = 64;
        for (int i = 0; i < slices; i++) {
            auto edge = [&](float t) {
                const float c = 2 * t - 1;
                return (2 + 41 * (1 - c * c)) / 512.0f * mh;
            };
            const float t0 = static_cast<float>(i) / slices, t1 = static_cast<float>(i + 1) / slices;
            const float x0 = bx + bw * t0, x1 = bx + bw * t1;
            const float a0 = edge(t0), a1 = edge(t1);
            const ImVec2 q0 = g_v.P(x0, my + a0), q1 = g_v.P(x1, my + a1), q2 = g_v.P(x1, my + mh - a1), q3 = g_v.P(x0, my + mh - a0);
            const float ua = u0 + (u1 - u0) * t0, ub = u0 + (u1 - u0) * t1;
            d->AddImageQuad(Id(bg), q0, q1, q2, q3, ImVec2(ua, a0 / mh), ImVec2(ub, a1 / mh), ImVec2(ub, 1 - a1 / mh), ImVec2(ua, 1 - a0 / mh), FadeIn(bg));
        }
    }
    if (const TextureView* frame = Texture(MenuTex("StagePreviewWindow"))) Image(d, frame, g_v.P(bx, by), g_v.P(bx + bw, by + bh));
    const std::string name = random ? "Random" : shown ? shown->name : "";
    Text(d, f.name, g_v.L(46), g_v.P(800, by + bh - 106), Rgba(255, 255, 255), name, 0.5f, true);
    if (random && live.myCharacter) {
        // My character's Random stages: how many it can land on.
        const auto [on, owned] = StagesOn(RandomList(), live.randomOff);
        const std::string line = on == owned ? "All " + std::to_string(owned) + " stages" : std::to_string(on) + " of " + std::to_string(owned) + " stages";
        const float w = TextW(f.text, 16, line) + 36, y = by + bh - 146;
        Pill(d, g_v.P(800 - w / 2, y), g_v.P(800 + w / 2, y + 30));
        Text(d, f.text, g_v.L(16), g_v.P(800, y + 6), Rgba(233, 196, 106), line, 0.5f);
    }
    if (settings) {
        char line[32];
        std::snprintf(line, sizeof(line), "Setting %d / %d", setting + 1, settings + 1);
        const float w = TextW(f.text, 16, line) + 36, y = by + bh - 146;
        Pill(d, g_v.P(800 - w / 2, y), g_v.P(800 + w / 2, y + 30));
        Text(d, f.text, g_v.L(16), g_v.P(800, y + 6), Rgba(233, 196, 106), line, 0.5f);
    }
    Infinity(d, g_v.P(800, by + bh + 30), g_v.L(56), Rgba(242, 201, 76));
    // The strip: each stage's thin picture, then random.
    const float sw = 29, sh = 168, gap = 3;
    const float total = (count + 1) * sw + count * gap;
    const float sx = 800 - total / 2, sy = by + bh + 64;
    for (int i = 0; i <= count; i++) {
        const bool on = i == stage;
        const float x = sx + i * (sw + gap);
        const float lift = on ? 8.0f : 0.0f, grow = on ? 1.12f : 1.0f;
        const float cx = x + sw / 2, cy = sy + sh / 2 - lift;
        const ImVec2 a = g_v.P(cx - sw * grow / 2, cy - sh * grow / 2), b = g_v.P(cx + sw * grow / 2, cy + sh * grow / 2);
        const std::string icon = on && shown ? shown->code : i < count ? g_draw->stages[i].code : "";
        const TextureView* t = i < count ? Texture(StageTex(icon, "StageSelectIcon")) : Texture(MenuTex("StageSelect_Random"));
        if (t) Image(d, t, a, b);
        else d->AddRectFilled(a, b, Rgba(30, 32, 36));
        if (i < count && g_draw->stages[i].locked) d->AddRectFilled(a, b, Rgba(0, 0, 0, 170));
        if (on) {
            if (const TextureView* cursor = Texture(MenuTex("StageSelectCursor"))) Image(d, cursor, ImVec2(a.x - g_v.L(1), a.y - g_v.L(1)), ImVec2(b.x + g_v.L(1), b.y + g_v.L(1)));
        }
        if (picking && Clicked() && MouseIn(g_v.P(x, sy), g_v.P(x + sw, sy + sh)) && !lockedAt(i, 0)) in->stage = i;
    }
    Text(d, f.text, g_v.L(19), g_v.P(800, sy + sh + 16), Rgba(232, 228, 216),
         picking ? "Pick the stage." : "Player 1 is picking the stage...", 0.5f);
    if (picking && Confirm() && !lockedAt(stage, setting)) in->lockStage = true;
}

void MoveCursor(int dr, int dx)
{
    if (dx) {
        const int n = static_cast<int>(g_rows[g_ui.cursorRow].size());
        g_ui.cursorX = (g_ui.cursorX + dx + n) % n;
    }
    if (dr) {
        const ImVec2* q = g_rows[g_ui.cursorRow][g_ui.cursorX].q;
        const float x = (q[0].x + q[2].x) / 2;
        const int to = 1 - g_ui.cursorRow;
        int best = 0;
        float bestD = 1e9f;
        for (int i = 0; i < static_cast<int>(g_rows[to].size()); i++) {
            const ImVec2* o = g_rows[to][i].q;
            const float dist = std::fabs((o[0].x + o[2].x) / 2 - x);
            if (dist < bestD) {
                bestD = dist;
                best = i;
            }
        }
        g_ui.cursorRow = to;
        g_ui.cursorX = best;
    }
}


constexpr const char* kLoadingText = "Both ready. Loading...";

// A line of status over the bottom strip.
void StatusPill(ImDrawList* d, const SelectFonts& f, const char* text)
{
    const float w = TextW(f.text, 19, text) + 44;
    Pill(d, g_v.P(800 - w / 2, 856), g_v.P(800 + w / 2, 892));
    Text(d, f.text, g_v.L(19), g_v.P(800, 862), Rgba(233, 196, 106), text, 0.5f);
}


// B with nothing to go back to: the leave dialog, on Stay.
void OpenLeave()
{
    g_ui.leaving = true;
    g_ui.leaveChosen = false;
}

// The leave dialog over the screen: Leave or Stay, left and right to choose,
// A (or a click) to take it, B stays.
void DrawLeaveDialog(ImDrawList* d, const SelectFonts& f, const SelectLive& live, bool takeInput, SelectInput* in)
{
    // The S modal (600 wide): its kicker, a title, what happens, Leave and
    // Stay; the prompts in its footer.
    const bool watching = live.you < 0;
    const char* title = watching ? "Stop watching?" : live.set ? "Leave the set?" : "Leave the session?";
    const char* detail = watching   ? "You'll return to the main menu."
                         : live.set ? "The set ends here for both players."
                                    : "The session ends for both players.";
    ImFont* bold = f.bold ? f.bold : f.text;
    ui::ModalSpec m;
    m.w = 600;
    m.h = 44 + 22 + 34 + 12 + 24 + 24 + 48 + 26 + 46;
    m.x0 = 500;
    m.y0 = (844 - m.h) / 2;
    m.kicker = watching ? "STOP WATCHING" : "LEAVE";
    m.danger = !watching;
    m.prompts = {{"button_a", "Select"}, {"button_b", "Stay"}};
    int prompt = -1;
    float y = ui::Modal(d, bold, f.text, m, &prompt) + 22;
    Text(d, f.name, g_v.L(28), g_v.P(528, y), Rgba(244, 241, 232), title);
    y += 46;
    Text(d, f.text, g_v.L(19), g_v.P(528, y), look::kCream, detail);
    y += 24 + 24;
    int pressed = -1; // 1 Leave, 0 Stay
    if (ui::Button(d, bold, 528, y, 806, y + 48, watching ? "Stop watching" : "Leave", g_ui.leaveChosen, !watching)) pressed = 1;
    if (ui::Button(d, bold, 822, y, 1072, y + 48, "Stay", !g_ui.leaveChosen)) pressed = 0;
    if (!takeInput) return;
    if (Left() || Right()) g_ui.leaveChosen = !g_ui.leaveChosen;
    if (pressed >= 0) g_ui.leaveChosen = pressed == 1;
    if (Back()) g_ui.leaving = false;
    else if (Confirm() || pressed >= 0) {
        in->leave = g_ui.leaveChosen;
        g_ui.leaving = false;
    }
}

// The whole screen for a lobby's state; what the player does goes to `in`.
// My character's CURRENT LOADOUT card, where the other player's side is.
void DrawCard(ImDrawList* d, const SelectFonts& f, const SelectCard& c)
{
    // Sized to its rows (the medium weight at 18, a row every 36).
    const float x0 = 980, y0 = 108, x1 = 1552, y1 = y0 + 48 + 66 + 20 + static_cast<float>(c.rows.size()) * 36 + 62;
    const ImU32 white = look::kCream, dim = look::kSoft, text = look::kCream, gold = look::kGold, green = Rgba(127, 216, 139);
    ImFont* medium = f.title ? f.title : f.text;
    ImFont* bold = f.bold ? f.bold : medium;
    d->AddRectFilled(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(8, 9, 11, 228));
    d->AddRect(g_v.P(x0, y0), g_v.P(x1, y1), Rgba(201, 168, 92, 150), 0, 0, std::max(1.0f, g_v.L(1)));
    Text(d, bold, g_v.L(16), g_v.P(x0 + 22, y0 + 18), gold, "CURRENT LOADOUT");
    // The player bar: the pick's grid icon, name (you), character, place.
    const float bx0 = x0 + 22, by0 = y0 + 48, bx1 = x1 - 22, by1 = by0 + 66;
    d->AddRectFilled(g_v.P(bx0, by0), g_v.P(bx1, by1), Rgba(0, 0, 0, 120));
    float x = bx0 + 12;
    if (const TextureView* t = c.chara.empty() ? nullptr : Texture(CharaTex(c.chara, "CharaSelectIconLeft"))) {
        // The icon's portrait (116x144) as the grid shows it, upright.
        Image(d, t, g_v.P(x, by0 + 8), g_v.P(x + 40, by0 + 58), ImVec2(0.12f, 0.0f), ImVec2(0.88f, 0.95f));
    }
    x += 52;
    const std::string player = Upper(c.player.empty() ? "Player" : c.player);
    Text(d, bold, g_v.L(19), g_v.P(x, by0 + 21), white, player);
    x += TextW(bold, 19, player) + 6;
    Text(d, f.text, g_v.L(17), g_v.P(x, by0 + 22), dim, "(you)");
    x += TextW(f.text, 17, "(you)") + 14;
    Text(d, medium, g_v.L(18), g_v.P(x, by0 + 21), white, c.charaName);
    Text(d, medium, g_v.L(17), g_v.P(bx1 - 12, by0 + 22), gold, c.place, 1.0f);
    // The rows: labels soft, values cream, both the medium weight.
    float y = by1 + 20;
    for (const SelectCard::Row& r : c.rows) {
        Text(d, f.text, g_v.L(18), g_v.P(x0 + 26, y), dim, r.label);
        const std::string value = FitText(medium, 18, r.value, x1 - 22 - (x0 + 170) - (r.swatch1 ? 46 : 0));
        Text(d, medium, g_v.L(18), g_v.P(x0 + 170, y), white, value);
        if (r.swatch1) {
            const float sx = x0 + 170 + TextW(medium, 18, value) + 10;
            d->AddRectFilled(g_v.P(sx, y + 2), g_v.P(sx + 17, y + 19), FromArgb(r.swatch1));
            d->AddRectFilled(g_v.P(sx + 21, y + 2), g_v.P(sx + 38, y + 19), FromArgb(r.swatch2));
        }
        y += 36;
    }
    // The status and the loading bar.
    const float sy = y1 - 50;
    Text(d, medium, g_v.L(17), g_v.P(x0 + 22, sy), c.done ? green : text, c.status);
    Text(d, medium, g_v.L(17), g_v.P(x1 - 22, sy), c.done ? green : gold, c.statusRight, 1.0f);
    if (c.progress >= 0) {
        const float ly = y1 - 22, lx0 = x0 + 22, lx1 = x1 - 22;
        d->AddRectFilled(g_v.P(lx0, ly), g_v.P(lx1, ly + 4), Rgba(255, 255, 255, 40));
        const float p = std::clamp(c.progress, 0.0f, 1.0f);
        d->AddRectFilled(g_v.P(lx0, ly), g_v.P(lx0 + (lx1 - lx0) * p, ly + 4), c.done ? green : gold);
    }
}

void Render(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in)
{
    g_myCharacter = live.myCharacter;
    if (g_gridFor != g_draw) BuildGrid(*g_draw);
    const bool watching = live.you < 0;
    const SelectSide* me = watching ? nullptr : &live.sides[live.you];
    if (live.stageLocked && !g_ui.wasLocked && !(live.myCharacter && g_ui.cameFromStage)) {
        // On to the characters: the cursor starts on the player's pick.
        g_ui.loadout = false;
        g_ui.onRandom = false;
        g_ui.lastChara = -2;
    }
    if (!live.stageLocked) g_ui.cameFromStage = true;
    else if (g_ui.wasLocked) g_ui.cameFromStage = false;
    g_ui.wasLocked = live.stageLocked;
    // While the leave dialog is up it takes the input: the screen behind is
    // drawn as it is.
    if (!live.myCharacter || live.stageLocked) g_ui.randomList = false;
    const bool dialog = g_ui.leaving || g_ui.randomList, inputOn = g_inputOn;
    if (dialog) g_inputOn = false;

    std::vector<Prompt> prompts;
    if (!live.stageLocked) {
        DrawBackdrop(d, live.stage, live.setting, Rgba(70, 69, 67));
        d->AddRectFilledMultiColor(g_v.P(0, 0), g_v.P(1600, 450), Rgba(6, 8, 11, 90), Rgba(6, 8, 11, 90), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0));
        d->AddRectFilledMultiColor(g_v.P(0, 450), g_v.P(1600, 900), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 170), Rgba(6, 8, 11, 170));
        DrawStageSelect(d, f, live, in);
        // What the character select draws, loading while the stage is picked
        // (after this screen's own: the textures load in the order asked).
        for (const auto& row : g_rows) {
            for (const Slot& t : row) {
                if (const SelectChara* c = Chara(t.chara)) Texture(CharaTex(c->code, t.right ? "CharaSelectIconRight" : "CharaSelectIconLeft"));
            }
        }
        for (const SelectSide& s : live.sides) {
            if (const SelectChara* c = s.present ? Chara(s.chara) : nullptr) Texture(CharaTex(c->code, "PartCharacter"));
        }
        for (const char* icon : {"button_a", "button_b", "button_y"}) Texture(PadTex(icon));
        const SelectStage* stage = Shown(live.stage, 0);
        if (live.host && !watching && stage && !stage->settings.empty()) prompts.push_back({"button_y", "Setting", 3});
        // My character, on the random tile: X opens the Random stages list.
        const bool onRandomStage = live.myCharacter && (in->stage >= 0 ? in->stage : live.stage) >= static_cast<int>(g_draw->stages.size());
        if (onRandomStage) prompts.push_back({"button_x", "Random stages", 2});
        if (live.host && !watching) prompts.push_back({"button_a", "Confirm stage", 0});
        prompts.push_back({"button_b", live.myCharacter ? "Back" : "Leave", 1});
        if (onRandomStage && Pressed({ImGuiKey_GamepadFaceLeft}, false)) {
            g_ui.randomList = true;
            g_ui.variantsOf = -1;
            g_ui.listNote.clear();
        }
        if (Back()) {
            if (live.myCharacter) in->closeStage = true;
            else OpenLeave();
        }
    }
    else {
        const bool ready = me && me->ready;
        const bool grid = me && !ready && !g_ui.loadout;
        const bool loadout = me && !ready && g_ui.loadout;
        // The cursor follows the player's pick (also when it changes elsewhere).
        if (me && me->chara != g_ui.lastChara) {
            g_ui.onRandom = !CursorTo(me->chara) && g_ui.onRandom;
            g_ui.lastChara = me->chara;
        }
        if (grid) {
            if (Left()) MoveCursor(0, -1);
            if (Right()) MoveCursor(0, 1);
            if (Up() || Down()) MoveCursor(1, 0);
            g_ui.onRandom = g_rows[g_ui.cursorRow][g_ui.cursorX].random;
        }
        // Each side as that player sees their own screen: on the grid the
        // character under their cursor (none on the random tile), past it
        // their pick and loadout. The other player's comes from the session
        // (screen 0: not known, drawn from the pick as before).
        const int hover = g_ui.onRandom ? -1 : g_rows[g_ui.cursorRow][g_ui.cursorX].chara;
        bool onGrid[2], onRandom[2], showLoadout[2];
        int shown[2], lit[2];
        for (int i = 0; i < 2; i++) {
            const SelectSide& s = live.sides[i];
            const bool mine = live.you == i;
            onGrid[i] = mine ? grid : s.present && !s.ready && s.screen == 1;
            onRandom[i] = onGrid[i] && (mine ? g_ui.onRandom : s.hoverRandom);
            shown[i] = !s.present ? -1 : onGrid[i] ? (onRandom[i] ? -1 : mine ? hover : s.hover) : s.chara;
            showLoadout[i] = s.present && s.chara >= 0 &&
                             (mine ? !grid : s.ready || s.screen == 2 || (s.screen == 0 && (watching || !grid)));
            lit[i] = mine ? (loadout ? g_ui.row : -1) : !s.ready && s.screen == 2 ? s.row : -1;
        }
        in->screen = grid ? 1 : me ? 2 : 0;
        in->hover = hover;
        in->hoverRandom = g_ui.onRandom;
        in->screenRow = g_ui.row;
        if (loadout) {
            if (Up()) g_ui.row = (g_ui.row + kSelectRows - 1) % kSelectRows;
            if (Down()) g_ui.row = (g_ui.row + 1) % kSelectRows;
            if (!me->rows[g_ui.row].fixed) {
                if (Left()) in->row = g_ui.row, in->delta = -1;
                if (Right()) in->row = g_ui.row, in->delta = 1;
            }
        }

        // The chosen stage behind everything.
        DrawBackdrop(d, live.stage, live.setting, Rgba(118, 116, 112));
        d->AddRectFilledMultiColor(g_v.P(0, 0), g_v.P(1600, 300), Rgba(6, 8, 11, 120), Rgba(6, 8, 11, 120), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0));
        DrawRender(d, shown[0], false);
        DrawRender(d, shown[1], true);
        d->AddRectFilledMultiColor(g_v.P(0, 500), g_v.P(1600, 640), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 230), Rgba(6, 8, 11, 230));
        d->AddRectFilled(g_v.P(0, 640), g_v.P(1600, 900), Rgba(6, 8, 11, 200));
        // The loadouts: each side's once that player is past the grid.
        for (int i = 0; i < 2; i++) {
            if (showLoadout[i]) DrawLoadout(d, f, i == 1, live.sides[i], lit[i], loadout && live.you == i, in);
        }
        for (int i = 0; i < 2; i++) {
            if (live.myCharacter && i == 1) continue;
            DrawPlayerLine(d, f, live, i);
            const SelectChara* c = Chara(shown[i]);
            const std::string name = onRandom[i] ? "RANDOM" : c ? Upper(c->name) : "";
            Text(d, f.name, g_v.L(74), g_v.P(i ? 1482.0f : 118.0f, 496), Rgba(255, 255, 255), name, i ? 1.0f : 0.0f, true);
        }
        Text(d, f.name, g_v.L(42), g_v.P(800, 502), Rgba(255, 255, 255), live.myCharacter ? "MY CHARACTER" : "CHARACTER SELECT", 0.5f, true);
        if (live.myCharacter) {
            DrawCard(d, f, live.card);
            const std::string& note = loadout ? me->rows[g_ui.row].hint : live.note;
            Text(d, f.text, g_v.L(19), g_v.P(48, 860), Rgba(236, 232, 220), note);
        }
        else Infinity(d, g_v.P(800, 578), g_v.L(56), Rgba(242, 201, 76));
        // The grid: each side's tag on what it shows; on the random tile while
        // that player's cursor is there.
        for (int r = 0; r < 2; r++) {
            for (int x = 0; x < static_cast<int>(g_rows[r].size()); x++) {
                const Slot& t = g_rows[r][x];
                if (r == 1 && t.random) continue; // drawn with the top row
                bool tag[2];
                for (int i = 0; i < 2; i++) tag[i] = t.random ? onRandom[i] : t.chara >= 0 && t.chara == shown[i];
                DrawTile(d, t, tag[0], tag[1], f);
                if (grid && Clicked() && MouseInQuad(t.q)) {
                    // A click moves the cursor; on the tile under it, picks.
                    if (g_ui.cursorRow == r && g_ui.cursorX == x) ui::ClickPad(ImGuiKey_GamepadFaceDown);
                    g_ui.cursorRow = r;
                    g_ui.cursorX = x;
                    g_ui.onRandom = t.random;
                }
            }
        }
        if (grid && live.myCharacter) {
            prompts.push_back({"button_a", "Select", 0});
            prompts.push_back({"button_x", "Preferred stage", 2});
            prompts.push_back({"button_y", "Random", 3});
            prompts.push_back({"button_b", live.keepLabel.empty() ? "Back" : live.keepLabel, 1});
        }
        else if (grid) {
            prompts.push_back({"button_a", "Select", 0});
            prompts.push_back({"button_b", live.host ? "Stage" : "Leave", 1});
        }
        else if (watching) {
            prompts.push_back({"button_b", "Leave", 1});
        }
        else if (loadout && live.myCharacter) {
            prompts.push_back({"button_a", "Confirm", 0});
            prompts.push_back({"button_x", "Preferred stage", 2});
            prompts.push_back({"button_b", "Characters", 1});
        }
        else if (loadout) {
            prompts.push_back({"button_a", "Ready", 0});
            prompts.push_back({"button_b", "Back", 1});
        }
        // A character this profile does not own: its art shows, it cannot be
        // picked (the game's own select has it the same way).
        const SelectChara* under = !g_ui.onRandom && hover >= 0 ? Chara(hover) : nullptr;
        if (grid && under && under->locked) StatusPill(d, f, ("You don't own " + under->name).c_str());
        if (ready && live.myCharacter) {
            prompts.push_back({"button_a", "Back to the room", 0});
            prompts.push_back({"button_b", "Change again", 1});
        }
        else if (ready) {
            // B says not ready, until both are: then the match is locked in
            // (B leaves).
            if (live.bothReady) {
                StatusPill(d, f, live.loading ? kLoadingText : "Both ready. Starting...");
                prompts.push_back({"button_b", "Leave", 1});
            }
            else {
                StatusPill(d, f, "Ready. Waiting for your opponent...");
                prompts.push_back({"button_b", "Not ready", 1});
            }
        }
        // A and B.
        const bool a = Confirm();
        if (grid && a && under && under->locked) {
            // Not owned: stays on the grid (as the game's select does).
        }
        else if (grid && a) {
            if (g_ui.onRandom) in->randomPick = true;
            else if (hover >= 0) in->chara = hover;
            g_ui.loadout = true;
            g_ui.onRandom = false;
            g_ui.lastChara = -2;
            g_ui.row = 0;
        }
        else if (loadout && a) {
            in->ready = true;
        }
        else if (ready && a && live.myCharacter) {
            in->done = true;
        }
        if ((grid || loadout) && live.myCharacter && Pressed({ImGuiKey_GamepadFaceLeft}, false)) in->openStage = true;
        if (grid && live.myCharacter && Pressed({ImGuiKey_GamepadFaceUp}, false)) {
            in->randomPick = true;
            g_ui.loadout = true;
            g_ui.onRandom = false;
            g_ui.lastChara = -2;
            g_ui.row = 0;
        }
        if (Back()) {
            if (grid && live.myCharacter) in->cancel = true;
            else if (ready && !live.bothReady) in->unready = true;
            else if (loadout) g_ui.loadout = false;
            else if (grid && live.host) in->unlockStage = true;
            else OpenLeave();
        }
    }
    if (watching) {
        // Watching: the spectator's own link to the host, and who else watches.
        const char* text = "WATCHING";
        char detail[64];
        if (live.pingMs) std::snprintf(detail, sizeof(detail), "%u ms to the host  \xC2\xB7  %d watching", live.pingMs, std::max(1, live.spectators));
        else std::snprintf(detail, sizeof(detail), "%d watching", std::max(1, live.spectators));
        const float tw = TextW(f.text, 19, text), dw = TextW(f.text, 18, detail);
        const float w = 24 + tw + 18 + 30 + dw + 24;
        const float bx = 800 - w / 2;
        Pill(d, g_v.P(bx, 16), g_v.P(bx + w, 52));
        Text(d, f.text, g_v.L(19), g_v.P(bx + 24, 22), Rgba(242, 201, 76), text);
        const float ix = bx + 24 + tw + 18;
        if (live.relayed) IconCloud(d, g_v.P(ix + 10, 34), g_v.s, Rgba(233, 196, 106));
        else IconLink(d, g_v.P(ix + 10, 34), g_v.s, Rgba(217, 212, 198));
        Text(d, f.text, g_v.L(18), g_v.P(ix + 28, 23), Rgba(236, 232, 220), detail);
    }
    if (watching && live.loading) StatusPill(d, f, kLoadingText);
    g_inputOn = inputOn;
    // The leave modal has its prompts in its footer.
    if (g_ui.leaving) DrawLeaveDialog(d, f, live, dialog, in);
    else if (g_ui.randomList) DrawRandomList(d, f, live, in);
    else DrawPrompts(d, f, prompts);
}


// ----- the preview's mock players -----------------------------------------------

std::atomic<bool> g_previewOpen{false};
bool g_previewFresh = false;
int g_previewPhase = 0;
SelectData g_preview;

struct Mock {
    int stage = 0, setting = 0;
    bool locked = false;
    int chara[2] = {0, 0};
    int values[2][kSelectRows] = {{0, 0, 0, 0, 0, 0, 2}, {1, 0, 0, 1, 0, 0, 3}};
    bool ready[2] = {false, true};
    // Watching: the mock players act on a clock that starts over with each phase.
    int shown = -1;
    float inPhase = 0, nextAct = 0;
    uint32_t roll = 1;
} g_mock;

int MockPhase()
{
    if (!g_mock.locked) return 0;
    if (g_mock.ready[0]) return 3;
    return g_ui.loadout ? 2 : 1;
}

void SetMockPhase(int phase)
{
    g_mock.locked = phase > 0;
    g_ui.loadout = phase >= 2;
    g_ui.wasLocked = g_mock.locked;
    g_mock.ready[0] = phase == 3;
    g_mock.ready[1] = !g_preview.spectator || phase == 3;
}

int MockCount(const SelectChara& c, int row, int costume)
{
    switch (row) {
    case 0: return std::max<int>(1, static_cast<int>(c.costumes.size()));
    case 1: {
        const size_t k = std::min<size_t>(costume, c.swatches.empty() ? 0 : c.swatches.size() - 1);
        return c.swatches.empty() || c.swatches[k].empty() ? 1 : static_cast<int>(c.swatches[k].size());
    }
    case 2: return 1;
    case 3: return std::max<int>(2, static_cast<int>(c.vskills.size()));
    case 4: return std::max<int>(2, static_cast<int>(c.vtriggers.size()));
    case 5: return 1 + static_cast<int>(c.vcomments.size());
    default: return 11;
    }
}

SelectSide MockSide(int i)
{
    SelectSide s;
    s.present = true;
    s.player = g_preview.spectator ? (i ? "Mock opponent" : "Mock host") : i ? "Mock opponent" : (g_preview.you.empty() ? "You" : g_preview.you);
    s.link = i ? 2 : 1;
    s.chara = g_mock.chara[i];
    s.ready = g_mock.ready[i];
    const SelectChara& c = g_preview.charas[std::clamp(s.chara, 0, static_cast<int>(g_preview.charas.size()) - 1)];
    const int* v = g_mock.values[i];
    const bool eleven = c.code == "ELV";
    auto roman = [](int n) { return n == 1 ? std::string("I") : n == 2 ? std::string("II") : std::to_string(n); };
    for (int r = 0; r < kSelectRows; r++) s.rows[r].fixed = eleven && r < 6;
    if (eleven) {
        for (int r = 0; r < 6; r++) s.rows[r].value = "Random";
    }
    else {
        s.rows[0].value = c.costumes.empty() ? "Default" : c.costumes[v[0] % c.costumes.size()];
        const size_t ci = std::min<size_t>(v[0], c.swatches.empty() ? 0 : c.swatches.size() - 1);
        const int colors = MockCount(c, 1, v[0]);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d", v[1] % colors + 1);
        s.rows[1].value = buf;
        if (!c.swatches.empty() && !c.swatches[ci].empty()) {
            s.rows[1].swatch1 = c.swatches[ci][v[1] % colors].first;
            s.rows[1].swatch2 = c.swatches[ci][v[1] % colors].second;
        }
        s.rows[2].value = "Normal";
        s.rows[2].fixed = true;
        for (int k = 0; k < 2; k++) {
            const auto& names = k ? c.vtriggers : c.vskills;
            const int n = v[3 + k] % std::max<int>(2, static_cast<int>(names.size()));
            s.rows[3 + k].value = roman(n + 1) + (n < static_cast<int>(names.size()) && !names[n].empty() ? "  " + names[n] : "");
        }
        const int comment = v[5] % (1 + static_cast<int>(c.vcomments.size()));
        s.rows[5].value = comment == 0 ? "AUTO" : std::to_string(comment);
    }
    s.rows[6].value = std::to_string(v[6]) + (v[6] == 1 ? " frame" : " frames");
    const char* hints[kSelectRows] = {"", "", "", "", "", "", "Suggested for a 38 ms ping: 2 frames. Lower feels sharper but rolls back more."};
    const int comment = v[5] % (1 + static_cast<int>(c.vcomments.size()));
    const std::string commentLine = eleven ? "Plays a random character in silver, with a random V-Skill and V-Trigger."
                                    : comment == 0 ? "Changes with your opponent's character."
                                                   : "\"" + c.vcomments[comment - 1] + "\"";
    for (int r = 0; r < kSelectRows; r++) s.rows[r].hint = r == 5 || eleven ? commentLine : hints[r];
    return s;
}

void MockTick(float dt)
{
    const int phase = MockPhase();
    if (g_mock.shown != phase) {
        g_mock.shown = phase;
        g_mock.inPhase = 0;
        g_mock.nextAct = 0.6f;
    }
    g_mock.inPhase += dt;
    if (phase == 3 || g_mock.inPhase < g_mock.nextAct) return;
    auto next = [] {
        g_mock.roll = g_mock.roll * 1664525u + 1013904223u;
        return g_mock.roll >> 8;
    };
    const int stages = static_cast<int>(g_preview.stages.size());
    if (phase == 0) {
        g_mock.stage = (g_mock.stage + 1) % (stages + 1);
        g_mock.setting = 0;
        g_mock.nextAct += 0.55f;
        return;
    }
    if (phase == 1) {
        std::vector<int> picks;
        for (const auto& row : g_rows) {
            for (const Slot& t : row) {
                if (t.chara >= 0) picks.push_back(t.chara);
            }
        }
        if (!picks.empty()) g_mock.chara[next() & 1] = picks[next() % picks.size()];
        g_mock.nextAct += 0.8f;
        return;
    }
    if (g_mock.inPhase > 3.0f) g_mock.ready[1] = true;
    if (g_mock.inPhase > 6.0f) g_mock.ready[0] = true;
    const int side = !g_mock.ready[0] && (g_mock.ready[1] || (next() & 1)) ? 0 : 1;
    if (!g_mock.ready[side]) {
        int* v = g_mock.values[side];
        const int row = static_cast<int>(next() % 5);
        const SelectChara& c = g_preview.charas[g_mock.chara[side]];
        v[row] = (v[row] + 1) % MockCount(c, row, v[0]);
        if (row == 0) v[1] = 0;
    }
    g_mock.nextAct += 0.7f;
}

void PreviewReset()
{
    g_mock = Mock{};
    g_ui = Ui{};
    BuildGrid(g_preview);
    g_mock.chara[0] = std::max(0, CharaAt(g_preview, "RYU"));
    g_mock.chara[1] = std::max(0, CharaAt(g_preview, "CNL"));
    // Ring of Destiny when owned, else the first stage that is.
    int first = -1;
    for (size_t i = 0; i < g_preview.stages.size(); i++) {
        if (g_preview.stages[i].locked) continue;
        if (first < 0) first = static_cast<int>(i);
        if (g_preview.stages[i].code == "TAI") g_mock.stage = static_cast<int>(i);
    }
    if (g_mock.stage < 0 || g_mock.stage >= static_cast<int>(g_preview.stages.size()) || g_preview.stages[g_mock.stage].locked) {
        g_mock.stage = std::max(0, first);
    }
    SetMockPhase(g_previewPhase);
}

// The preview's own controls: which phase, and close.
void DrawPreviewTabs(ImDrawList* d, const SelectFonts& f)
{
    const char* const labels[] = {"Stage select", "Character", "Loadout", "Ready", "Close preview"};
    float x = 24;
    const int phase = MockPhase();
    for (int i = 0; i < 5; i++) {
        const float w = TextW(f.text, 15, labels[i]) + 28;
        const bool on = i < 4 && phase == i;
        const ImVec2 a = g_v.P(x, 16), b = g_v.P(x + w, 50);
        d->AddRectFilled(a, b, on ? Rgba(233, 196, 106) : Rgba(0, 0, 0, 150), g_v.L(17));
        d->AddRect(a, b, on ? Rgba(233, 196, 106) : Rgba(93, 90, 82), g_v.L(17));
        Text(d, f.text, g_v.L(15), g_v.P(x + w / 2, 24), on ? Rgba(21, 19, 13) : Rgba(244, 241, 232), labels[i], 0.5f);
        if (Clicked() && MouseIn(a, b)) {
            if (i == 4) g_previewOpen = false;
            else SetMockPhase(i);
        }
        x += w + 8;
    }
}

} // namespace

void SetSelectData(SelectData data)
{
    std::lock_guard lock(g_dataMutex);
    g_data = std::move(data);
    g_gridFor = nullptr;
}

void ResetSelectScreen()
{
    std::lock_guard lock(g_dataMutex);
    g_ui = Ui{};
}

void WarmSelectTextures()
{
    std::lock_guard lock(g_dataMutex);
    if (g_data.charas.empty()) return;
    if (g_gridFor != &g_data) BuildGrid(g_data);
    g_drawing = false;
    struct Restore {
        ~Restore() { g_drawing = true; }
    } restore;
    // The stage select: its frame, icons and prompts, then each stage's icon.
    for (const char* name : {"StagePreviewWindowShadow", "StagePreviewWindow", "StageSelectCursor", "StageSelect_Random"}) {
        Texture(MenuTex(name));
    }
    for (const char* icon : {"button_a", "button_b", "button_y", "button_start"}) Texture(PadTex(icon));
    for (const SelectStage& s : g_data.stages) Texture(StageTex(s.code, "StageSelectIcon"));
    // The character grid's tiles, then each stage's backdrop, then each
    // character's picture (what browsing the grid shows).
    for (const auto& row : g_rows) {
        for (const Slot& t : row) {
            if (t.chara >= 0 && t.chara < static_cast<int>(g_data.charas.size())) {
                Texture(CharaTex(g_data.charas[t.chara].code, t.right ? "CharaSelectIconRight" : "CharaSelectIconLeft"));
            }
        }
    }
    for (const SelectStage& s : g_data.stages) Texture(StageTex(s.code, "StageSelectBG"));
    for (const SelectStage& s : g_data.stages) {
        for (const SelectStage& k : s.settings) {
            Texture(StageTex(k.code, "StageSelectIcon"));
            Texture(StageTex(k.code, "StageSelectBG"));
        }
    }
    for (const SelectChara& c : g_data.charas) Texture(CharaTex(c.code, "PartCharacter"));
}

void WarmBootTextures(const SelectData& data, const std::string& chara, const std::string& stage)
{
    std::vector<std::pair<std::string, uint32_t>> want;
    auto add = [&](std::string path) { want.emplace_back(std::move(path), 0); };
    for (const char* icon : {"button_a", "button_b", "button_x", "button_y", "button_start", "button_back", "button_lb", "button_rb"}) {
        add(PadTex(icon));
    }
    if (!chara.empty()) add(CharaTex(chara, "PartCharacter"));
    if (!stage.empty()) add(StageTex(stage, "StageSelectBG"));
    for (const char* name : {"StagePreviewWindowShadow", "StagePreviewWindow", "StageSelectCursor", "StageSelect_Random"}) add(MenuTex(name));
    // The grid's tiles lean to the side they are on.
    auto onRight = [](const std::string& code) {
        auto in = [&](const auto& side) { return std::any_of(std::begin(side), std::end(side), [&](const char* c) { return code == c; }); };
        return in(kTopRight) || in(kBottomRight);
    };
    for (const SelectChara& c : data.charas) add(CharaTex(c.code, onRight(c.code) ? "CharaSelectIconRight" : "CharaSelectIconLeft"));
    for (const SelectStage& s : data.stages) add(StageTex(s.code, "StageSelectIcon"));
    for (const SelectStage& s : data.stages) add(StageTex(s.code, "StageSelectBG"));
    WarmPakTextures(want);
    Log("select screen: warming %zu textures (favorites %s, %s)", want.size(), chara.empty() ? "-" : chara.c_str(),
        stage.empty() ? "-" : stage.c_str());
}

void DrawSelectLive(const SelectFonts& f, const SelectLive& live, float alpha, bool takeInput, SelectInput* input)
{
    *input = SelectInput{};
    if (!f.title || !f.name || !f.text || alpha <= 0) return;
    std::lock_guard lock(g_dataMutex);
    if (g_data.charas.empty()) return;
    g_draw = &g_data;
    BeginScreen();
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    const int from = d->VtxBuffer.Size;
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(6, 8, 11));
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    g_inputOn = takeInput;
    Render(d, f, live, input);
    g_inputOn = false;
    d->PopClipRect();
    FadeFrom(d, from, alpha);
}

void OpenSelectPreview(SelectData data, int phase)
{
    {
        std::lock_guard lock(g_dataMutex);
        g_preview = std::move(data);
        g_previewPhase = std::clamp(phase, 0, 3);
    }
    g_previewFresh = true;
    g_previewOpen = true;
    Log("select preview: %zu characters, %zu stages%s", g_preview.charas.size(), g_preview.stages.size(),
        g_preview.spectator ? ", watching" : "");
}

bool SelectPreviewOpen() { return g_previewOpen; }

void DrawSelectPreview(const SelectFonts& f)
{
    if (!g_previewOpen || !f.title || !f.name || !f.text) return;
    std::lock_guard lock(g_dataMutex);
    if (g_preview.charas.empty()) {
        g_previewOpen = false;
        return;
    }
    g_draw = &g_preview;
    if (g_previewFresh) {
        g_previewFresh = false;
        PreviewReset();
    }
    if (g_gridFor != &g_preview) BuildGrid(g_preview);
    BeginScreen();
    if (g_preview.spectator) MockTick(ImGui::GetIO().DeltaTime);
    SelectLive live;
    live.you = g_preview.spectator ? -1 : 0;
    live.host = true;
    live.sides[0] = MockSide(0);
    live.sides[1] = MockSide(1);
    live.stage = g_mock.stage;
    live.setting = g_mock.setting;
    live.stageLocked = g_mock.locked;
    live.pingMs = g_preview.spectator ? 120 : 38;
    live.spectators = 3;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(6, 8, 11));
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);
    SelectInput in;
    g_inputOn = true;
    Render(d, f, live, &in);
    DrawPreviewTabs(d, f);
    g_inputOn = false;
    d->PopClipRect();
    // The mock players take what the player did.
    if (in.stage >= 0) g_mock.stage = in.stage, g_mock.setting = 0;
    if (in.setting >= 0) g_mock.setting = in.setting;
    if (in.lockStage) g_mock.locked = true;
    if (in.unlockStage) g_mock.locked = false;
    if (in.chara >= 0) g_mock.chara[0] = in.chara;
    if (in.randomPick) {
        static std::mt19937 rng{std::random_device{}()};
        g_mock.chara[0] = static_cast<int>(rng() % g_preview.charas.size());
        std::fill(std::begin(g_mock.values[0]), std::end(g_mock.values[0]), 0);
        g_mock.values[0][6] = 2;
    }
    if (in.row >= 0) {
        const SelectChara& c = g_preview.charas[g_mock.chara[0]];
        int& v = g_mock.values[0][in.row];
        const int count = MockCount(c, in.row, g_mock.values[0][0]);
        v = (v + in.delta + count) % count;
        if (in.row == 0) g_mock.values[0][1] = 0;
    }
    if (in.ready) g_mock.ready[0] = true;
    if (in.unready) g_mock.ready[0] = false;
    // Leave closes the preview.
    if (in.leave) g_previewOpen = false;
}

} // namespace sfv::mod
