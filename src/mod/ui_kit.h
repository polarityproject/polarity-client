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
void DrawPrompts(ImDrawList* d, ImFont* text, const std::vector<Prompt>& prompts);

} // namespace sfv::mod::ui
