#pragma once
// Hitbox display: the fighters' and their effects' boxes drawn over the
// battle, in Training (on by default, Polarity settings) and in the Battle Log's
// replays (toggled from the stripe). Never in online matches.
//
// The boxes are the game's own: each actor (the two fighters and 16 pooled
// effect slots) keeps the boxes active on its current frame in lists inside
// its 15808-byte snapshot region R (the move file's box: tick ranges, state
// conditions and hits already spent applied). Placed from the actor's
// position and side, in battle units (x right from the stage centre, y up
// from the floor; 1 unit = 100 UE units on the Y = 0 plane), then projected
// through the drawn camera's POV. docs/hitbox-display.md has the research.

#include <cstdint>
#include <vector>

struct ImDrawList;

namespace sfv::mod {

class SfvGame;

struct WorldBox {
    enum Kind : uint8_t { Hurt,
                          Throwable,
                          Hit,
                          Throw,
                          Proximity };

    float x, y, w, h;
    Kind kind;
};

// The saved choices (polarity.ini [hitboxes] training / replays).
bool HitboxesInTraining();
void SetHitboxesInTraining(bool on);
bool HitboxesInReplays();
void SetHitboxesInReplays(bool on);
// Research (`hitboxes 0|1`): shown in any battle, whatever the choices.
void SetHitboxesForced(bool on);

// Game thread, after each rendered battle frame (battle_driver.cpp): the
// boxes of the frame about to be drawn, when they are to be shown.
void CollectHitboxes(SfvGame& game);
// Whether the overlay has boxes to draw now (it renders a frame for them).
bool HitboxesVisible();
// Render thread (overlay): the boxes, on `d`, over a display of that size.
void DrawHitboxes(ImDrawList* d, float width, float height);

// Research (`boxlog <ticks>`): each collection's boxes per actor.
void LogHitboxes(int frames);
std::vector<WorldBox> HitboxSnapshot();

} // namespace sfv::mod
