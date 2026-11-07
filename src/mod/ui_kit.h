#pragma once
// What the mod's full screens share (the select screen, the SFVR hub, the
// custom rooms screens): the game's textures read in the background, the
// 1600x900 design space fitted to the window, pad and keyboard input through
// ImGui, and the drawing helpers. Render thread only, inside an ImGui frame,
// unless said otherwise.

#include "mod/textures.h"

#include <imgui.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace sfv::mod::ui {

// ----- the game's textures ------------------------------------------------------
// A texture by its package path ("/Game/Chara/RYU/Hud/PartCharacter"), read
// from the paks on the mod's own threads (or loaded on the game thread when
// the paks cannot give it); null until it is there. Drawing asks every frame.
const TextureView* Texture(const std::string& path);
// The same texture shrunk (an area average, from the paks) near the size it is
// drawn at, `pixels` wide: the UI samples without mips (ImGui's DX11 sampler
// stops at the top level), so a big image drawn small skips pixels and looks
// grainy. The full size stands in until the small one has arrived.
const TextureView* TextureSized(const std::string& path, float pixels);
extern bool g_drawing; // false while asking ahead (not drawing): those load after what is drawn
std::string CharaTex(const std::string& code, const char* name); // "/Game/Chara/<code>/Hud/<name>"
std::string StageTex(const std::string& code, const char* name); // "/Game/Stage/<code>/UI/<name>"
std::string MenuTex(const char* name);                            // the character select's own pictures
std::string PadTex(const char* name);                             // "button_a" ... the pad glyphs

// ----- the design space ---------------------------------------------------------
// Design coordinates: 1600x900, fitted to the screen.
struct View {
    float s = 1, ox = 0, oy = 0;
    ImVec2 P(float x, float y) const { return ImVec2(ox + x * s, oy + y * s); }
    float L(float v) const { return v * s; }
};
extern View g_v;
void BeginScreen(); // fits g_v to this frame's display
// Every vertex drawn since `from` at `alpha` (a screen's fade).
void FadeFrom(ImDrawList* d, int from, float alpha);

// ----- input ----------------------------------------------------------------------
extern bool g_inputOn; // this frame reads input
// A prompt clicked last frame acts as its button this frame: A, B or Y.
extern bool g_clickConfirm, g_clickBack, g_clickSetting;
// Directions repeat while held; A and B count once per press.
bool Pressed(std::initializer_list<ImGuiKey> keys, bool repeat = true);
bool Left();
bool Right();
bool Up();
bool Down();
bool Confirm();     // A, Enter, Space
bool Back();        // B, Escape, Backspace
bool NextSetting(); // Y
bool Clicked();
bool MouseIn(ImVec2 a, ImVec2 b);
bool MouseInQuad(const ImVec2 (&q)[4]); // design coordinates

// ----- drawing --------------------------------------------------------------------
ImU32 Rgba(int r, int g, int b, int a = 255);
ImU32 FromArgb(uint32_t argb);
std::string Upper(std::string v);
float TextW(ImFont* f, float size, const std::string& t); // in design units
// `size` in pixels (g_v.L(...)); `align` 0 left, 0.5 centre, 1 right.
void Text(ImDrawList* d, ImFont* f, float size, ImVec2 at, ImU32 c, const std::string& t, float align = 0, bool shadow = false);
void GradientQuad(ImDrawList* d, const ImVec2 (&q)[4], ImU32 top, ImU32 bottom);
ImTextureID Id(const TextureView* t);
ImU32 FadeIn(const TextureView* t, ImU32 tint = IM_COL32_WHITE); // a texture fades in after arriving
void Image(ImDrawList* d, const TextureView* t, ImVec2 a, ImVec2 b, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1),
           ImU32 tint = IM_COL32_WHITE);
// An image whose alpha runs across it, left to right on the screen, through
// `stops` ({x 0..1, alpha 0..1}, in order, from 0 to 1): the fades the
// mock-ups draw with CSS masks. The stops span the image (a..b), or with
// fadeA < fadeB that stretch of the screen, so an image can move under a
// fade that stays put. `mirror` flips the image itself.
struct FadeStop {
    float x, alpha;
};
void ImageFadeX(ImDrawList* d, const TextureView* t, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, const FadeStop* stops, int count,
                bool mirror = false, ImU32 tint = IM_COL32_WHITE, float fadeA = 0, float fadeB = 0);
void Infinity(ImDrawList* d, ImVec2 c, float w, ImU32 col);
void Pill(ImDrawList* d, ImVec2 a, ImVec2 b);
void IconWired(ImDrawList* d, ImVec2 c, float s, ImU32 col);
void IconWifi(ImDrawList* d, ImVec2 c, float s, ImU32 col);
void IconLink(ImDrawList* d, ImVec2 c, float s, ImU32 col);
void IconCloud(ImDrawList* d, ImVec2 c, float s, ImU32 col);
void IconLinkType(ImDrawList* d, ImVec2 c, float s, int link, ImU32 col);

// The prompts at the top right: the game's pad icons with what they do here.
// A click acts as the button (on the next frame).
struct Prompt {
    const char* icon;
    std::string label;
    int action; // 0 confirm (A), 1 back (B), 3 Y
};
// `right` and `top` (design units) place the row elsewhere (a panel's own).
void DrawPrompts(ImDrawList* d, ImFont* text, const std::vector<Prompt>& prompts, float right = 1572, float top = 14);

// ----- the 1d look (the SFVR design system) -------------------------------------
// Glass panes over the game's stage art, one gold for the focus, square
// everything, light drop shadows. Positions are design units (1600 x 900).
namespace look {
constexpr ImU32 kGold = IM_COL32(233, 196, 106, 255);    // the focus; kickers, rules, actions as text
constexpr ImU32 kRing = IM_COL32(240, 194, 74, 255);     // the 2 px ring round a focused pane
constexpr ImU32 kCream = IM_COL32(244, 241, 232, 255);   // names, titles, values
constexpr ImU32 kSoft = IM_COL32(217, 212, 198, 255);    // body text, rules, labels
constexpr ImU32 kMuted = IM_COL32(168, 162, 147, 255);   // hints, footers
constexpr ImU32 kInk = IM_COL32(11, 11, 11, 255);        // text on gold
constexpr ImU32 kInkSoft = IM_COL32(58, 47, 18, 255);    // a second line on gold
constexpr ImU32 kOk = IM_COL32(127, 216, 139, 255);
constexpr ImU32 kWarn = IM_COL32(233, 196, 106, 255);
constexpr ImU32 kDanger = IM_COL32(255, 107, 94, 255);
constexpr ImU32 kInfo = IM_COL32(111, 195, 255, 255);
constexpr ImU32 kGround = IM_COL32(6, 8, 11, 255);
constexpr ImU32 kControl = IM_COL32(255, 255, 255, 15);      // unfocused buttons, rows, seats
constexpr ImU32 kEdgeControl = IM_COL32(236, 228, 206, 77);  // their 1 px frame
constexpr ImU32 kHairline = IM_COL32(236, 228, 206, 36);
constexpr ImU32 kSelfRow = IM_COL32(233, 196, 106, 41);
} // namespace look

// The stage behind every full screen (the Capcom Pro Tour arena), softened a
// little and dimmed flat. Over the whole display; `alpha` fades it.
void Backdrop(ImDrawList* d, float alpha = 1);
enum class Glass {
    Panel, // a screen's panes: 55% glass over the blurred stage
    Modal, // modals and toasts: 84%
    Fight, // over a fight (nothing to blur): 72% glass, no stage copy
};
// A glass pane: its light drop shadow, the stage's blurred copy inside it,
// the tint, the 2 px warm edge, the lit top line, a dark hairline outside.
// `edge` 0: the kind's own.
void Pane(ImDrawList* d, float x0, float y0, float x1, float y1, Glass kind = Glass::Panel, ImU32 edge = 0);
// Just the drop shadow under a box (for things that are not panes: notes).
void DropShadow(ImDrawList* d, float x0, float y0, float x1, float y1, float offset, float blur, float alpha, bool core);
// A dark title strip inside a pane, or a gold one when the pane has the focus.
void Strip(ImDrawList* d, float x0, float y0, float x1, float y1, bool lit);
// The 2 px gold ring round a focused pane (drawn just outside it).
void Ring(ImDrawList* d, float x0, float y0, float x1, float y1);
// A control (button, row, tab, key, seat): glass with a 1 px frame, or
// solid `focusFill` when it has the focus.
void Control(ImDrawList* d, float x0, float y0, float x1, float y1, bool focused, ImU32 focusFill = look::kGold);
// Text with nothing behind it: the light drop shadow under it. `size` and
// the position in design units.
void StageText(ImDrawList* d, ImFont* f, float size, float x, float y, ImU32 c, const std::string& t, float align = 0);
// A scroll bar: `first` and `inView` of `total` rows. On the stage (in the
// gutter between columns): a dark track and a gold thumb; in a pane: light.
void ScrollBar(ImDrawList* d, float x, float y0, float y1, float first, float inView, float total, bool stage);
// Pad prompts in a row ending at `right` (design units), glyph `glyph` high,
// labels at `size`; `stage`: with the text shadow. A click acts as the
// prompt: its index in *clicked (else unchanged).
using PromptList = std::vector<std::pair<const char*, std::string>>;
void PromptRow(ImDrawList* d, ImFont* f, float size, const PromptList& prompts, float right, float y, float glyph, bool stage, int* clicked);
// The screen's hint and prompts at the bottom, on the stage (no bar).
void PromptLine(ImDrawList* d, ImFont* body, ImFont* bold, const std::string& lead, ImU32 leadCol, const std::string& text,
                const PromptList& prompts, int* clicked);
// A modal: the screen dimmed, a glass pane with a header strip (a kicker in
// gold, or red when `danger`; `aside` on the right) and a footer strip (a
// hint, then the modal's own prompts). Returns the y where the body starts;
// *clicked: the footer prompt clicked.
struct ModalSpec {
    float x0 = 500, y0 = 300, w = 600, h = 300;
    std::string kicker, aside;
    ImU32 asideCol = look::kSoft;
    bool danger = false;
    std::string hint;
    ImU32 hintCol = look::kMuted;
    PromptList prompts;
};
float Modal(ImDrawList* d, ImFont* bold, ImFont* body, const ModalSpec& m, int* clicked);
// A modal's button: focused gold (or red when `danger`); true when clicked.
bool Button(ImDrawList* d, ImFont* bold, float x0, float y0, float x1, float y1, const std::string& label, bool focused, bool danger = false);

} // namespace sfv::mod::ui
