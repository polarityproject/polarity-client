#pragma once
// Frame meter: 120 frames (2 s) of what each player's character is doing,
// one block a frame, over the V-Trigger gauges, with each move's startup,
// total and frame advantage (Advantage) above. In Training (on by default, Polarity
// settings) and in the Battle Log's replays (toggled from the stripe), never
// online. docs/frame-meter.md has the design and the research.
//
// Each battle tick (sampled as the next one starts) classifies both
// fighters from the game's own state (battle_actors.h): hit or block stun,
// an active hitbox or throwbox, a parry, invincibility, the move file's
// counter-hit state, recovery, interruptible recovery (IASA), neutral; and
// marks frames where the player's projectile is out. Hitstop, super
// freezes and cinematics are not counted (the meter holds). While both are
// neutral it holds too: an action within 120 frames draws the gap and goes
// on, after longer the next action clears the bar and starts at the left.
// Past 120 frames it wraps, the 10 blocks after the newest left clear.

#include <cstdint>

struct ImDrawList;
struct ImFont;

namespace sfv::mod {

class SfvGame;

bool FrameMeterInTraining();
void FrameMeterSetInTraining(bool on);
bool FrameMeterInReplays();
void FrameMeterSetInReplays(bool on);
// Research (`framemeter 0|1`): shown in any battle, whatever the choices.
void FrameMeterSetForced(bool on);

// Game thread, as each battle tick starts (the state the last one left).
void FrameMeterTick(SfvGame& game);
// Game thread, once per rendered battle frame (battle_driver.cpp): keeps the
// meter shown while no ticks run (a paused replay).
void FrameMeterFrame(SfvGame& game);
// Whether the overlay has a meter to draw now.
bool FrameMeterVisible();
// Render thread (overlay): the meter, in the 1600 x 900 design space (g_v).
void FrameMeterDraw(ImDrawList* d, ImFont* bold);
// Research (`fmlog <ticks>`): each tick's classification.
void FrameMeterLog(int ticks);

} // namespace sfv::mod
