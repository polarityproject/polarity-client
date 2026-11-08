#include "mod/ui_kit.h"

#include "core/key_names.h"
#include "mod/flow.h"
#include "mod/log.h"
#include "mod/pak_textures.h"

#include <unknwn.h>

#include <imgui_internal.h> // the draw list's white pixel, IM_PI

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace sfv::mod::ui {

// ----- the game's textures --------------------------------------------------

// Read from the paks on the mod's own threads (pak_textures.h); one the
// paks cannot give is loaded on the game thread instead. The render thread
// draws the views once they are there (a missing one leaves its place
// dark) and fades each in (kFadeInMs).
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
bool g_drawing = true;          // false while WarmSelectTextures asks ahead (render thread only)
constexpr int64_t kFadeInMs = 150;
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
        view.readyMs = TextureClockMs();
        g_textures[path] = view;
        g_texBatch++;
        g_texBatchMs += ms;
        g_texSlowestMs = std::max(g_texSlowestMs, ms);
    }
    RunOnGameThread(LoadTextures); // the rest next frame
}

const TextureView* Texture(const std::string& path)
{
    bool failed = false;
    // Asked for by drawing (not ahead of time): it goes first.
    if (const TextureView* t = PakTexture(path, 0, g_drawing, &failed)) return t;
    if (!failed) return nullptr;
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

void Glyph(ImDrawList* d, const char* name, ImVec2 a, ImVec2 b, ImU32 tint, bool shadow)
{
    const std::string path = PadTex(name);
    const TextureView* t = Texture(path);
    if (!t) return;
    if (shadow) {
        // shadow-text's 0 2px 4px at .8, for a 32 px glyph: a 64 px copy,
        // blurred 4 px (2 design), with 12 px of room round it.
        constexpr uint32_t kSide = 64, kPad = 12;
        TexLook look;
        look.shadow = true;
        look.blur = 4;
        look.pad = kPad;
        bool failed = false;
        if (const TextureView* s = PakTexture(path, kSide, g_drawing, &failed, look)) {
            const float inner = static_cast<float>(std::max(s->width, s->height) - 2 * kPad);
            const float px = (b.x - a.x) * kPad / inner, py = (b.y - a.y) * kPad / inner;
            const float drop = (b.y - a.y) / 16;
            const uint32_t alpha = (tint >> IM_COL32_A_SHIFT & 0xFF) * 204 / 255;
            d->AddImage(Id(s), ImVec2(a.x - px, a.y - py + drop), ImVec2(b.x + px, b.y + py + drop), ImVec2(0, 0), ImVec2(1, 1),
                        FadeIn(s, IM_COL32(255, 255, 255, alpha)));
        }
    }
    Image(d, t, a, b, ImVec2(0, 0), ImVec2(1, 1), tint);
}

View g_v;

void BeginScreen()
{
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    g_v.s = std::min(screen.x / 1600.0f, screen.y / 900.0f);
    g_v.ox = (screen.x - 1600 * g_v.s) / 2;
    g_v.oy = (screen.y - 900 * g_v.s) / 2;
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

// ----- input ----------------------------------------------------------------

bool g_inputOn = false;                         // this frame reads input
bool g_textEntry = false;
namespace {
int g_keyboardMap[16] = {};
} // namespace

void SetKeyboardMap(const int (&vk)[16]) { std::copy(std::begin(vk), std::end(vk), std::begin(g_keyboardMap)); }

uint32_t MappedPadKeys(int vk)
{
    uint32_t bits = 0;
    for (int slot = 0; slot < kKeyboardSlots && vk; slot++) {
        if (g_keyboardMap[slot] == vk) bits |= PadKeyOfSlot(slot);
    }
    return bits;
}
ImGuiKey g_clickKey = ImGuiKey_None, g_clickNext = ImGuiKey_None; // a glyph clicked: this frame's button, the next's

void ClickPad(ImGuiKey key) { g_clickNext = key; }
void BeginInputFrame() { g_clickKey = std::exchange(g_clickNext, ImGuiKey_None); }

ImGuiKey PadKeyOfGlyph(const char* glyph)
{
    struct Named {
        const char* glyph;
        ImGuiKey key;
    };
    static const Named kGlyphs[] = {
        {"button_a", ImGuiKey_GamepadFaceDown}, {"button_b", ImGuiKey_GamepadFaceRight}, {"button_x", ImGuiKey_GamepadFaceLeft},
        {"button_y", ImGuiKey_GamepadFaceUp},   {"button_start", ImGuiKey_GamepadStart}, {"button_back", ImGuiKey_GamepadBack},
        {"button_lb", ImGuiKey_GamepadL1},      {"button_rb", ImGuiKey_GamepadR1},
    };
    for (const Named& n : kGlyphs) {
        if (glyph && std::strcmp(glyph, n.glyph) == 0) return n.key;
    }
    return ImGuiKey_None;
}

// Directions repeat while held; A and B count once per press (a long
// frame, a character loading, would make a held A a repeat: the loadout's
// Ready right after the grid's Select).
bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat)
{
    if (!g_inputOn) return false;
    for (ImGuiKey k : keys) {
        if (k == g_clickKey || ImGui::IsKeyPressed(k, repeat)) return true;
    }
    return false;
}
// The keyboard's keys come as the pad's (SetKeyboardMap): no keys of the
// mod's own beside them, so a keyboard works as in the game's menus.
bool Left() { return Pressed({ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}); }
bool Right() { return Pressed({ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}); }
bool Up() { return Pressed({ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp}); }
bool Down() { return Pressed({ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}); }
bool Confirm() { return Pressed({ImGuiKey_GamepadFaceDown}, false); }
bool Back() { return Pressed({ImGuiKey_GamepadFaceRight}, false); }
// Y: the stage's next setting.
bool NextSetting()
{
    return Pressed({ImGuiKey_GamepadFaceUp}, false);
}

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

ImU32 Rgba(int r, int g, int b, int a) { return IM_COL32(r, g, b, a); }
ImU32 FromArgb(uint32_t argb) { return IM_COL32((argb >> 16) & 255, (argb >> 8) & 255, argb & 255, 255); }

std::string Upper(std::string v)
{
    for (char& ch : v) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return v;
}

float TextW(ImFont* f, float size, const std::string& t) { return f->CalcTextSizeA(TextPx(size), FLT_MAX, 0, t.c_str()).x / g_v.s; }

void Text(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 c, const std::string& t, float align, bool shadow)
{
    size = std::max(size, kMinTextPx);
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

const TextureView* TextureSized(const std::string& path, float pixels)
{
    // In steps of 32 px, so a window resize asks for few new sizes.
    const uint32_t side = std::max(32u, static_cast<uint32_t>(std::ceil(pixels / 32.0f)) * 32u);
    const TextureView* full = Texture(path);
    bool failed = false;
    const TextureView* shrunk = PakTexture(path, side, g_drawing, &failed);
    // Swapped in once its fade-in is over, so the art does not flash.
    if (shrunk && (!full || TextureClockMs() - shrunk->readyMs >= kFadeInMs)) return shrunk;
    return full;
}

ImTextureID Id(const TextureView* t) { return reinterpret_cast<ImTextureID>(t->srv); }

// A texture's tint while it fades in after arriving.
ImU32 FadeIn(const TextureView* t, ImU32 tint)
{
    const int64_t age = TextureClockMs() - t->readyMs;
    if (age >= kFadeInMs || age < 0) return tint;
    const uint32_t alpha = (tint >> IM_COL32_A_SHIFT & 0xFF) * static_cast<uint32_t>(age) / kFadeInMs;
    return (tint & ~IM_COL32_A_MASK) | alpha << IM_COL32_A_SHIFT;
}

void Image(ImDrawList* d, const TextureView* t, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 tint)
{
    if (t) d->AddImage(Id(t), a, b, uv0, uv1, FadeIn(t, tint));
}

void ImageFadeX(ImDrawList* d, const TextureView* t, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, const FadeStop* stops, int count,
                bool mirror, ImU32 tint, float fadeA, float fadeB)
{
    if (!t || count < 2 || b.x <= a.x) return;
    const ImU32 col = FadeIn(t, tint);
    const float base = static_cast<float>(col >> IM_COL32_A_SHIFT & 0xFF);
    if (base <= 0) return;
    auto at = [&](float alpha) {
        const uint32_t v = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * base + 0.5f);
        return (col & ~IM_COL32_A_MASK) | v << IM_COL32_A_SHIFT;
    };
    // The fade's stretch of the screen, and its alpha at a screen x.
    const float fa = fadeB > fadeA ? fadeA : a.x, fb = fadeB > fadeA ? fadeB : b.x;
    auto alphaAt = [&](float x) {
        const float f = (x - fa) / (fb - fa);
        if (f <= stops[0].x) return stops[0].alpha;
        for (int i = 1; i < count; i++) {
            if (f <= stops[i].x) {
                const float span = stops[i].x - stops[i - 1].x;
                return span > 0 ? stops[i - 1].alpha + (stops[i].alpha - stops[i - 1].alpha) * (f - stops[i - 1].x) / span : stops[i].alpha;
            }
        }
        return stops[count - 1].alpha;
    };
    // The image's x at a screen x, as a texture coordinate.
    auto u = [&](float x) {
        const float f = (x - a.x) / (b.x - a.x);
        return mirror ? uv1.x - (uv1.x - uv0.x) * f : uv0.x + (uv1.x - uv0.x) * f;
    };
    // A strip from each stop to the next, within the image.
    float xs[34];
    int n = 0;
    xs[n++] = a.x;
    for (int i = 0; i < count && n < 33; i++) {
        const float x = fa + (fb - fa) * stops[i].x;
        if (x > a.x && x < b.x) xs[n++] = x;
    }
    xs[n++] = b.x;
    std::sort(xs, xs + n);
    d->PushTexture(ImTextureRef(Id(t)));
    for (int i = 0; i + 1 < n; i++) {
        const float x0 = xs[i], x1 = xs[i + 1];
        const float a0 = alphaAt(x0), a1 = alphaAt(x1);
        if (x1 <= x0 || (a0 <= 0 && a1 <= 0)) continue;
        const ImU32 c0 = at(a0), c1 = at(a1);
        d->PrimReserve(6, 4);
        const ImDrawIdx idx = static_cast<ImDrawIdx>(d->_VtxCurrentIdx);
        d->PrimWriteIdx(idx);
        d->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 1));
        d->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
        d->PrimWriteIdx(idx);
        d->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 2));
        d->PrimWriteIdx(static_cast<ImDrawIdx>(idx + 3));
        d->PrimWriteVtx(ImVec2(x0, a.y), ImVec2(u(x0), uv0.y), c0);
        d->PrimWriteVtx(ImVec2(x1, a.y), ImVec2(u(x1), uv0.y), c1);
        d->PrimWriteVtx(ImVec2(x1, b.y), ImVec2(u(x1), uv1.y), c1);
        d->PrimWriteVtx(ImVec2(x0, b.y), ImVec2(u(x0), uv1.y), c0);
    }
    d->PopTexture();
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

// The prompts at the top right: the game's pad icons with what they do here.
// A click acts as the button (A and B on the next frame).


void DrawPrompts(ImDrawList* d, ImFont* text, const std::vector<Prompt>& prompts, float right, float top)
{
    for (int i = static_cast<int>(prompts.size()) - 1; i >= 0; i--) {
        const Prompt& p = prompts[i];
        const float w = TextW(text, 19, p.label) + 52;
        const ImVec2 a = g_v.P(right - w, top), b = g_v.P(right, top + 42);
        d->AddRectFilled(a, b, Rgba(0, 0, 0, 150), g_v.L(21));
        Glyph(d, p.icon, g_v.P(right - w + 5, top + 5), g_v.P(right - w + 37, top + 37), IM_COL32_WHITE, false);
        Text(d, text, g_v.L(19), g_v.P(right - w + 42, top + 9), Rgba(244, 241, 232), p.label);
        if (Clicked() && MouseIn(a, b)) {
            ClickPad(PadKeyOfGlyph(p.icon));
        }
        right -= w + 18;
    }
}

// ----- the 1d look ------------------------------------------------------------

using namespace look;

namespace {

const char* const kStage = "/Game/Stage/S21/UI/StageSelectBG"; // the Capcom Pro Tour arena, 2048 x 512
// The stage covers the design space's full height, 4:1: x -1000 to 2600.
constexpr float kStageX0 = -1000, kStageW = 3600;
constexpr ImU32 kGlassPanel = IM_COL32(10, 12, 16, 140);
constexpr ImU32 kGlassModal = IM_COL32(10, 12, 16, 214);
constexpr ImU32 kGlassFight = IM_COL32(10, 12, 16, 184);
constexpr ImU32 kEdge = IM_COL32(236, 228, 206, 102);
constexpr ImU32 kEdgeModal = IM_COL32(236, 228, 206, 115);
constexpr ImU32 kHighlight = IM_COL32(255, 255, 255, 77);
constexpr ImU32 kOutline = IM_COL32(0, 0, 0, 140);
constexpr ImU32 kStrip = IM_COL32(0, 0, 0, 82);
// The stage art drawn behind a whole screen, as the CSS: brightness 0.88,
// then the flat scrim (6,8,11 at 15%).
constexpr ImU32 kStageTint = IM_COL32(224, 224, 224, 255);
constexpr ImU32 kScrim = IM_COL32(6, 8, 11, 38);
constexpr ImU32 kGrain = IM_COL32(255, 255, 255, 22); // the noise over the backdrop

ImU32 WithAlpha(ImU32 c, float k)
{
    const ImU32 a = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(k, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// A quad with one color at its first two corners and another at the last two.
void Quad(ImDrawList* d, ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, ImU32 c01, ImU32 c23)
{
    const ImVec2 uv = d->_Data->TexUvWhitePixel;
    d->PrimReserve(6, 4);
    const ImDrawIdx i = static_cast<ImDrawIdx>(d->_VtxCurrentIdx);
    d->PrimWriteIdx(i);
    d->PrimWriteIdx(static_cast<ImDrawIdx>(i + 1));
    d->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
    d->PrimWriteIdx(i);
    d->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
    d->PrimWriteIdx(static_cast<ImDrawIdx>(i + 3));
    d->PrimWriteVtx(p0, uv, c01);
    d->PrimWriteVtx(p1, uv, c01);
    d->PrimWriteVtx(p2, uv, c23);
    d->PrimWriteVtx(p3, uv, c23);
}

// The stage's picture between design x0..x1, y0..y1 at its own place: the
// backdrop (blur-stage 4 px, saturate 1.1) or a pane's copy (blur-glass
// 18 px, saturate 1.25). `pixels`: the copy's width; the blur is in design
// units, so the same whatever the window's size. Until the blurred one is
// made: nothing (the ground), or the plain one if it cannot be.
void StageAt(ImDrawList* d, float x0, float y0, float x1, float y1, uint32_t pixels, float blur, float saturate, ImU32 tint)
{
    TexLook look;
    look.blur = blur * pixels / kStageW;
    look.saturate = saturate;
    bool failed = false;
    const TextureView* t = PakTexture(kStage, pixels, g_drawing, &failed, look);
    if (failed) t = TextureSized(kStage, static_cast<float>(pixels));
    if (!t) return;
    const ImVec2 uv0((x0 - kStageX0) / kStageW, y0 / 900), uv1((x1 - kStageX0) / kStageW, y1 / 900);
    d->AddImage(Id(t), g_v.P(x0, y0), g_v.P(x1, y1), uv0, uv1, FadeIn(t, tint));
}

} // namespace

int g_backdropEnd = -1;

bool BackdropReady()
{
    // Backdrop's stage art (StageAt's look, 1024 px): loaded, or it never will.
    TexLook look;
    look.blur = 4.0f * 1024 / kStageW;
    look.saturate = 1.1f;
    bool failed = false;
    return PakTexture(kStage, 1024, true, &failed, look) || failed;
}

void Backdrop(ImDrawList* d, float alpha)
{
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    d->AddRectFilled(ImVec2(0, 0), screen, WithAlpha(kGround, alpha));
    // Softened a little: a copy about a quarter of the drawn width, stretched.
    const float left = std::max(kStageX0, -g_v.ox / g_v.s), right = std::min(kStageX0 + kStageW, (screen.x - g_v.ox) / g_v.s);
    StageAt(d, left, 0, right, 900, 1024, 4, 1.1f, WithAlpha(kStageTint, alpha));
    d->AddRectFilled(ImVec2(0, 0), screen, WithAlpha(kScrim, alpha));
    Grain(d, alpha);
    g_backdropEnd = d->VtxBuffer.Size;
}

void Grain(ImDrawList* d, float alpha)
{
    // A 256 px tile of noise at about a screen pixel each (two on a big
    // screen), laid over lightly.
    const TextureView* n = NoiseTexture(256);
    if (!n) return;
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float tile = 256 * std::max(1.0f, std::round(g_v.s));
    const ImU32 tint = FadeIn(n, WithAlpha(kGrain, alpha));
    for (float y = 0; y < screen.y; y += tile) {
        for (float x = 0; x < screen.x; x += tile) d->AddImage(Id(n), ImVec2(x, y), ImVec2(x + tile, y + tile), ImVec2(0, 0), ImVec2(1, 1), tint);
    }
}

const TextureView* BlurredStage(const std::string& path, float blur, float saturate, bool* failed)
{
    // 1024 px across the stage's 3600 design units (it is 4:1 at 900 high).
    TexLook look;
    look.blur = blur * 1024 / kStageW;
    look.saturate = saturate;
    return PakTexture(path, 1024, g_drawing, failed, look);
}

void DropShadow(ImDrawList* d, float x0, float y0, float x1, float y1, float offset, float blur, float alpha, bool core)
{
    // A solid core under the box (hidden by an opaque pane), and a feather
    // from its edge out to `blur`, falling to nothing.
    const ImU32 in = IM_COL32(0, 0, 0, static_cast<int>(255 * alpha)), out = IM_COL32(0, 0, 0, 0);
    const ImVec2 a = g_v.P(x0, y0 + offset), b = g_v.P(x1, y1 + offset);
    const float r = g_v.L(blur);
    if (core) d->AddRectFilled(a, b, in);
    // No core (a translucent pane: the fight shows through it): the offset
    // still leaves a strip below the box the feather starts under, which
    // read as a gap between the pane and its shadow. That strip, solid.
    else if (offset > 0) d->AddRectFilled(g_v.P(x0, y1), b, in);
    const ImVec2 A(a.x - r, a.y - r), B(b.x + r, b.y + r);
    Quad(d, ImVec2(a.x, a.y), ImVec2(b.x, a.y), ImVec2(b.x, A.y), ImVec2(a.x, A.y), in, out); // top
    Quad(d, ImVec2(b.x, b.y), ImVec2(a.x, b.y), ImVec2(a.x, B.y), ImVec2(b.x, B.y), in, out); // bottom
    Quad(d, ImVec2(a.x, b.y), ImVec2(a.x, a.y), ImVec2(A.x, a.y), ImVec2(A.x, b.y), in, out); // left
    Quad(d, ImVec2(b.x, a.y), ImVec2(b.x, b.y), ImVec2(B.x, b.y), ImVec2(B.x, a.y), in, out); // right
    // The corners: a fan from the box's corner to the feather's.
    const ImVec2 corners[4][3] = {{a, ImVec2(a.x, A.y), ImVec2(A.x, a.y)},
                                  {ImVec2(b.x, a.y), ImVec2(B.x, a.y), ImVec2(b.x, A.y)},
                                  {b, ImVec2(b.x, B.y), ImVec2(B.x, b.y)},
                                  {ImVec2(a.x, b.y), ImVec2(A.x, b.y), ImVec2(a.x, B.y)}};
    const ImVec2 uv = d->_Data->TexUvWhitePixel;
    for (const auto& c : corners) {
        constexpr int kSteps = 6;
        d->PrimReserve(kSteps * 3, kSteps + 2);
        const ImDrawIdx base = static_cast<ImDrawIdx>(d->_VtxCurrentIdx);
        d->PrimWriteVtx(c[0], uv, in);
        const float a0 = std::atan2(c[1].y - c[0].y, c[1].x - c[0].x);
        float a1 = std::atan2(c[2].y - c[0].y, c[2].x - c[0].x);
        if (a1 - a0 > IM_PI) a1 -= 2 * IM_PI;
        if (a0 - a1 > IM_PI) a1 += 2 * IM_PI;
        for (int i = 0; i <= kSteps; i++) {
            const float t = a0 + (a1 - a0) * i / kSteps;
            d->PrimWriteVtx(ImVec2(c[0].x + std::cos(t) * r, c[0].y + std::sin(t) * r), uv, out);
        }
        for (int i = 0; i < kSteps; i++) {
            d->PrimWriteIdx(base);
            d->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
            d->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2 + i));
        }
    }
}

void Pane(ImDrawList* d, float x0, float y0, float x1, float y1, Glass kind, ImU32 edge)
{
    // shadow-pane: 0 6px 20px rgba(0,0,0,.35) (a fight's pane has nothing
    // opaque inside, so no core under it).
    DropShadow(d, x0, y0, x1, y1, 6, 18, 0.32f, kind != Glass::Fight);
    const ImVec2 a = g_v.P(x0, y0), b = g_v.P(x1, y1);
    if (kind != Glass::Fight) {
        // The glass: the stage behind it, blurred (a 256 px copy stretched),
        // under the tint. Until the copy is there: the ground.
        d->AddRectFilled(a, b, kGround);
        StageAt(d, x0, y0, x1, y1, 512, 18, 1.25f, kStageTint);
        d->AddRectFilled(a, b, kScrim);
    }
    d->AddRectFilled(a, b, kind == Glass::Modal ? kGlassModal : kind == Glass::Fight ? kGlassFight : kGlassPanel);
    const float e = g_v.L(2);
    d->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), kOutline, 0.0f, 0, 1.0f);
    d->AddRect(ImVec2(a.x + e / 2, a.y + e / 2), ImVec2(b.x - e / 2, b.y - e / 2), edge ? edge : kind == Glass::Modal ? kEdgeModal : kEdge, 0, 0, e);
    d->AddLine(ImVec2(a.x + e, a.y + e + 0.5f), ImVec2(b.x - e, a.y + e + 0.5f), kHighlight, std::max(1.0f, g_v.L(1)));
}

void Strip(ImDrawList* d, float x0, float y0, float x1, float y1, bool lit)
{
    d->AddRectFilled(g_v.P(x0, y0), g_v.P(x1, y1), lit ? kGold : kStrip);
}

void Ring(ImDrawList* d, float x0, float y0, float x1, float y1)
{
    const float w = g_v.L(2);
    d->AddRect(ImVec2(g_v.P(x0, y0).x - w / 2, g_v.P(x0, y0).y - w / 2), ImVec2(g_v.P(x1, y1).x + w / 2, g_v.P(x1, y1).y + w / 2), kRing, 0, 0, w);
}

void Control(ImDrawList* d, float x0, float y0, float x1, float y1, bool focused, ImU32 focusFill)
{
    const ImVec2 a = g_v.P(x0, y0), b = g_v.P(x1, y1);
    if (focused) {
        d->AddRectFilled(a, b, focusFill);
        return;
    }
    d->AddRectFilled(a, b, kControl);
    d->AddRect(a, b, kEdgeControl, 0, 0, std::max(1.0f, g_v.L(1)));
}

void StageText(ImDrawList* d, ImFont* f, float size, float x, float y, ImU32 c, const std::string& t, float align)
{
    // shadow-text: 0 2px 4px rgba(0,0,0,.8), 0 0 12px rgba(0,0,0,.5): a
    // copy 2 px down, and a soft halo of four copies round it.
    const float px = TextPx(size);
    const ImVec2 w = f->CalcTextSizeA(px, FLT_MAX, 0, t.c_str());
    const ImVec2 p(g_v.P(x, y).x - w.x * align, g_v.P(x, y).y);
    const float k = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    const float h = std::max(1.0f, g_v.L(1.5f));
    const ImU32 halo = IM_COL32(0, 0, 0, static_cast<int>(70 * k));
    d->AddText(f, px, ImVec2(p.x - h, p.y), halo, t.c_str());
    d->AddText(f, px, ImVec2(p.x + h, p.y), halo, t.c_str());
    d->AddText(f, px, ImVec2(p.x, p.y - h), halo, t.c_str());
    d->AddText(f, px, ImVec2(p.x, p.y + h * 2), halo, t.c_str());
    d->AddText(f, px, ImVec2(p.x, p.y + g_v.L(2)), IM_COL32(0, 0, 0, static_cast<int>(200 * k)), t.c_str());
    d->AddText(f, px, p, c, t.c_str());
}

void ScrollBar(ImDrawList* d, float x, float y0, float y1, float first, float inView, float total, bool stage)
{
    if (total <= inView || total <= 0) return;
    const float h = y1 - y0;
    const float top = y0 + h * std::clamp(first / total, 0.0f, 1.0f), len = std::max(h * inView / total, 12.0f);
    if (stage) {
        // On the stage: a dark track framed, a gold thumb.
        DropShadow(d, x - 1, y0, x + 6, y1, 2, 6, 0.3f, false);
        d->AddRectFilled(g_v.P(x - 1, y0), g_v.P(x + 6, y1), IM_COL32(6, 8, 11, 199));
        d->AddRect(g_v.P(x - 1, y0), g_v.P(x + 6, y1), kEdgeControl, 0, 0, std::max(1.0f, g_v.L(1)));
        d->AddRectFilled(g_v.P(x, std::min(top, y1 - len) + 1), g_v.P(x + 5, std::min(top + len, y1) - 1), kGold);
        return;
    }
    d->AddRectFilled(g_v.P(x, y0), g_v.P(x + 5, y1), IM_COL32(236, 228, 206, 31));
    d->AddRectFilled(g_v.P(x, std::min(top, y1 - len)), g_v.P(x + 5, std::min(top + len, y1)), IM_COL32(236, 228, 206, 153));
}

float PromptRow(ImDrawList* d, ImFont* f, float size, const PromptList& prompts, float right, float y, float glyph, bool stage, int* clicked)
{
    const float gap = glyph > 28 ? 26.0f : 22.0f;
    for (int i = static_cast<int>(prompts.size()) - 1; i >= 0; i--) {
        const float w = glyph + 8 + TextW(f, size, prompts[i].second);
        const float px = right - w;
        Glyph(d, prompts[i].first, g_v.P(px, y), g_v.P(px + glyph, y + glyph), IM_COL32_WHITE, stage);
        const float ty = y + (glyph - size * 1.25f) / 2;
        if (stage) StageText(d, f, size, px + glyph + 8, ty, kCream, prompts[i].second);
        else Text(d, f, g_v.L(size), g_v.P(px + glyph + 8, ty), kCream, prompts[i].second);
        if (Clicked() && MouseIn(g_v.P(px, y - 6), g_v.P(right, y + glyph + 6))) {
            ClickPad(PadKeyOfGlyph(prompts[i].first));
            if (clicked) *clicked = i;
        }
        right = px - gap;
    }
    return right + gap;
}

int StageTextFit(ImDrawList* d, ImFont* f, float size, float smallest, float x, float y, float room, ImU32 c, const std::string& t, int lines)
{
    if (t.empty() || room <= 0) return 0;
    float s = size;
    while (s > smallest && TextW(f, s, t) > room) s -= 1;
    if (TextW(f, s, t) <= room) {
        StageText(d, f, s, x, y, c, t);
        return 1;
    }
    // Two lines (by words) a little above and below, the second cut to fit.
    std::string first, rest = t;
    if (lines > 1) {
        size_t cut = std::string::npos;
        for (size_t at = t.find(' '); at != std::string::npos; at = t.find(' ', at + 1)) {
            if (TextW(f, s, t.substr(0, at)) > room) break;
            cut = at;
        }
        if (cut != std::string::npos) {
            first = t.substr(0, cut);
            rest = t.substr(cut + 1);
        }
    }
    auto cutToFit = [&](std::string v) {
        if (TextW(f, s, v) <= room) return v;
        while (v.size() > 1 && TextW(f, s, v + "...") > room) v.pop_back();
        return v + "...";
    };
    if (first.empty()) {
        StageText(d, f, s, x, y, c, cutToFit(rest));
        return 1;
    }
    const float step = s * 1.2f;
    StageText(d, f, s, x, y - step / 2, c, first);
    StageText(d, f, s, x, y + step / 2, c, cutToFit(rest));
    return 2;
}

void PromptLine(ImDrawList* d, ImFont* /*body*/, ImFont* bold, const std::string& lead, ImU32 leadCol, const std::string& text,
                const PromptList& prompts, int* clicked)
{
    // The prompts first (right to left), then the hint in the room left.
    *clicked = -1;
    const float left = PromptRow(d, bold, 20, prompts, 1564, 849, 36, true, clicked);
    float x = 48;
    if (!lead.empty()) {
        StageText(d, bold, 20, x, 855, leadCol, lead);
        x += TextW(bold, 20, lead) + 14;
    }
    // The hint in the prompts' own weight and colour (the light one read as fainter).
    StageTextFit(d, bold, 20, 17, x, 855, left - 36 - x, kCream, text);
}

float Modal(ImDrawList* d, ImFont* bold, ImFont* body, const ModalSpec& m, int* clicked)
{
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    d->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(4, 5, 7, 153));
    const float x1 = m.x0 + m.w, y1 = m.y0 + m.h;
    Pane(d, m.x0, m.y0, x1, y1, m.glass, m.danger ? kDanger : 0);
    Strip(d, m.x0 + 2, m.y0 + 2, x1 - 2, m.y0 + 44, false);
    Text(d, bold, g_v.L(16), g_v.P(m.x0 + 28, m.y0 + 12), m.danger ? kDanger : kGold, m.kicker);
    if (!m.aside.empty()) Text(d, body, g_v.L(17), g_v.P(x1 - 28, m.y0 + 11), m.asideCol, m.aside, 1);
    // The footer: the hint, then this modal's own prompts.
    Strip(d, m.x0 + 2, y1 - 48, x1 - 2, y1 - 2, false);
    if (clicked) *clicked = -1;
    const float left = PromptRow(d, bold, 17, m.prompts, x1 - 22, y1 - 39, 28, false, clicked);
    if (!m.hint.empty()) {
        // Cut to the room the prompts leave.
        std::string hint = m.hint;
        const float room = left - 24 - (m.x0 + 28);
        while (hint.size() > 1 && TextW(body, 17, hint) > room) hint.pop_back();
        if (hint.size() < m.hint.size()) hint += "...";
        Text(d, body, g_v.L(17), g_v.P(m.x0 + 28, y1 - 36), m.hintCol, hint);
    }
    return m.y0 + 44;
}

bool Button(ImDrawList* d, ImFont* bold, float x0, float y0, float x1, float y1, const std::string& label, bool focused, bool danger)
{
    Control(d, x0, y0, x1, y1, focused, danger ? kDanger : kGold);
    Text(d, bold, g_v.L(18), g_v.P((x0 + x1) / 2, (y0 + y1) / 2 - 12), focused ? kInk : kCream, label, 0.5f);
    return Clicked() && MouseIn(g_v.P(x0, y0), g_v.P(x1, y1));
}

} // namespace sfv::mod::ui
