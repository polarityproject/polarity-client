#include "core/look.h"

#include <algorithm>

namespace sfv {

MatchSetup ShownSetup(const MatchSetup& setup, int viewer, const LookRules& rules)
{
    MatchSetup shown = setup;
    const int other = viewer == 1 ? 0 : 1;
    const PlayerSetup& me = setup.players[1 - other];
    PlayerSetup& them = shown.players[other];
    if (rules.isEleven && rules.isEleven(me) && rules.isEleven(them)) {
        // Eleven's color is fixed (silver); two of them: the other in bronze.
        if (them.color == me.color) them.color = rules.elevenOtherColor != me.color ? rules.elevenOtherColor : 0;
        return shown;
    }
    if (me.character != them.character || me.costume != them.costume || me.color != them.color) return shown;
    const std::vector<int> colors = rules.colorsOf ? rules.colorsOf(them) : std::vector<int>{};
    const auto at = std::find(colors.begin(), colors.end(), them.color);
    if (colors.size() > 1 && at != colors.end()) them.color = static_cast<uint8_t>(colors[(at - colors.begin() + 1) % colors.size()]);
    return shown;
}

MatchSetup RealPicks(const MatchSetup& loaded, const MatchSetup& setup, int viewer, const LookRules& rules)
{
    MatchSetup real = loaded;
    const MatchSetup shown = ShownSetup(setup, viewer, rules);
    for (int p = 0; p < kNumPlayers; p++) {
        if (shown.players[p].color != setup.players[p].color && loaded.players[p].color == shown.players[p].color) {
            real.players[p].color = setup.players[p].color;
        }
    }
    return real;
}

} // namespace sfv
