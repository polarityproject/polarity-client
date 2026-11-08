#pragma once
// The mod's random stage: the host draws it (netplay_driver ChooseSetup) and
// sends it in the match setup, so both games play the same stage.
//
// Random never lands on Flamenco Tavern (SX1), Skies of Honor (DBI) or
// Kanzuki Beach (KPB), nor on a setting of theirs (user, 2026-11-08). They can
// still be picked by name.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sfv {

inline constexpr std::string_view kNeverRandomStages[] = {"SX1", "DBI", "KPB"};

inline bool IsNeverRandomStage(std::string_view code)
{
    for (std::string_view s : kNeverRandomStages) {
        if (code == s) return true;
    }
    return false;
}

// A stage random may fall on: its stage table index, its code, and the code
// of the menu stage it is a setting of ("" for a menu stage itself).
struct RandomStageChoice {
    int index = -1;
    std::string code;
    std::string baseCode;
};

// The stage `roll` lands on among `stages` less the never-random ones, or -1
// when none is left.
inline int PickRandomStage(const std::vector<RandomStageChoice>& stages, uint64_t roll)
{
    std::vector<int> pool;
    for (const auto& s : stages) {
        if (!IsNeverRandomStage(s.code) && !IsNeverRandomStage(s.baseCode)) pool.push_back(s.index);
    }
    return pool.empty() ? -1 : pool[roll % pool.size()];
}

} // namespace sfv
