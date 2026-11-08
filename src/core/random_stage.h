#pragma once
// The mod's random stage: the host draws it (netplay_driver ChooseSetup) and
// sends it in the match setup, so both games play the same stage.
//
// Random never lands on Flamenco Tavern (SX1), Skies of Honor (DBI) or
// Kanzuki Beach (KPB), nor on a setting of theirs (user, 2026-11-08). They can
// still be picked by name.
//
// Each player can also leave stages out of their own random (My character's
// Stage Select, "Random stages"; docs/random-stage-plan.md): a list of the
// variant codes left out, one code per stage or setting (TRN The Grid, S25
// The Grid Alternative). The host's list decides a session's random.

#include <algorithm>
#include <cctype>
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

// The stage `roll` lands on among `stages` less the never-random ones and
// those `off` leaves out (by their own code; a stage left out keeps its
// settings), or -1 when none is left. When `off` leaves nothing, every stage
// but the never-random ones is in again.
inline int PickRandomStage(const std::vector<RandomStageChoice>& stages, uint64_t roll, const std::vector<std::string>& off = {})
{
    std::vector<int> pool, all;
    for (const auto& s : stages) {
        if (IsNeverRandomStage(s.code) || IsNeverRandomStage(s.baseCode)) continue;
        all.push_back(s.index);
        if (std::find(off.begin(), off.end(), s.code) == off.end()) pool.push_back(s.index);
    }
    if (pool.empty()) pool = std::move(all);
    return pool.empty() ? -1 : pool[roll % pool.size()];
}

// A random list as the panel ini keeps it ("CHN,S25"): codes of three
// letters or digits, sorted, each once. Parse takes the same (spaces and
// lower case too) and drops anything else.
inline std::vector<std::string> ParseStageMask(std::string_view text)
{
    std::vector<std::string> codes;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(',', at);
        if (end == std::string_view::npos) end = text.size();
        std::string code;
        for (char ch : text.substr(at, end - at)) {
            if (!std::isspace(static_cast<unsigned char>(ch))) code += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        const bool valid = code.size() == 3 && std::all_of(code.begin(), code.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0; });
        if (valid) codes.push_back(std::move(code));
        at = end + 1;
    }
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    return codes;
}

inline std::string FormatStageMask(const std::vector<std::string>& off)
{
    std::string joined;
    for (const auto& code : off) joined += code + ",";
    std::string text;
    for (const auto& code : ParseStageMask(joined)) text += (text.empty() ? "" : ",") + code;
    return text;
}

} // namespace sfv
