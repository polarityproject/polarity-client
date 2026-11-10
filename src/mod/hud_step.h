#pragma once
// The battle HUD's Scaleform movies kept in step with a run of ticks.
//
// The banners ("ROUND 2", "FIGHT!", the KO's) and the round timer are
// Scaleform movies (ScaleformUtilGFxPlayer actors playing the SwfMovie
// assets Announce and CockpitTop) that the plugin's manager advances once a
// rendered frame by the real time passed (rva 0x17CA560: GFx::Movie::Advance
// through vtable +0xC0, only while the movie's wrapper plays, +0xA6). A
// replay viewer run simulates many ticks a rendered frame: a banner the
// ticks showed and closed within a frame or two never played out, and stayed
// on screen (over the next round's call, or the match's end). Advancing
// those movies by one tick's time after each extra tick of a rendered frame
// lets them see the ticks as played (2026-11-10, TestPC: one tick a
// rendered frame through a call was the only clean pace).

#include "mod/image.h"

#include <string>

namespace sfv::mod {

// Checks the plugin code the step relies on; false: StepHudMovies does nothing.
bool InstallHudStep(const GameImage& image);
// A new battle: finds its HUD movies (walks the object array: once a battle).
void FindHudMovies();
void ForgetHudMovies();
// Advances the HUD movies by `seconds` (game thread, between ticks).
void StepHudMovies(float seconds);
// "N HUD steps, T ms" since the last report (a jump's cost, in its log line).
std::string HudStepReport();
// On by default; `hudstep 0|1|2` (research; 2: the Announce movie only).
void SetHudStep(int mode);
bool HudStepOn();

} // namespace sfv::mod
