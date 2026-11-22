// The replay viewer's stripe: its controls, Replay Takeover's bar and the round jumps' fade.

#include "mod/battle_log_screen.h"

#include "mod/frame_meter.h"
#include "mod/hitbox_display.h"
#include "mod/replay_driver.h"
#include "mod/ui_kit.h"
#include "mod/ui_sound.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sfv::mod {

using namespace ui;

namespace {

enum class JumpFade { None,
                      Out,
                      Black,
                      In };

struct JumpState {
    JumpFade phase = JumpFade::None;
    int dir = 0;
    double since = 0;
    uint32_t handledAt = 0;
};

JumpState g_jump;
constexpr double kFadeOut = 0.25, kFadeIn = 0.35, kJumpGiveUp = 30;

void RoundJumpFade(ImDrawList* d, const ReplayView& v) {
    const double now = ImGui::GetTime();
    float alpha = 0;
    switch (g_jump.phase) {
        case JumpFade::None: return;
        case JumpFade::Out:
            alpha = static_cast<float>((now - g_jump.since) / kFadeOut);
            if (alpha >= 1) {
                alpha = 1;
                g_jump.handledAt = g_jump.dir != 0 ? ReplayApplyControl(ReplayControl::Round, g_jump.dir) : v.handled;
                g_jump.phase = g_jump.handledAt || g_jump.dir == 0 ? JumpFade::Black : JumpFade::In;
                g_jump.since = now;
            }
            break;
        case JumpFade::Black:
            alpha = 1;
            if ((v.handled >= g_jump.handledAt && !v.seeking) || now - g_jump.since > kJumpGiveUp) {
                g_jump.phase = JumpFade::In;
                g_jump.since = now;
            }
            break;
        case JumpFade::In:
            alpha = 1 - static_cast<float>((now - g_jump.since) / kFadeIn);
            if (alpha <= 0) {
                g_jump = {};
                return;
            }
            break;
    }
    d->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Rgba(0, 0, 0, static_cast<int>(255 * std::clamp(alpha, 0.0f, 1.0f))));
}

} // namespace

void BattleLogDrawStripe(const HubFonts& f, bool takeInput) {
    const ReplayView v = ReplayViewNow();
    if (!v.active) {
        g_jump = {};
        return;
    }
    BeginScreen();
    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* bold = f.bold ? f.bold : f.strong;
    g_inputOn = takeInput;
    SetUiSoundSet(UiSoundSet::Menu);
    if (v.benchmark) {
        if (takeInput && Back()) ReplayApplyControl(ReplayControl::Leave);
        g_inputOn = false;
        const float y0 = 856, y1 = 900;
        d->AddRectFilled(g_v.P(0, y0), g_v.P(1600, y1), Rgba(10, 12, 16, 184));
        d->AddLine(g_v.P(0, y0), g_v.P(1600, y0), Rgba(236, 228, 206, 115), std::max(1.0f, g_v.L(1)));
        if (v.frames > 0) d->AddRectFilled(g_v.P(0, y0 - 1), g_v.P(1600.0f * (v.frame + 1) / v.frames, y0 + 2), look::kGold);
        Text(d, bold, g_v.L(16), g_v.P(48, y0 + 13), look::kGold, "BENCHMARK");
        const int left = std::max(0, (v.frames - v.frame) / 60);
        char line[96];
        std::snprintf(line, sizeof(line), "Please wait, and keep the game window in focus \xC2\xB7 0:%02d left", left);
        Text(d, bold, g_v.L(18), g_v.P(48 + TextW(bold, 16, "BENCHMARK") + 18, y0 + 11), look::kCream, line);
        int clicked = -1;
        PromptRow(d, f.body, 17, {{"button_b", "Stop"}}, 1564, y0 + 8, 28, false, &clicked);
        if (clicked >= 0) ReplayApplyControl(ReplayControl::Leave);
        return;
    }
    if (v.takeover >= 0) {
        if (takeInput && Pressed({ImGuiKey_GamepadStart}, false)) {
            PlayUi(UiSound::Cancel);
            ReplayApplyControl(ReplayControl::TakeoverStop);
        }
        g_inputOn = false;
        if (v.takeoverFading && g_jump.phase == JumpFade::None) g_jump = {JumpFade::Out, 0, ImGui::GetTime(), 0};
        RoundJumpFade(d, v);
        if (v.takeoverCountdown > 0) {
            const int n = std::clamp(static_cast<int>(std::ceil(v.takeoverCountdown)), 1, 3);
            StageText(d, f.display, 220, 800, 300, look::kCream, std::to_string(n), 0.5f);
            StageText(d, bold, 26, 800, 560, look::kGold, v.takeover == 0 ? "YOU ARE PLAYER 1" : "YOU ARE PLAYER 2", 0.5f);
        }
        const float y0 = 856, y1 = 900;
        d->AddRectFilled(g_v.P(0, y0), g_v.P(1600, y1), Rgba(10, 12, 16, 184));
        d->AddLine(g_v.P(0, y0), g_v.P(1600, y0), Rgba(236, 228, 206, 115), std::max(1.0f, g_v.L(1)));
        if (v.roundProgress > 0) d->AddRectFilled(g_v.P(0, y0 - 1), g_v.P(1600 * v.roundProgress, y0 + 2), look::kGold);
        const char* state = v.takeover == 0 ? "CONTROL P1" : "CONTROL P2";
        Text(d, bold, g_v.L(16), g_v.P(48, y0 + 13), look::kGold, state);
        int clicked = -1;
        const PromptList prompts = {{"button_start", v.takeoverEnding ? "Back to the start" : "Stop"}};
        const float left = PromptRow(d, f.body, 17, prompts, 1564, y0 + 8, 28, false, &clicked);
        const float textX = 48 + TextW(bold, 16, state) + 18;
        const std::string line = v.takeoverCountdown > 0 ? "Get ready" : v.takeoverEnding ? "Round over: back to where you took control"
                                                                                          : "Until the round's KO";
        Text(d, bold, g_v.L(18), g_v.P(textX, y0 + 11), look::kCream, Ellipsize(bold, 18, v.title + " \xC2\xB7 " + line, left - 24 - textX));
        if (clicked >= 0) ReplayApplyControl(ReplayControl::TakeoverStop);
        return;
    }
    if (v.opening && g_jump.phase == JumpFade::None) g_jump = {JumpFade::Black, 0, ImGui::GetTime(), v.handled};
    const auto jumpPressed = [&] {
        if (Pressed({ImGuiKey_GamepadL1}, false)) return -1;
        if (Pressed({ImGuiKey_GamepadR1}, false) && v.canRoundForward) return 1;
        return 0;
    };
    if (takeInput && g_jump.phase == JumpFade::In && !v.seeking) {
        if (const int dir = jumpPressed(); dir != 0) {
            PlayUi(UiSound::Step);
            const double shown = std::clamp(1 - (ImGui::GetTime() - g_jump.since) / kFadeIn, 0.0, 1.0);
            g_jump = {JumpFade::Out, dir, ImGui::GetTime() - shown * kFadeOut, 0};
        }
    }
    const bool fading = g_jump.phase != JumpFade::None;
    const bool canTakeOver = v.roundFrame >= 0 && !v.seeking && !fading;
    if (takeInput && fading && Back()) ReplayApplyControl(ReplayControl::Leave);
    if (takeInput && !fading) {
        if (canTakeOver) {
            const int side = Pressed({ImGuiKey_GamepadFaceLeft}, false) ? 0 : Pressed({ImGuiKey_GamepadFaceUp}, false) ? 1
                                                                                                                       : -1;
            if (side >= 0) {
                PlayUi(UiSound::Decide);
                ReplayApplyControl(ReplayControl::Takeover, side);
            }
        }
        if (Confirm()) ReplayApplyControl(ReplayControl::Toggle);
        if (Pressed({ImGuiKey_GamepadBack}, false)) {
            PlayUi(UiSound::Decide);
            HitboxesSetInReplays(!HitboxesInReplays());
        }
        if (Pressed({ImGuiKey_GamepadStart}, false)) {
            PlayUi(UiSound::Decide);
            FrameMeterSetInReplays(!FrameMeterInReplays());
        }
        if (Pressed({ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}) && v.frame > v.backLimit) ReplayApplyControl(ReplayControl::Step, -1);
        if (Pressed({ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}) && v.canStepForward) {
            PlayUi(UiSound::Cursor);
            ReplayApplyControl(ReplayControl::Step, 1);
        }
        if (g_jump.phase == JumpFade::None) {
            const int dir = jumpPressed();
            if (dir != 0) {
                PlayUi(UiSound::Step);
                g_jump = {JumpFade::Out, dir, ImGui::GetTime(), 0};
            }
        }
        static bool fast = false;
        const bool held = ImGui::IsKeyDown(ImGuiKey_GamepadR2);
        if (held != fast) {
            fast = held;
            ReplayApplyControl(ReplayControl::Fast, held ? 4 : 0);
        }
        if (Back()) ReplayApplyControl(ReplayControl::Leave);
    }
    g_inputOn = false;
    RoundJumpFade(d, v);
    const float y0 = 856, y1 = 900;
    d->AddRectFilled(g_v.P(0, y0), g_v.P(1600, y1), Rgba(10, 12, 16, 184));
    d->AddLine(g_v.P(0, y0), g_v.P(1600, y0), Rgba(236, 228, 206, 115), std::max(1.0f, g_v.L(1)));
    if (v.roundProgress > 0) d->AddRectFilled(g_v.P(0, y0 - 1), g_v.P(1600 * v.roundProgress, y0 + 2), look::kGold);
    const bool action = v.roundFrame >= 0 && !fading;
    const char* state = v.seeking ? "SEEKING" : v.paused           ? "PAUSED"
                                            : v.fast > 1 && action ? "FAST"
                                                                   : "PLAYING";
    Text(d, bold, g_v.L(16), g_v.P(48, y0 + 13), look::kGold, state);
    char frame[64] = "";
    if (action && v.roundFrames > 0) std::snprintf(frame, sizeof(frame), " \xC2\xB7 Frame %d / %d", v.roundFrame, v.roundFrames - 1);
    else if (action) std::snprintf(frame, sizeof(frame), " \xC2\xB7 Frame %d", v.roundFrame);
    PromptList prompts = {{"button_a", v.paused ? "Play" : "Pause"}, {"button_lb", "Round"}, {"button_rb", ""}, {"button_rt", "Hold to fast forward"}, {"button_back", HitboxesInReplays() ? "Hide hitboxes" : "Show hitboxes"}, {"button_start", FrameMeterInReplays() ? "Hide frame meter" : "Show frame meter"}, {"button_b", "Leave"}};
    if (!v.canRoundForward) prompts.erase(prompts.begin() + 2);
    if (canTakeOver) prompts.insert(prompts.begin() + 1, {{"button_x", "Control P1"}, {"button_y", "Control P2"}});
    int clicked = -1;
    const float left = PromptRow(d, f.body, 17, prompts, 1564, y0 + 8, 28, false, &clicked);
    const float tx = left - 120;
    const float textX = 48 + TextW(bold, 16, state) + 18;
    const float titleRoom = std::max(60.0f, tx - 24 - textX - TextW(bold, 18, frame));
    Text(d, bold, g_v.L(18), g_v.P(textX, y0 + 11), look::kCream, Ellipsize(bold, 18, v.title, titleRoom) + frame);
    const bool canBack = v.frame > v.backLimit && !v.seeking && !fading;
    d->AddTriangleFilled(g_v.P(tx, y0 + 22), g_v.P(tx + 12, y0 + 14), g_v.P(tx + 12, y0 + 30), canBack ? look::kCream : Rgba(236, 228, 206, 70));
    d->AddTriangleFilled(g_v.P(tx + 30, y0 + 14), g_v.P(tx + 42, y0 + 22), g_v.P(tx + 30, y0 + 30),
                         v.canStepForward && !fading ? look::kCream : Rgba(236, 228, 206, 70));
    Text(d, bold, g_v.L(17), g_v.P(tx + 52, y0 + 12), look::kCream, "Frame");
    if (clicked >= 0) ClickPad(PadKeyOfGlyph(prompts[clicked].first));
    if (v.outOfSync >= 0) {
        const std::string note = v.outOfSync > 0 ? "Out of sync with the recording from round " + std::to_string(v.outOfSync)
                                                 : "Out of sync with the recording";
        const float w = TextW(bold, 17, note) + 32, x = 800 - w / 2, top = 168;
        d->AddRectFilled(g_v.P(x, top), g_v.P(x + w, top + 34), Rgba(10, 12, 16, 200));
        d->AddRect(g_v.P(x, top), g_v.P(x + w, top + 34), Rgba(229, 67, 90, 200), 0, 0, std::max(1.0f, g_v.L(1.5f)));
        Text(d, bold, g_v.L(17), g_v.P(x + 16, top + 7), look::kCream, note);
    }
}

} // namespace sfv::mod
