#pragma once
// Two players on the same look (docs/custom-rooms-plan.md, M2). In rooms
// both picks come from My character and no select screen runs first, so
// both may arrive as the same character, costume and color. The shared
// setup keeps both players' real picks; each PC shows the other player in
// another color, so each player always sees themselves as they picked:
//
//   - the same character, costume and color: the other player takes the
//     costume's next color in list order, wrapping (a one-color costume
//     stays as it is);
//   - both Eleven (who plays a fighter in its silver costume): the other one
//     wears the bronze palette.
//
// Spectators, replays and reports show what the host (P1, side 0) sees.
// Only colors change, and colors are presentation: the battle plays the
// same on every PC (checked on two instances, M0).

#include "core/game.h"

#include <functional>
#include <vector>

namespace sfv {

// What the rule needs from the game's data.
struct LookRules {
    // Whether a pick is Eleven (a fighter in its 0C costume).
    std::function<bool(const PlayerSetup&)> isEleven;
    uint8_t elevenOtherColor = 1; // the 0C costume's color 2 of 2 (bronze)
    // The pick's costume's colors in the order the select screen lists them
    // (EX colors after the rest); empty when not known.
    std::function<std::vector<int>(const PlayerSetup&)> colorsOf;
};

// The setup as `viewer` (0 or 1: P1 or P2; spectators, replays, reports: 0)
// sees it. Only the other player's color may change.
MatchSetup ShownSetup(const MatchSetup& setup, int viewer, const LookRules& rules);

// A battle `viewer` loaded, with the other player's color put back to the
// real pick wherever ShownSetup changed it: what the two sides loaded can
// then be compared as before.
MatchSetup RealPicks(const MatchSetup& loaded, const MatchSetup& setup, int viewer, const LookRules& rules);

} // namespace sfv
