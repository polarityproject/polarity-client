#pragma once
// The spectator's pause panel (custom-rooms-plan.md M5): Start while
// watching opens it over the match. It says what is watched (a table, or a
// Direct Match), which game of how many, how far behind the players, a chip
// per kept game, and steps through them: Previous, Current game (the newest),
// Next; B resumes, Y stops watching. It replaces the F1 panel's Previous /
// Next / Live for watching. Render thread, in an ImGui frame.

#include "mod/hub_screen.h"

#include <string>
#include <vector>

namespace sfv::mod {

struct WatchPanelGame {
    std::string p1, p2; // short character names
    int winner = -1;    // 0 P1, 1 P2, -1 playing or not known
    bool finished = false;
};

struct WatchPanelLive {
    int table = -1;                    // the watched table (0-based), -1 a Direct Match
    std::vector<WatchPanelGame> games; // the host's kept games, oldest first
    int at = -1;                       // the one being watched
    float behindSeconds = 0;           // frames in hand not yet played, as time
    std::string players[2];            // P1's and P2's names
};

struct WatchPanelInput {
    bool resume = false;
    bool stop = false;
    int watch = -2; // a game to watch from its start (an index into games), -1 the current game, -2 none
};

// `alpha` 0..1 (it fades in); with `takeInput` it reads the pad and keys.
void DrawWatchPanel(const HubFonts& fonts, const WatchPanelLive& live, float alpha, bool takeInput, WatchPanelInput* input);

} // namespace sfv::mod
