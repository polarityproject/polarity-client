#include "mod/select_screen.h"

#include "mod/flow.h"
#include "mod/log.h"
#include "mod/textures.h"

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

// ----- the game's textures --------------------------------------------------

// Loaded on the game thread on first use; the render thread draws the views
// once they are there (a missing one leaves its place dark).
std::mutex g_texMutex;
std::map<std::string, TextureView> g_textures;
std::set<std::string> g_requested;
std::deque<std::string> g_texQueue; // asked for, not loaded yet (in order)
std::set<std::string> g_texDrawn;   // asked for again by drawing: moved to the queue's front once
bool g_texLoading = false;          // LoadTextures is on its way
// A texture loaded without its view yet (the render thread had not made its
// resource): asked for again a little later, a few times. Once failed it
// stayed blank for the whole run (Gill's grid icon, once). A view of a few
// pixels is the engine's stand-in until then (white): kept, the stage
// frame's shadow showed as a white box behind "Random" on one PC.
struct TexRetry {
    std::string path;
    std::chrono::steady_clock::time_point due;
};
std::vector<TexRetry> g_texRetries;
std::map<std::string, int> g_texTries;
constexpr int kTextureTries = 6;
constexpr uint32_t kStandInSize = 8; // a view this small (either side) is a stand-in
constexpr auto kTextureRetryAfter = std::chrono::milliseconds(250);
// Since the queue last ran dry: how many loaded, in how long (the log line).
int g_texBatch = 0;
double g_texBatchMs = 0, g_texSlowestMs = 0;

// On the game thread: the queue's textures, a few ms' worth a frame. The
// screen's first frames ask for about 80: all in one frame held the game
// ~5 s after a join (presses lost).
void LoadTextures()
{
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        std::string path;
        {
            std::lock_guard lock(g_texMutex);
            for (auto r = g_texRetries.begin(); r != g_texRetries.end();) {
                if (start < r->due) {
                    ++r;
                    continue;
                }
                g_texQueue.push_back(std::move(r->path));
                r = g_texRetries.erase(r);
            }
            if (g_texQueue.empty() && !g_texRetries.empty()) break; // the next frame looks again
            if (g_texQueue.empty()) {
                g_texLoading = false;
                if (g_texBatch >= 10) {
                    Log("select screen: %d textures loaded, %.0f ms in all (slowest %.0f ms)", g_texBatch, g_texBatchMs,
                        g_texSlowestMs);
                }
                g_texBatch = 0;
                g_texBatchMs = g_texSlowestMs = 0;
                return;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(4)) break;
            path = std::move(g_texQueue.front());
            g_texQueue.pop_front();
        }
        const size_t slash = path.rfind('/');
        const std::string name = path.substr(slash + 1);
        const auto at = std::chrono::steady_clock::now();
        TextureView view = ViewOfTexture(FindOrLoadTexture(path + "." + name), true);
        if (view.srv && (view.width < kStandInSize || view.height < kStandInSize)) {
            Log("select screen: %s has a %ux%u view for now: asking again", path.c_str(), view.width, view.height);
            static_cast<IUnknown*>(view.srv)->Release();
            view = {};
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at).count();
        std::lock_guard lock(g_texMutex);
        if (!view.srv) {
            const int tries = ++g_texTries[path];
            if (tries < kTextureTries) {
                g_texRetries.push_back({path, std::chrono::steady_clock::now() + kTextureRetryAfter});
                continue;
            }
            Log("select screen: no texture at %s (%d tries)", path.c_str(), tries);
        }
        else if (g_texTries.count(path)) {
            Log("select screen: %s loaded on try %d", path.c_str(), g_texTries[path] + 1);
        }
        g_textures[path] = view;
        g_texBatch++;
        g_texBatchMs += ms;
        g_texSlowestMs = std::max(g_texSlowestMs, ms);
    }
    RunOnGameThread(LoadTextures); // the rest next frame
}

const TextureView* Texture(const std::string& path)
{
    std::lock_guard lock(g_texMutex);
    const auto it = g_textures.find(path);
    if (it != g_textures.end()) return it->second.srv ? &it->second : nullptr;
    if (!g_requested.insert(path).second) {
        // Asked for ahead of time (WarmSelectTextures) and drawn now: it
        // loads next (looked for once: drawing asks every frame).
        if (g_texDrawn.insert(path).second) {
            const auto queued = std::find(g_texQueue.begin(), g_texQueue.end(), path);
            if (queued != g_texQueue.end() && queued != g_texQueue.begin()) {
                g_texQueue.erase(queued);
                g_texQueue.push_front(path);
            }
        }
        return nullptr;
    }
    g_texQueue.push_back(path);
    if (!g_texLoading) {
        g_texLoading = true;
        RunOnGameThread(LoadTextures);
    }
    return nullptr;
}

std::string CharaTex(const std::string& code, const char* name) { return "/Game/Chara/" + code + "/Hud/" + name; }
std::string StageTex(const std::string& code, const char* name) { return "/Game/Stage/" + code + "/UI/" + name; }
std::string MenuTex(const char* name) { return std::string("/Game/MenuAsset/CharaSelectGFx/") + name; }
std::string PadTex(const char* name) { return std::string("/Game/CommonAsset/PlatformAsset/Windows/padkey/") + name; }

// ----- data and the screen's own state ----------------------------------------

std::mutex g_dataMutex;
SelectData g_data; // the live screen's lists (SetSelectData)

const char* const kRowLabels[kSelectRows] = {"Costume", "Color", "Variant", "V-Skill", "V-Trigger", "V-Comment", "Input delay"};

// What the player is doing on the screen (a session only knows their pick
// and whether they are ready).
struct Ui {
    bool loadout = false;  // past the grid
    int row = 0;           // the loadout row
    int cursorRow = 0, cursorX = 0;
    bool onRandom = false; // the cursor on the random tile
    int lastChara = -2;    // the pick the cursor last followed (it starts there, and moves when the pick changes)
    int lastStage = 0;     // the stage behind the screen (the last real one while random is chosen)
    bool wasLocked = false;
} g_ui;

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

// ----- input ----------------------------------------------------------------

bool g_inputOn = false;                         // this frame reads input
bool g_clickConfirm = false, g_clickBack = false; // a prompt clicked last frame: A or B this frame

// Directions repeat while held; A, B and Start count once per press (a long
// frame, a character loading, would make a held A a repeat: the loadout's
// Ready right after the grid's Select).
bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat = true)
{
    if (!g_inputOn) return false;
    for (ImGuiKey k : keys) {
        if (ImGui::IsKeyPressed(k, repeat)) return true;
    }
    return false;
}
bool Left() { return Pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}); }
bool Right() { return Pressed({ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}); }
bool Up() { return Pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp}); }
bool Down() { return Pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}); }
bool Confirm()
{
    const bool clicked = std::exchange(g_clickConfirm, false) && g_inputOn;
    return clicked || Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown}, false);
}
bool Back()
{
    const bool clicked = std::exchange(g_clickBack, false) && g_inputOn;
    return clicked || Pressed({ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight}, false);
}
bool Menu() { return Pressed({ImGuiKey_GamepadStart}, false); }

bool Clicked() { return g_inputOn && ImGui::IsMouseClicked(ImGuiMouseButton_Left); }
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

std::string Upper(std::string v)
{
    for (char& ch : v) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return v;
}

float TextW(ImFont* f, float size, const std::string& t) { return f->CalcTextSizeA(g_v.L(size), FLT_MAX, 0, t.c_str()).x / g_v.s; }

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
void IconCloud(ImDrawList* d, ImVec2 c, float s, ImU32 col)
{
    d->AddCircle(ImVec2(c.x - 3 * s, c.y + 1 * s), 3 * s, col, 12, 1.5f * s);
    d->AddCircle(ImVec2(c.x + 1.5f * s, c.y - 1 * s), 4 * s, col, 12, 1.5f * s);
    d->AddLine(ImVec2(c.x - 3 * s, c.y + 4 * s), ImVec2(c.x + 5 * s, c.y + 4 * s), col, 1.5f * s);
}
void IconLinkType(ImDrawList* d, ImVec2 c, float s, int link, ImU32 col)
{
    if (link == 1) IconWired(d, c, s, col);
    else if (link == 2) IconWifi(d, ImVec2(c.x, c.y - 1 * s), s, col);
}

// ----- the parts ----------------------------------------------------------------

const SelectData* g_draw = nullptr; // the data of the screen being drawn

const SelectChara* Chara(int i)
{
    if (!g_draw || i < 0 || i >= static_cast<int>(g_draw->charas.size())) return nullptr;
    return &g_draw->charas[i];
}

// The stage's picture over the whole screen (cropped at the sides: it is 4:1),
// darkened; the screen draws over it.
void DrawBackdrop(ImDrawList* d, int stage, ImU32 tint)
{
    const int count = static_cast<int>(g_draw->stages.size());
    if (stage >= 0 && stage < count) g_ui.lastStage = stage;
    if (count == 0) return;
    const TextureView* bg = Texture(StageTex(g_draw->stages[std::clamp(g_ui.lastStage, 0, count - 1)].code, "StageSelectBG"));
    if (!bg) return;
    const float aspect = bg->height ? static_cast<float>(bg->width) / bg->height : 4.0f;
    const float visible = (1600.0f / 900.0f) / aspect; // the share of the width that fits
    Image(d, bg, g_v.P(0, 0), g_v.P(1600, 900), ImVec2(0.5f - visible / 2, 0), ImVec2(0.5f + visible / 2, 1), tint);
}

// `highlight`: the row lit up (this player's, or the row the other is on), -1 none.
void DrawLoadout(ImDrawList* d, const SelectFonts& f, bool right, const SelectSide& side, int highlight, bool interactive,
                 SelectInput* in)
{
    const float x0 = right ? 980.0f : 0.0f, top = 108, rowH = 40, gap = 4, w = 620;
    ImFont* font = f.text;
    for (int r = 0; r < kSelectRows; r++) {
        const SelectRow& row = side.rows[r];
        const float y = top + r * (rowH + gap);
        const ImVec2 a = g_v.P(x0, y), b = g_v.P(x0 + w, y + rowH);
        const ImU32 dark = Rgba(8, 9, 11, 235), fade = Rgba(8, 9, 11, 200);
        d->AddRectFilledMultiColor(a, b, right ? fade : dark, right ? dark : fade, right ? dark : fade, right ? fade : dark);
        if (r == highlight) {
            const ImVec2 gb = g_v.P(x0 + (right ? w - 280 : 280), y + rowH);
            const ImU32 gold = Rgba(240, 200, 90, 140), none = Rgba(240, 200, 90, 0);
            if (right) d->AddRectFilledMultiColor(ImVec2(gb.x, a.y), b, none, gold, gold, none);
            else d->AddRectFilledMultiColor(a, gb, gold, none, none, gold);
        }
        const ImU32 rule = Rgba(201, 168, 92, 190);
        d->AddLine(a, ImVec2(b.x, a.y), rule, std::max(1.0f, g_v.L(1)));
        d->AddLine(ImVec2(a.x, b.y), b, rule, std::max(1.0f, g_v.L(1)));
        Text(d, font, g_v.L(19), g_v.P(x0 + 44, y + 9), Rgba(244, 241, 232), kRowLabels[r]);
        const float mid = x0 + 190 + (w - 190) / 2;
        const float tw = TextW(font, 18, row.value);
        const float sw = row.swatch1 ? 46.0f : 0.0f;
        const float tx = mid - (tw + sw) / 2;
        Text(d, font, g_v.L(18), g_v.P(tx, y + 10), Rgba(244, 241, 232), row.value);
        if (row.swatch1) {
            const float sx = tx + tw + 10;
            d->AddRectFilled(g_v.P(sx, y + 11), g_v.P(sx + 18, y + 29), FromArgb(row.swatch1));
            d->AddRect(g_v.P(sx, y + 11), g_v.P(sx + 18, y + 29), Rgba(0, 0, 0));
            d->AddRectFilled(g_v.P(sx + 21, y + 11), g_v.P(sx + 39, y + 29), FromArgb(row.swatch2));
            d->AddRect(g_v.P(sx + 21, y + 11), g_v.P(sx + 39, y + 29), Rgba(0, 0, 0));
        }
        if (interactive && !row.fixed) {
            const ImU32 arrow = Rgba(233, 196, 106);
            Text(d, font, g_v.L(15), g_v.P(x0 + 212, y + 11), arrow, "<", 0.5f);
            Text(d, font, g_v.L(15), g_v.P(x0 + w - 22, y + 11), arrow, ">", 0.5f);
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
    const ImVec2 a = g_v.P(x0, y), b = g_v.P(x0 + w, y + 30);
    if (side.ready) {
        const ImU32 g1 = Rgba(217, 166, 50), g2 = Rgba(150, 108, 28, 230);
        d->AddRectFilledMultiColor(a, b, right ? g2 : g1, right ? g1 : g2, right ? g1 : g2, right ? g2 : g1);
        Text(d, f.name, g_v.L(19), g_v.P(x0 + 44, y + 4), Rgba(26, 20, 5), "READY");
    }
    else {
        d->AddRectFilled(a, b, Rgba(8, 9, 11, 185));
        const std::string& hint = side.rows[interactive ? g_ui.row : 5].hint;
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
    if (const SelectChara* c = Chara(t.chara)) {
        if (const TextureView* tex = Texture(CharaTex(c->code, t.right ? "CharaSelectIconRight" : "CharaSelectIconLeft"))) {
            // The portrait corner to corner: same angle as the frame, no gaps.
            const ImVec2* art = t.right ? kArtRight : kArtLeft;
            auto uv = [&](int i) { return ImVec2(art[i].x / 116.0f, art[i].y / 144.0f); };
            d->AddImageQuad(Id(tex), iq[0], iq[1], iq[2], iq[3], uv(0), uv(1), uv(2), uv(3), on ? IM_COL32_WHITE : Rgba(205, 205, 205));
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
    const float pw = (s.link ? 34.0f : 0.0f) + (link ? 34.0f : 0.0f) + (ping[0] ? TextW(f.text, 15, ping) + 18 : 0.0f);
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
    if (ping[0]) Text(d, f.text, g_v.L(15), g_v.P(cx + 4, y + 5), Rgba(127, 216, 139), ping);
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

// The stage select: the stage's picture inside the game's curved frame, its
// name, and the strip of stages. Only the host moves it.
void DrawStageSelect(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in)
{
    const int count = static_cast<int>(g_draw->stages.size());
    const bool picking = live.host && live.you >= 0;
    int stage = std::clamp(live.stage, 0, count);
    if (picking && Left()) in->stage = stage = (stage + count) % (count + 1);
    if (picking && Right()) in->stage = stage = (stage + 1) % (count + 1);
    Text(d, f.name, g_v.L(42), g_v.P(800, 76), Rgba(255, 255, 255), "STAGE SELECT", 0.5f, true);
    // The frame across the whole screen (its texture is 1920x534).
    const float bx = 0, by = 132, bw = 1600, bh = 1600.0f * 534 / 1920;
    if (const TextureView* shadow = Texture(MenuTex("StagePreviewWindowShadow"))) Image(d, shadow, g_v.P(bx, by), g_v.P(bx + bw, by + bh));
    const bool random = stage >= count;
    const std::string code = random || count == 0 ? "" : g_draw->stages[stage].code;
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
            d->AddImageQuad(Id(bg), q0, q1, q2, q3, ImVec2(ua, a0 / mh), ImVec2(ub, a1 / mh), ImVec2(ub, 1 - a1 / mh), ImVec2(ua, 1 - a0 / mh));
        }
    }
    if (const TextureView* frame = Texture(MenuTex("StagePreviewWindow"))) Image(d, frame, g_v.P(bx, by), g_v.P(bx + bw, by + bh));
    const std::string name = random ? "Random" : count ? g_draw->stages[stage].name : "";
    Text(d, f.name, g_v.L(46), g_v.P(800, by + bh - 106), Rgba(255, 255, 255), name, 0.5f, true);
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
        const TextureView* t = i < count ? Texture(StageTex(g_draw->stages[i].code, "StageSelectIcon")) : Texture(MenuTex("StageSelect_Random"));
        if (t) Image(d, t, a, b);
        else d->AddRectFilled(a, b, Rgba(30, 32, 36));
        if (on) {
            if (const TextureView* cursor = Texture(MenuTex("StageSelectCursor"))) Image(d, cursor, ImVec2(a.x - g_v.L(1), a.y - g_v.L(1)), ImVec2(b.x + g_v.L(1), b.y + g_v.L(1)));
        }
        if (picking && Clicked() && MouseIn(g_v.P(x, sy), g_v.P(x + sw, sy + sh))) in->stage = i;
    }
    Text(d, f.text, g_v.L(16), g_v.P(800, sy + sh + 18), Rgba(189, 184, 170),
         picking ? "You select the stage." : "Player 1 is selecting the stage...", 0.5f);
    if (picking && Confirm()) in->lockStage = true;
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

// The prompts at the top right: the game's pad icons with what they do here.
// A click acts as the button (A and B on the next frame).
struct Prompt {
    const char* icon;
    std::string label;
    int action; // 0 confirm (A), 1 back (B), 2 menu (Start)
};

constexpr const char* kLoadingText = "Both ready: loading the characters";

// A line of status over the bottom strip.
void StatusPill(ImDrawList* d, const SelectFonts& f, const char* text)
{
    const float w = TextW(f.text, 16, text) + 40;
    Pill(d, g_v.P(800 - w / 2, 860), g_v.P(800 + w / 2, 890));
    Text(d, f.text, g_v.L(16), g_v.P(800, 866), Rgba(233, 196, 106), text, 0.5f);
}

void DrawPrompts(ImDrawList* d, const SelectFonts& f, const std::vector<Prompt>& prompts, SelectInput* in)
{
    float right = 1572;
    for (int i = static_cast<int>(prompts.size()) - 1; i >= 0; i--) {
        const Prompt& p = prompts[i];
        const float w = TextW(f.text, 17, p.label) + 50;
        const ImVec2 a = g_v.P(right - w, 14), b = g_v.P(right, 56);
        d->AddRectFilled(a, b, Rgba(0, 0, 0, 150), g_v.L(21));
        if (const TextureView* icon = Texture(PadTex(p.icon))) Image(d, icon, g_v.P(right - w + 5, 19), g_v.P(right - w + 37, 51));
        Text(d, f.text, g_v.L(17), g_v.P(right - w + 42, 24), Rgba(244, 241, 232), p.label);
        if (Clicked() && MouseIn(a, b)) {
            if (p.action == 0) g_clickConfirm = true;
            else if (p.action == 1) g_clickBack = true;
            else in->menu = true;
        }
        right -= w + 18;
    }
}

// The whole screen for a lobby's state; what the player does goes to `in`.
void Render(ImDrawList* d, const SelectFonts& f, const SelectLive& live, SelectInput* in)
{
    if (g_gridFor != g_draw) BuildGrid(*g_draw);
    const bool watching = live.you < 0;
    const SelectSide* me = watching ? nullptr : &live.sides[live.you];
    if (live.stageLocked && !g_ui.wasLocked) {
        // On to the characters: the cursor starts on the player's pick.
        g_ui.loadout = false;
        g_ui.onRandom = false;
        g_ui.lastChara = -2;
    }
    g_ui.wasLocked = live.stageLocked;

    std::vector<Prompt> prompts;
    if (!live.stageLocked) {
        DrawBackdrop(d, live.stage, Rgba(70, 69, 67));
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
        for (const char* icon : {"button_a", "button_b"}) Texture(PadTex(icon));
        if (live.host && !watching) prompts.push_back({"button_a", "Confirm stage", 0});
        if (Back()) in->back = true;
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
        DrawBackdrop(d, live.stage, Rgba(118, 116, 112));
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
            DrawPlayerLine(d, f, live, i);
            const SelectChara* c = Chara(shown[i]);
            const std::string name = onRandom[i] ? "RANDOM" : c ? Upper(c->name) : "";
            Text(d, f.name, g_v.L(74), g_v.P(i ? 1482.0f : 118.0f, 496), Rgba(255, 255, 255), name, i ? 1.0f : 0.0f, true);
        }
        Text(d, f.name, g_v.L(42), g_v.P(800, 502), Rgba(255, 255, 255), "CHARACTER SELECT", 0.5f, true);
        Infinity(d, g_v.P(800, 578), g_v.L(56), Rgba(242, 201, 76));
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
                    if (g_ui.cursorRow == r && g_ui.cursorX == x) g_clickConfirm = true;
                    g_ui.cursorRow = r;
                    g_ui.cursorX = x;
                    g_ui.onRandom = t.random;
                }
            }
        }
        if (grid) {
            prompts.push_back({"button_a", "Select", 0});
            if (live.host) prompts.push_back({"button_b", "Stage", 1});
        }
        else if (loadout) {
            prompts.push_back({"button_a", "Ready", 0});
            prompts.push_back({"button_b", "Back", 1});
        }
        if (ready) {
            // B says not ready, until both are: then the match is locked in
            // (Leave is in the menu).
            if (live.bothReady) StatusPill(d, f, live.loading ? kLoadingText : "Both ready: the match is starting");
            else {
                StatusPill(d, f, "Ready: the match starts once both players are");
                prompts.push_back({"button_b", "Not ready", 1});
            }
        }
        // A and B.
        const bool a = Confirm();
        if (grid && a) {
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
        if (Back()) {
            if (ready) in->unready = !live.bothReady;
            else if (loadout) g_ui.loadout = false;
            else if (grid && live.host) in->unlockStage = true;
            else in->back = true;
        }
    }
    if (watching) {
        // Watching: the spectator's own link to the host, and who else watches.
        const char* text = "WATCHING";
        char detail[64];
        if (live.pingMs) std::snprintf(detail, sizeof(detail), "%u ms to the host  \xC2\xB7  %d watching", live.pingMs, std::max(1, live.spectators));
        else std::snprintf(detail, sizeof(detail), "%d watching", std::max(1, live.spectators));
        const float tw = TextW(f.text, 16, text), dw = TextW(f.text, 15, detail);
        const float w = 24 + tw + 18 + 30 + dw + 24;
        const float bx = 800 - w / 2;
        Pill(d, g_v.P(bx, 18), g_v.P(bx + w, 50));
        Text(d, f.text, g_v.L(16), g_v.P(bx + 24, 25), Rgba(242, 201, 76), text);
        const float ix = bx + 24 + tw + 18;
        if (live.relayed) IconCloud(d, g_v.P(ix + 10, 34), g_v.s, Rgba(233, 196, 106));
        else IconLink(d, g_v.P(ix + 10, 34), g_v.s, Rgba(217, 212, 198));
        Text(d, f.text, g_v.L(15), g_v.P(ix + 28, 26), Rgba(217, 212, 198), detail);
    }
    if (watching && live.loading) StatusPill(d, f, kLoadingText);
    prompts.push_back({"button_start", "Menu", 2});
    DrawPrompts(d, f, prompts, in);
    if (Menu()) in->menu = true;
}

// Every vertex drawn since `from` at `alpha` (the screen's fade).
void FadeFrom(ImDrawList* d, int from, float alpha)
{
    if (alpha >= 1) return;
    const float k = std::clamp(alpha, 0.0f, 1.0f);
    for (int i = from; i < d->VtxBuffer.Size; i++) {
        ImU32& c = d->VtxBuffer[i].col;
        const ImU32 a = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * k);
        c = (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

void Begin()
{
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    g_v.s = std::min(screen.x / 1600.0f, screen.y / 900.0f);
    g_v.ox = (screen.x - 1600 * g_v.s) / 2;
    g_v.oy = (screen.y - 900 * g_v.s) / 2;
}

// ----- the preview's mock players -----------------------------------------------

std::atomic<bool> g_previewOpen{false};
bool g_previewFresh = false;
int g_previewPhase = 0;
SelectData g_preview;

struct Mock {
    int stage = 0;
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
    const char* hints[kSelectRows] = {"Pick a costume.", "Pick a color.", "A variant of this costume.", "V-Skill.", "V-Trigger.", "",
                                      "Suggested: 2 frames for a 38 ms ping. Lower reacts sooner, with more rollbacks."};
    const int comment = v[5] % (1 + static_cast<int>(c.vcomments.size()));
    const std::string commentLine = eleven ? "A random fighter in silver, its V-Skill and V-Trigger at random."
                                    : comment == 0 ? "Comment changes automatically based on opponent's character."
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
    for (size_t i = 0; i < g_preview.stages.size(); i++) {
        if (g_preview.stages[i].code == "TAI") g_mock.stage = static_cast<int>(i);
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
    // The stage select: its frame, icons and prompts, then each stage's icon.
    for (const char* name : {"StagePreviewWindowShadow", "StagePreviewWindow", "StageSelectCursor", "StageSelect_Random"}) {
        Texture(MenuTex(name));
    }
    for (const char* icon : {"button_a", "button_b", "button_start"}) Texture(PadTex(icon));
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
    for (const SelectChara& c : g_data.charas) Texture(CharaTex(c.code, "PartCharacter"));
}

void DrawSelectLive(const SelectFonts& f, const SelectLive& live, float alpha, bool takeInput, SelectInput* input)
{
    *input = SelectInput{};
    if (!f.title || !f.name || !f.text || alpha <= 0) return;
    std::lock_guard lock(g_dataMutex);
    if (g_data.charas.empty()) return;
    g_draw = &g_data;
    Begin();
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
    Begin();
    if (g_preview.spectator) MockTick(ImGui::GetIO().DeltaTime);
    SelectLive live;
    live.you = g_preview.spectator ? -1 : 0;
    live.host = true;
    live.sides[0] = MockSide(0);
    live.sides[1] = MockSide(1);
    live.stage = g_mock.stage;
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
    if (in.stage >= 0) g_mock.stage = in.stage;
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
    // Back with nothing to go back to (the stage select, watching) or Start
    // closes the preview.
    if (in.back || in.menu) g_previewOpen = false;
}

} // namespace sfv::mod
