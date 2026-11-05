#include "mod/ui_kit.h"

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
bool g_clickConfirm = false, g_clickBack = false, g_clickSetting = false; // a prompt clicked last frame: A, B or Y this frame

// Directions repeat while held; A and B count once per press (a long
// frame, a character loading, would make a held A a repeat: the loadout's
// Ready right after the grid's Select).
bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat)
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
    const bool clicked = g_inputOn && std::exchange(g_clickConfirm, false);
    return clicked || Pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown}, false);
}
bool Back()
{
    const bool clicked = g_inputOn && std::exchange(g_clickBack, false);
    return clicked || Pressed({ImGuiKey_Escape, ImGuiKey_Backspace, ImGuiKey_GamepadFaceRight}, false);
}
// Y: the stage's next setting.
bool NextSetting()
{
    const bool clicked = g_inputOn && std::exchange(g_clickSetting, false);
    return clicked || Pressed({ImGuiKey_GamepadFaceUp}, false);
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

float TextW(ImFont* f, float size, const std::string& t) { return f->CalcTextSizeA(g_v.L(size), FLT_MAX, 0, t.c_str()).x / g_v.s; }

void Text(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 c, const std::string& t, float align, bool shadow)
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
        const float w = TextW(text, 17, p.label) + 50;
        const ImVec2 a = g_v.P(right - w, top), b = g_v.P(right, top + 42);
        d->AddRectFilled(a, b, Rgba(0, 0, 0, 150), g_v.L(21));
        if (const TextureView* icon = Texture(PadTex(p.icon))) Image(d, icon, g_v.P(right - w + 5, top + 5), g_v.P(right - w + 37, top + 37));
        Text(d, text, g_v.L(17), g_v.P(right - w + 42, top + 10), Rgba(244, 241, 232), p.label);
        if (Clicked() && MouseIn(a, b)) {
            if (p.action == 0) g_clickConfirm = true;
            else if (p.action == 1) g_clickBack = true;
            else if (p.action == 3) g_clickSetting = true;
        }
        right -= w + 18;
    }
}

} // namespace sfv::mod::ui
