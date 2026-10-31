#include "mod/select_screen.h"

#include "mod/flow.h"
#include "mod/log.h"
#include "mod/textures.h"

#include <imgui.h>
#include <imgui_internal.h> // IM_PI, the draw list's white pixel

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <set>

namespace sfv::mod {
namespace {

// ----- the game's textures --------------------------------------------------

// Loaded on the game thread on first use; the render thread draws the views
// once they are there (a missing one leaves its place dark).
std::mutex g_texMutex;
std::map<std::string, TextureView> g_textures;
std::set<std::string> g_requested;

const TextureView* Texture(const std::string& path)
{
    {
        std::lock_guard lock(g_texMutex);
        const auto it = g_textures.find(path);
        if (it != g_textures.end()) return it->second.srv ? &it->second : nullptr;
        if (!g_requested.insert(path).second) return nullptr;
    }
    RunOnGameThread([path] {
        const size_t slash = path.rfind('/');
        const std::string name = path.substr(slash + 1);
        const TextureView view = ViewOfTexture(FindOrLoadTexture(path + "." + name), true);
        std::lock_guard lock(g_texMutex);
        g_textures[path] = view;
    });
    return nullptr;
}

std::string CharaTex(const std::string& code, const char* name) { return "/Game/Chara/" + code + "/Hud/" + name; }
std::string StageTex(const std::string& code, const char* name) { return "/Game/Stage/" + code + "/UI/" + name; }
std::string MenuTex(const char* name) { return std::string("/Game/MenuAsset/CharaSelectGFx/") + name; }
std::string PadTex(const char* name) { return std::string("/Game/CommonAsset/PlatformAsset/Windows/padkey/") + name; }

// ----- the screen's state ---------------------------------------------------

enum class Phase { Stage, Grid, Loadout, Ready };

std::mutex g_dataMutex;
SelectData g_data;
std::atomic<bool> g_open{false};
bool g_fresh = false; // opened since the last frame: reset the state
int g_startPhase = 0;

struct State {
    Phase phase = Phase::Stage;
    int stage = 0;             // into the stages; == count: random
    int p1 = 0, p2 = 0;        // into the characters
    int cursorRow = 0, cursorX = 0; // the grid cursor (row, slot from the left)
    int row = 0;               // the loadout row
    int value[6] = {0, 0, 0, 0, 0, 2}; // costume, color, V-Skill, V-Trigger, V-Comment, delay
    bool randomOn = false;
} g_s;

constexpr int kRows = 6;
const char* const kRowLabels[kRows] = {"Costume", "Color", "V-Skill", "V-Trigger", "V-Comment", "Input delay"};
const int kMockP2Values[kRows] = {1, 0, 1, 0, 0, 3};

// The game's grid, from the middle outwards (the final roster's screen):
// top row 11 a side, bottom row 12 a side.
const char* const kTopLeft[] = {"RYU", "CNL", "RMK", "RSD", "NSH", "Z21", "Z23", "Z28", "Z32", "Z36", "Z39"};
const char* const kTopRight[] = {"KEN", "CMY", "BLR", "NCL", "GUL", "Z20", "Z25", "Z29", "Z33", "Z37", "Z41"};
const char* const kBottomLeft[] = {"ALX", "KRN", "ZGF", "LAR", "IBK", "JRI", "Z22", "Z26", "Z30", "Z34", "Z38", "Z42"};
const char* const kBottomRight[] = {"VEG", "BRD", "DSM", "FAN", "BSN", "URN", "Z24", "Z27", "Z31", "Z35", "Z40", "ELV"};

// Design coordinates: 1600x900, fitted to the screen.
struct View {
    float s = 1, ox = 0, oy = 0;
    ImVec2 P(float x, float y) const { return ImVec2(ox + x * s, oy + y * s); }
    float L(float v) const { return v * s; }
} g_v;

int CharaAt(const std::string& code)
{
    for (size_t i = 0; i < g_data.charas.size(); i++) {
        if (g_data.charas[i].code == code) return static_cast<int>(i);
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

constexpr float kC = 800, kY0 = 606, kTH = 112, kY1 = 726, kSlant = 14, kTopW = 50, kPitch = 58, kHalf = 48;
constexpr float kK = kSlant / kTH;

void BuildGrid()
{
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
            t.chara = CharaAt(codes[j]);
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
    half(g_rows[1], kBottomLeft, std::size(kBottomLeft), 1, false);
    half(g_rows[1], kBottomRight, std::size(kBottomRight), 1, true);
}

void Reset()
{
    g_s = State{};
    BuildGrid();
    g_s.p1 = std::max(0, CharaAt("RYU"));
    g_s.p2 = std::max(0, CharaAt("CNL"));
    for (size_t x = 0; x < g_rows[0].size(); x++) {
        if (g_rows[0][x].chara == g_s.p1) g_s.cursorX = static_cast<int>(x);
    }
    for (size_t i = 0; i < g_data.stages.size(); i++) {
        if (g_data.stages[i].code == "TAI") g_s.stage = static_cast<int>(i);
    }
}

// ----- input ----------------------------------------------------------------

bool Pressed(std::initializer_list<ImGuiKey> keys)
{
    for (ImGuiKey k : keys) {
        if (ImGui::IsKeyPressed(k, true)) return true;
    }
    return false;
}
bool Left() { return Pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}); }
bool Right() { return Pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}); }
bool Up() { return Pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp}); }
bool Down() { return Pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}); }
bool Confirm() { return Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown}); }
bool Back() { return Pressed({ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight}); }

bool Clicked() { return ImGui::IsMouseClicked(ImGuiMouseButton_Left); }
bool MouseIn(ImVec2 a, ImVec2 b)
{
    const ImVec2 m = ImGui::GetIO().MousePos;
    return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
}
bool MouseInQuad(const ImVec2 (&q)[4])
{
    const ImVec2 m = ImGui::GetIO().MousePos;
    bool inside = false;
    for (int i = 0, j = 3; i < 4; j = i++) {
        const ImVec2 a = g_v.P(q[i].x, q[i].y), b = g_v.P(q[j].x, q[j].y);
        if ((a.y > m.y) != (b.y > m.y) && m.x < (b.x - a.x) * (m.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

// ----- drawing helpers --------------------------------------------------------

ImU32 Rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImU32 FromArgb(uint32_t argb) { return IM_COL32((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, 255); }

void Text(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 c, const std::string& t, float align = 0, bool shadow = false)
{
    const ImVec2 w = f->CalcTextSizeA(size, FLT_MAX, 0, t.c_str());
    const ImVec2 p(at.x - w.x * align, at.y);
    if (shadow) d->AddText(f, size, ImVec2(p.x, p.y + size * 0.05f), Rgba(0, 0, 0, 220), t.c_str());
    d->AddText(f, size, p, c, t.c_str());
}

// A quad with a color per edge (top two corners one color, bottom two another).
void GradientQuad(ImDrawList* d, const ImVec2 (&q)[4], ImU32 top, ImU32 bottom)
{
    const ImVec2 uv = d->_Data->TexUvWhitePixel;
    d->PrimReserve(6, 4);
    const ImDrawIdx base = static_cast<ImDrawIdx>(d->_VtxCurrentIdx);
    d->PrimWriteIdx(base);
    d->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
    d->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    d->PrimWriteIdx(base);
    d->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
    d->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
    d->PrimWriteVtx(q[0], uv, top);
    d->PrimWriteVtx(q[1], uv, top);
    d->PrimWriteVtx(q[2], uv, bottom);
    d->PrimWriteVtx(q[3], uv, bottom);
}

ImTextureID Id(const TextureView* t) { return reinterpret_cast<ImTextureID>(t->srv); }

void Image(ImDrawList* d, const TextureView* t, ImVec2 a, ImVec2 b, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1), ImU32 tint = IM_COL32_WHITE)
{
    if (t) d->AddImage(Id(t), a, b, uv0, uv1, tint);
}

// The timer's infinity (a lemniscate; the fonts may not have the glyph).
void Infinity(ImDrawList* d, ImVec2 c, float w, ImU32 col)
{
    for (int i = 0; i <= 48; i++) {
        const float t = IM_PI * 2 * i / 48;
        const float k = 1 + std::sin(t) * std::sin(t);
        d->PathLineTo(ImVec2(c.x + w / 2 * std::cos(t) / k, c.y + w / 2 * std::sin(t) * std::cos(t) / k));
    }
    d->PathStroke(col, ImDrawFlags_Closed, w * 0.11f);
}

void Pill(ImDrawList* d, ImVec2 a, ImVec2 b) { d->AddRectFilled(a, b, Rgba(0, 0, 0, 160), (b.y - a.y) / 2); }

void IconWired(ImDrawList* d, ImVec2 c, float s, ImU32 col)
{
    d->AddRect(ImVec2(c.x - 5 * s, c.y - 3 * s), ImVec2(c.x + 5 * s, c.y + 3 * s), col, 0, 0, 1.4f * s);
    d->AddRect(ImVec2(c.x - 2.5f * s, c.y + 3 * s), ImVec2(c.x + 2.5f * s, c.y + 5 * s), col, 0, 0, 1.4f * s);
    for (int i = -1; i <= 1; i++) d->AddLine(ImVec2(c.x + i * 2.2f * s, c.y - 3 * s), ImVec2(c.x + i * 2.2f * s, c.y - 0.5f * s), col, 1.2f * s);
}
void IconWifi(ImDrawList* d, ImVec2 c, float s, ImU32 col)
{
    for (int i = 1; i <= 3; i++) {
        d->PathArcTo(ImVec2(c.x, c.y + 4 * s), 2.6f * i * s, -IM_PI * 0.78f, -IM_PI * 0.22f, 10);
        d->PathStroke(col, 0, 1.6f * s);
    }
}
void IconLink(ImDrawList* d, ImVec2 c, float s, ImU32 col)
{
    d->AddRect(ImVec2(c.x - 7 * s, c.y - 3 * s), ImVec2(c.x - 1 * s, c.y + 3 * s), col, 3 * s, 0, 1.5f * s);
    d->AddRect(ImVec2(c.x + 1 * s, c.y - 3 * s), ImVec2(c.x + 7 * s, c.y + 3 * s), col, 3 * s, 0, 1.5f * s);
    d->AddLine(ImVec2(c.x - 3 * s, c.y), ImVec2(c.x + 3 * s, c.y), col, 1.5f * s);
}

// ----- the parts ----------------------------------------------------------------

const SelectChara& Chara(int i) { return g_data.charas[std::clamp(i, 0, static_cast<int>(g_data.charas.size()) - 1)]; }
bool IsEleven(int i) { return Chara(i).code == "ELV"; }

int RowCount(const SelectChara& c, int row, int costume)
{
    switch (row) {
    case 0: return std::max<int>(1, static_cast<int>(c.costumes.size()));
    case 1: {
        const size_t k = std::min<size_t>(costume, c.swatches.empty() ? 0 : c.swatches.size() - 1);
        return c.swatches.empty() || c.swatches[k].empty() ? 1 : static_cast<int>(c.swatches[k].size());
    }
    case 2: return std::max<int>(2, static_cast<int>(c.vskills.size()));
    case 3: return std::max<int>(2, static_cast<int>(c.vtriggers.size()));
    case 4: return 1 + static_cast<int>(c.vcomments.size());
    default: return 9;
    }
}

std::string RowValue(const SelectChara& c, int row, const int (&v)[kRows], uint32_t* sw1, uint32_t* sw2)
{
    *sw1 = *sw2 = 0;
    auto roman = [](int n) { return n == 1 ? std::string("I") : n == 2 ? std::string("II") : n == 3 ? std::string("III") : std::to_string(n); };
    if (c.code == "ELV" && row < 5) return "Random";
    switch (row) {
    case 0: return c.costumes.empty() ? "Default" : c.costumes[v[0] % c.costumes.size()];
    case 1: {
        const int count = RowCount(c, 1, v[0]);
        const int k = v[1] % count;
        const size_t ci = std::min<size_t>(v[0], c.swatches.empty() ? 0 : c.swatches.size() - 1);
        if (!c.swatches.empty() && !c.swatches[ci].empty()) {
            *sw1 = c.swatches[ci][k].first;
            *sw2 = c.swatches[ci][k].second;
        }
        // The colors, then the EX colors (the preview counts 15 or so of the first).
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d", k + 1);
        return buf;
    }
    case 2:
    case 3: {
        const auto& names = row == 2 ? c.vskills : c.vtriggers;
        const int n = v[row] % std::max<int>(2, static_cast<int>(names.size()));
        return roman(n + 1) + (n < static_cast<int>(names.size()) && !names[n].empty() ? "  " + names[n] : "");
    }
    case 4: {
        // A number: the text rarely fits, and the strip under the rows shows it whole.
        const int n = v[4] % (1 + static_cast<int>(c.vcomments.size()));
        return n == 0 ? "AUTO" : std::to_string(n);
    }
    default: return std::to_string(v[5]) + (v[5] == 1 ? " frame" : " frames");
    }
}

const char* RowHint(int row)
{
    static const char* const hints[kRows] = {"Pick a costume.", "Pick a color.", "V-Skill.", "V-Trigger.", "",
                                             "Suggested: 2 frames for a 38 ms ping. Lower reacts sooner, with more rollbacks."};
    return hints[row];
}

std::string CommentLine(const SelectChara& c, int comment)
{
    if (c.code == "ELV") return "A random fighter in silver, its V-Skill and V-Trigger at random.";
    const int n = comment % (1 + static_cast<int>(c.vcomments.size()));
    return n == 0 ? "Comment changes automatically based on opponent's character." : "\"" + c.vcomments[n - 1] + "\"";
}

void DrawLoadout(ImDrawList* d, const SelectFonts& f, bool right, int chara, const int (&values)[kRows], bool interactive, bool ready)
{
    const SelectChara& c = Chara(chara);
    const float x0 = right ? 980.0f : 0.0f, top = 108, rowH = 40, gap = 4, w = 620;
    ImFont* font = f.text;
    for (int r = 0; r < kRows; r++) {
        const float y = top + r * (rowH + gap);
        const ImVec2 a = g_v.P(x0, y), b = g_v.P(x0 + w, y + rowH);
        const ImU32 dark = Rgba(8, 9, 11, 235), fade = Rgba(8, 9, 11, 200);
        d->AddRectFilledMultiColor(a, b, right ? fade : dark, right ? dark : fade, right ? dark : fade, right ? fade : dark);
        if (interactive && g_s.row == r && g_s.phase == Phase::Loadout) {
            const ImVec2 gb = g_v.P(x0 + (right ? w - 280 : 280), y + rowH);
            const ImU32 gold = Rgba(240, 200, 90, 140), none = Rgba(240, 200, 90, 0);
            if (right) d->AddRectFilledMultiColor(ImVec2(gb.x, a.y), b, none, gold, gold, none);
            else d->AddRectFilledMultiColor(a, gb, gold, none, none, gold);
        }
        const ImU32 rule = Rgba(201, 168, 92, 190);
        d->AddLine(a, ImVec2(b.x, a.y), rule, std::max(1.0f, g_v.L(1)));
        d->AddLine(ImVec2(a.x, b.y), b, rule, std::max(1.0f, g_v.L(1)));
        Text(d, font, g_v.L(19), g_v.P(x0 + 44, y + 9), Rgba(244, 241, 232), kRowLabels[r]);
        uint32_t s1 = 0, s2 = 0;
        const std::string value = RowValue(c, r, values, &s1, &s2);
        const float mid = x0 + 190 + (w - 190) / 2;
        float tw = font->CalcTextSizeA(g_v.L(18), FLT_MAX, 0, value.c_str()).x / g_v.s;
        const float sw = s1 ? 46.0f : 0.0f;
        const float tx = mid - (tw + sw) / 2;
        Text(d, font, g_v.L(18), g_v.P(tx, y + 10), Rgba(244, 241, 232), value);
        if (s1) {
            const float sx = tx + tw + 10;
            d->AddRectFilled(g_v.P(sx, y + 11), g_v.P(sx + 18, y + 29), FromArgb(s1));
            d->AddRect(g_v.P(sx, y + 11), g_v.P(sx + 18, y + 29), Rgba(0, 0, 0));
            d->AddRectFilled(g_v.P(sx + 21, y + 11), g_v.P(sx + 39, y + 29), FromArgb(s2));
            d->AddRect(g_v.P(sx + 21, y + 11), g_v.P(sx + 39, y + 29), Rgba(0, 0, 0));
        }
        if (interactive && g_s.phase == Phase::Loadout && !(c.code == "ELV" && r < 5)) {
            const ImU32 arrow = Rgba(233, 196, 106);
            Text(d, font, g_v.L(15), g_v.P(x0 + 212, y + 11), arrow, "<", 0.5f);
            Text(d, font, g_v.L(15), g_v.P(x0 + w - 22, y + 11), arrow, ">", 0.5f);
            const int count = RowCount(c, r, values[0]);
            auto step = [&](int delta) {
                g_s.row = r;
                g_s.value[r] = (g_s.value[r] + delta + count) % count;
                if (r == 0) g_s.value[1] = 0;
            };
            if (Clicked() && MouseIn(g_v.P(x0 + 190, y), g_v.P(x0 + 234, y + rowH))) step(-1);
            else if (Clicked() && MouseIn(g_v.P(x0 + w - 44, y), g_v.P(x0 + w, y + rowH))) step(1);
            else if (Clicked() && MouseIn(a, b)) g_s.row = r;
        }
    }
    // The strip under the rows: the row's description (or the comment), gold
    // READY once that side is ready.
    const float y = top + kRows * (rowH + gap);
    const ImVec2 a = g_v.P(x0, y), b = g_v.P(x0 + w, y + 30);
    if (ready) {
        const ImU32 g1 = Rgba(217, 166, 50), g2 = Rgba(150, 108, 28, 230);
        d->AddRectFilledMultiColor(a, b, right ? g2 : g1, right ? g1 : g2, right ? g1 : g2, right ? g2 : g1);
        Text(d, f.title, g_v.L(20), g_v.P(x0 + 44, y + 4), Rgba(26, 20, 5), "READY");
    }
    else {
        d->AddRectFilled(a, b, Rgba(8, 9, 11, 185));
        const std::string hint = interactive && c.code != "ELV" ? (g_s.row == 4 ? CommentLine(c, values[4]) : RowHint(g_s.row)) : CommentLine(c, values[4]);
        Text(d, font, g_v.L(14), g_v.P(x0 + 44, y + 7), Rgba(217, 212, 198), hint);
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
    if (t.chara >= 0) {
        const std::string& code = Chara(t.chara).code;
        if (const TextureView* tex = Texture(CharaTex(code, t.right ? "CharaSelectIconRight" : "CharaSelectIconLeft"))) {
            // The 116x144 art at the inner height, centered across.
            const float top = iq[0].y, h = iq[3].y - iq[0].y;
            const float drawnW = h * 116.0f / 144.0f;
            const float cx = (std::min(iq[0].x, iq[3].x) + std::max(iq[1].x, iq[2].x)) / 2;
            auto uv = [&](ImVec2 p) { return ImVec2((p.x - (cx - drawnW / 2)) / drawnW, (p.y - top) / h); };
            d->AddImageQuad(Id(tex), iq[0], iq[1], iq[2], iq[3], uv(iq[0]), uv(iq[1]), uv(iq[2]), uv(iq[3]),
                            on ? IM_COL32_WHITE : Rgba(205, 205, 205));
        }
    }
    // The bottom fade.
    const float mid = 0.55f;
    ImVec2 lower[4] = {ImVec2(iq[0].x + (iq[3].x - iq[0].x) * mid, iq[0].y + (iq[3].y - iq[0].y) * mid),
                       ImVec2(iq[1].x + (iq[2].x - iq[1].x) * mid, iq[1].y + (iq[2].y - iq[1].y) * mid), iq[2], iq[3]};
    GradientQuad(d, lower, Rgba(0, 0, 0, 0), Rgba(0, 0, 0, on ? 40 : 150));
    if (p1) {
        const ImVec2 at(iq[0].x + g_v.L(t.right ? 13.0f : 2.0f), iq[0].y + g_v.L(2));
        d->AddRectFilled(at, ImVec2(at.x + g_v.L(20), at.y + g_v.L(15)), Rgba(47, 111, 228));
        Text(d, f.text, g_v.L(12), ImVec2(at.x + g_v.L(10), at.y + g_v.L(1)), Rgba(255, 255, 255), "1P", 0.5f);
    }
    if (p2) {
        const ImVec2 at(iq[2].x - g_v.L(t.right ? 13.0f : 2.0f), iq[2].y - g_v.L(2));
        d->AddRectFilled(ImVec2(at.x - g_v.L(20), at.y - g_v.L(15)), at, Rgba(210, 59, 49));
        Text(d, f.text, g_v.L(12), ImVec2(at.x - g_v.L(10), at.y - g_v.L(14)), Rgba(255, 255, 255), "2P", 0.5f);
    }
}

void DrawGrid(ImDrawList* d, const SelectFonts& f)
{
    for (int r = 0; r < 2; r++) {
        for (int x = 0; x < static_cast<int>(g_rows[r].size()); x++) {
            const Slot& t = g_rows[r][x];
            const bool cursor = g_s.phase == Phase::Grid && g_s.cursorRow == r && g_s.cursorX == x;
            const bool p1 = t.random ? cursor || (g_s.randomOn && g_s.phase != Phase::Grid) : t.chara == g_s.p1 && !g_s.randomOn;
            const bool p2 = !t.random && t.chara == g_s.p2;
            DrawTile(d, t, p1, p2, f);
            if (g_s.phase == Phase::Grid && Clicked() && MouseInQuad(t.q)) {
                g_s.cursorRow = r;
                g_s.cursorX = x;
                if (t.random) g_s.randomOn = true;
                else if (t.chara >= 0) {
                    g_s.p1 = t.chara;
                    g_s.randomOn = false;
                }
            }
        }
    }
}

// The name, and over it the player's line: name, link pill.
void DrawNames(ImDrawList* d, const SelectFonts& f)
{
    const float y = 470;
    const ImU32 white = Rgba(244, 241, 232), dim = Rgba(168, 162, 147);
    // You: name (you), connection type.
    std::string you = g_data.you.empty() ? "YOU" : g_data.you;
    for (char& ch : you) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    Text(d, f.text, g_v.L(16), g_v.P(120, y + 4), white, you);
    float x = 120 + f.text->CalcTextSizeA(g_v.L(16), FLT_MAX, 0, you.c_str()).x / g_v.s + 10;
    Text(d, f.text, g_v.L(16), g_v.P(x, y + 4), dim, "(you)");
    x += f.text->CalcTextSizeA(g_v.L(16), FLT_MAX, 0, "(you)").x / g_v.s + 10;
    Pill(d, g_v.P(x, y), g_v.P(x + 34, y + 26));
    IconWired(d, g_v.P(x + 17, y + 13), g_v.s, Rgba(217, 212, 198));
    // The other player: one pill (connection type | link path | ping), name.
    const std::string them = "MOCK OPPONENT";
    const float nameW = f.text->CalcTextSizeA(g_v.L(16), FLT_MAX, 0, them.c_str()).x / g_v.s;
    Text(d, f.text, g_v.L(16), g_v.P(1480, y + 4), white, them, 1);
    const char* ping = "38 ms \xC2\xB1" "3";
    const float pingW = f.text->CalcTextSizeA(g_v.L(15), FLT_MAX, 0, ping).x / g_v.s;
    const float pw = 22 + 1 + 22 + 1 + 12 + pingW + 20;
    const float px = 1480 - nameW - 12 - pw;
    Pill(d, g_v.P(px, y), g_v.P(px + pw, y + 26));
    IconWifi(d, g_v.P(px + 18, y + 12), g_v.s, Rgba(217, 212, 198));
    d->AddLine(g_v.P(px + 32, y + 6), g_v.P(px + 32, y + 20), Rgba(93, 90, 82), g_v.L(1));
    IconLink(d, g_v.P(px + 48, y + 13), g_v.s, Rgba(217, 212, 198));
    d->AddLine(g_v.P(px + 63, y + 6), g_v.P(px + 63, y + 20), Rgba(93, 90, 82), g_v.L(1));
    Text(d, f.text, g_v.L(15), g_v.P(px + 72, y + 5), Rgba(127, 216, 139), ping);
    // The characters' names.
    auto upper = [](std::string v) {
        for (char& ch : v) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return v;
    };
    const std::string n1 = g_s.randomOn && g_s.phase == Phase::Grid ? "RANDOM" : upper(Chara(g_s.p1).name);
    Text(d, f.name, g_v.L(74), g_v.P(118, y + 26), Rgba(255, 255, 255), n1, 0, true);
    Text(d, f.name, g_v.L(74), g_v.P(1482, y + 26), Rgba(255, 255, 255), upper(Chara(g_s.p2).name), 1, true);
}

void DrawRender(ImDrawList* d, int chara, bool right)
{
    const TextureView* t = Texture(CharaTex(Chara(chara).code, "PartCharacter"));
    if (!t) return;
    if (right) Image(d, t, g_v.P(1600 + 60 - 780, -70), g_v.P(1660, 710), ImVec2(1, 0), ImVec2(0, 1));
    else Image(d, t, g_v.P(-60, -70), g_v.P(720, 710));
}

// The stage select: the stage's picture inside the game's curved frame, its
// name, and the strip of stages.
void DrawStageSelect(ImDrawList* d, const SelectFonts& f)
{
    const int count = static_cast<int>(g_data.stages.size());
    if (Left()) g_s.stage = (g_s.stage + count) % (count + 1);
    if (Right()) g_s.stage = (g_s.stage + 1) % (count + 1);
    Text(d, f.title, g_v.L(40), g_v.P(800, 84), Rgba(255, 255, 255), "STAGE SELECT", 0.5f, true);
    const float bx = 40, by = 136, bw = 1520, bh = 423;
    if (const TextureView* shadow = Texture(MenuTex("StagePreviewWindowShadow"))) Image(d, shadow, g_v.P(bx, by), g_v.P(bx + bw, by + bh));
    const bool random = g_s.stage >= count;
    const std::string code = random || count == 0 ? "" : g_data.stages[g_s.stage].code;
    if (const TextureView* bg = code.empty() ? nullptr : Texture(StageTex(code, "StageSelectBG"))) {
        // The mask's curves (StagePreviewBG): 2 px from the edge at the sides,
        // 43 px at the middle (of 512), top and bottom alike. Slices follow them.
        const float my = by + 9, mh = 405;
        const float u0 = 0.031f, u1 = 0.969f; // cover: the picture is wider than the window
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
            d->AddImageQuad(Id(bg), q0, q1, q2, q3, ImVec2(ua, a0 / mh), ImVec2(ub, a1 / mh), ImVec2(ub, 1 - a1 / mh), ImVec2(ua, 1 - a0 / mh));
        }
    }
    if (const TextureView* frame = Texture(MenuTex("StagePreviewWindow"))) Image(d, frame, g_v.P(bx, by), g_v.P(bx + bw, by + bh));
    const std::string name = random ? "Random" : count ? g_data.stages[g_s.stage].name : "";
    Text(d, f.name, g_v.L(46), g_v.P(800, by + bh - 106), Rgba(255, 255, 255), name, 0.5f, true);
    if (random) Text(d, f.text, g_v.L(17), g_v.P(800, by + bh - 50), Rgba(232, 228, 216), "Any stage, picked when the match starts", 0.5f, true);
    Infinity(d, g_v.P(800, by + bh + 30), g_v.L(56), Rgba(242, 201, 76));
    // The strip: each stage's thin picture, then random.
    const float sw = 29, sh = 168, gap = 3;
    const float total = (count + 1) * sw + count * gap;
    const float sx = 800 - total / 2, sy = by + bh + 64;
    for (int i = 0; i <= count; i++) {
        const bool on = i == g_s.stage;
        const float x = sx + i * (sw + gap);
        const float lift = on ? 8.0f : 0.0f, grow = on ? 1.12f : 1.0f;
        const float cx = x + sw / 2, cy = sy + sh / 2 - lift;
        const ImVec2 a = g_v.P(cx - sw * grow / 2, cy - sh * grow / 2), b = g_v.P(cx + sw * grow / 2, cy + sh * grow / 2);
        const TextureView* t = i < count ? Texture(StageTex(g_data.stages[i].code, "StageSelectIcon")) : Texture(MenuTex("StageSelect_Random"));
        if (t) Image(d, t, a, b);
        else d->AddRectFilled(a, b, Rgba(30, 32, 36));
        if (on) {
            if (const TextureView* cursor = Texture(MenuTex("StageSelectCursor"))) Image(d, cursor, ImVec2(a.x - g_v.L(1), a.y - g_v.L(1)), ImVec2(b.x + g_v.L(1), b.y + g_v.L(1)));
        }
        if (Clicked() && MouseIn(g_v.P(x, sy), g_v.P(x + sw, sy + sh))) g_s.stage = i;
    }
    Text(d, f.text, g_v.L(16), g_v.P(800, sy + sh + 18), Rgba(189, 184, 170), "You host: you pick the stage, then both players pick characters", 0.5f);
}

void MoveCursor(int dr, int dx)
{
    if (dx) {
        const int n = static_cast<int>(g_rows[g_s.cursorRow].size());
        g_s.cursorX = (g_s.cursorX + dx + n) % n;
    }
    if (dr) {
        const ImVec2* q = g_rows[g_s.cursorRow][g_s.cursorX].q;
        const float x = (q[0].x + q[2].x) / 2;
        const int to = 1 - g_s.cursorRow;
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
        g_s.cursorRow = to;
        g_s.cursorX = best;
    }
    const Slot& t = g_rows[g_s.cursorRow][g_s.cursorX];
    g_s.randomOn = t.random;
    if (!t.random && t.chara >= 0) g_s.p1 = t.chara;
}

void Advance()
{
    switch (g_s.phase) {
    case Phase::Stage: g_s.phase = Phase::Grid; break;
    case Phase::Grid:
        if (g_s.randomOn) {
            // Random: any character but the tile itself.
            static std::mt19937 rng{std::random_device{}()};
            g_s.p1 = static_cast<int>(rng() % g_data.charas.size());
        }
        std::fill(std::begin(g_s.value), std::end(g_s.value), 0);
        g_s.value[5] = 2;
        g_s.row = 0;
        g_s.phase = Phase::Loadout;
        break;
    case Phase::Loadout: g_s.phase = Phase::Ready; break;
    case Phase::Ready: break;
    }
}

void Retreat()
{
    switch (g_s.phase) {
    case Phase::Stage: g_open = false; break;
    case Phase::Grid: g_s.phase = Phase::Stage; break;
    case Phase::Loadout: g_s.phase = Phase::Grid; break;
    case Phase::Ready: g_s.phase = Phase::Loadout; break;
    }
}

// The preview's own controls: which phase, and close; the A/B prompts with
// the game's pad icons.
void DrawChrome(ImDrawList* d, const SelectFonts& f)
{
    const char* const labels[] = {"Stage select", "Character", "Loadout", "Ready", "Close preview"};
    float x = 24;
    for (int i = 0; i < 5; i++) {
        const float w = f.text->CalcTextSizeA(g_v.L(15), FLT_MAX, 0, labels[i]).x / g_v.s + 28;
        const bool on = i < 4 && static_cast<int>(g_s.phase) == i;
        const ImVec2 a = g_v.P(x, 16), b = g_v.P(x + w, 50);
        d->AddRectFilled(a, b, on ? Rgba(233, 196, 106) : Rgba(0, 0, 0, 150), g_v.L(17));
        d->AddRect(a, b, on ? Rgba(233, 196, 106) : Rgba(93, 90, 82), g_v.L(17));
        Text(d, f.text, g_v.L(15), g_v.P(x + w / 2, 24), on ? Rgba(21, 19, 13) : Rgba(244, 241, 232), labels[i], 0.5f);
        if (Clicked() && MouseIn(a, b)) {
            if (i == 4) g_open = false;
            else g_s.phase = static_cast<Phase>(i);
        }
        x += w + 8;
    }
    const char* confirm = g_s.phase == Phase::Stage ? "Confirm stage" : g_s.phase == Phase::Grid ? "Select" : "Ready";
    struct Prompt {
        const char* icon;
        const char* label;
    } prompts[] = {{"button_a", confirm}, {"button_b", "Back"}};
    float right = 1572;
    for (int i = 1; i >= 0; i--) {
        const float w = f.text->CalcTextSizeA(g_v.L(17), FLT_MAX, 0, prompts[i].label).x / g_v.s + 50;
        const ImVec2 a = g_v.P(right - w, 14), b = g_v.P(right, 56);
        d->AddRectFilled(a, b, Rgba(0, 0, 0, 150), g_v.L(21));
        if (const TextureView* icon = Texture(PadTex(prompts[i].icon))) Image(d, icon, g_v.P(right - w + 5, 19), g_v.P(right - w + 37, 51));
        Text(d, f.text, g_v.L(17), g_v.P(right - w + 42, 24), Rgba(244, 241, 232), prompts[i].label);
        if (Clicked() && MouseIn(a, b)) {
            if (i == 0) Advance();
            else Retreat();
        }
        right -= w + 18;
    }
}

} // namespace

void OpenSelectPreview(SelectData data, int phase)
{
    g_startPhase = std::clamp(phase, 0, 3);
    {
        std::lock_guard lock(g_dataMutex);
        g_data = std::move(data);
    }
    g_fresh = true;
    g_open = true;
    Log("select preview: %zu characters, %zu stages", g_data.charas.size(), g_data.stages.size());
}

bool SelectPreviewOpen() { return g_open; }

void DrawSelectPreview(const SelectFonts& f)
{
    if (!g_open || !f.title || !f.name || !f.text) return;
    std::lock_guard lock(g_dataMutex);
    if (g_data.charas.empty()) {
        g_open = false;
        return;
    }
    if (g_fresh) {
        g_fresh = false;
        Reset();
        g_s.phase = static_cast<Phase>(g_startPhase);
    }
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    g_v.s = std::min(screen.x / 1600.0f, screen.y / 900.0f);
    g_v.ox = (screen.x - 1600 * g_v.s) / 2;
    g_v.oy = (screen.y - 900 * g_v.s) / 2;
    ImDrawList* d = ImGui::GetBackgroundDrawList();
    d->AddRectFilled(ImVec2(0, 0), screen, Rgba(6, 8, 11));
    d->PushClipRect(g_v.P(0, 0), g_v.P(1600, 900), true);

    // Keys: A on, B back; the phase's own moves.
    if (Back()) Retreat();
    else if (Confirm()) Advance();
    else if (g_s.phase == Phase::Grid) {
        if (Left()) MoveCursor(0, -1);
        if (Right()) MoveCursor(0, 1);
        if (Up() || Down()) MoveCursor(1, 0);
    }
    else if (g_s.phase == Phase::Loadout) {
        if (Up()) g_s.row = (g_s.row + kRows - 1) % kRows;
        if (Down()) g_s.row = (g_s.row + 1) % kRows;
        const int count = RowCount(Chara(g_s.p1), g_s.row, g_s.value[0]);
        if (!(IsEleven(g_s.p1) && g_s.row < 5)) {
            if (Left()) g_s.value[g_s.row] = (g_s.value[g_s.row] + count - 1) % count;
            if (Right()) g_s.value[g_s.row] = (g_s.value[g_s.row] + 1) % count;
        }
        if (g_s.row == 0 && (Left() || Right())) g_s.value[1] = 0;
    }

    if (g_s.phase == Phase::Stage) {
        DrawStageSelect(d, f);
    }
    else {
        // The chosen stage, dark and wide behind everything.
        const int count = static_cast<int>(g_data.stages.size());
        const std::string code = count == 0 ? "" : g_data.stages[g_s.stage < count ? g_s.stage : 0].code;
        if (const TextureView* bg = code.empty() ? nullptr : Texture(StageTex(code, "StageSelectBG"))) {
            Image(d, bg, g_v.P(800 - 1240, -20), g_v.P(800 + 1240, 600), ImVec2(0, 0), ImVec2(1, 1), Rgba(118, 116, 112));
        }
        d->AddRectFilledMultiColor(g_v.P(0, 0), g_v.P(1600, 300), Rgba(6, 8, 11, 120), Rgba(6, 8, 11, 120), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0));
        DrawRender(d, g_s.p1, false);
        DrawRender(d, g_s.p2, true);
        d->AddRectFilledMultiColor(g_v.P(0, 500), g_v.P(1600, 640), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 0), Rgba(6, 8, 11, 230), Rgba(6, 8, 11, 230));
        d->AddRectFilled(g_v.P(0, 640), g_v.P(1600, 900), Rgba(6, 8, 11, 240));
        if (g_s.phase != Phase::Grid) {
            DrawLoadout(d, f, false, g_s.p1, g_s.value, true, g_s.phase == Phase::Ready);
            DrawLoadout(d, f, true, g_s.p2, kMockP2Values, false, true);
        }
        DrawNames(d, f);
        Text(d, f.title, g_v.L(38), g_v.P(800, 506), Rgba(255, 255, 255), "CHARACTER SELECT", 0.5f, true);
        Infinity(d, g_v.P(800, 578), g_v.L(56), Rgba(242, 201, 76));
        DrawGrid(d, f);
    }
    DrawChrome(d, f);
    d->PopClipRect();
}

} // namespace sfv::mod
