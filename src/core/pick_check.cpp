#include "core/pick_check.h"

#include <algorithm>

namespace sfv {

namespace {

// A character code as text for logs and messages (it came from another PC).
std::string Printable(const std::string& code) {
    std::string out = code;
    for (char& c : out) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) c = '?';
    }
    return out.empty() ? "(none)" : out;
}

} // namespace

std::string PickProblem(const PlayerSetup& pick, const PickTables& tables) {
    const std::string code = UnpackCode(pick.character);
    const int index = tables.charaIndex(code);
    if (index < 0 || !tables.isFighter(index)) return "no fighter " + Printable(code);
    const std::string costume = code + " costume " + std::to_string(pick.costume);
    if (pick.costume >= tables.costumeCount(index)) return "no " + costume;
    const bool eleven = tables.isEleven(pick);
    if (!eleven && tables.colorCount(index, pick.costume) <= 0) return costume + " is not offered";
    const std::vector<int> colors = eleven ? std::vector<int>{0, 1} : tables.colorIds(index, pick.costume);
    if (std::find(colors.begin(), colors.end(), pick.color) == colors.end()) return costume + " has no color id " + std::to_string(pick.color);
    const auto skills = [&](bool vtrigger) {
        const size_t n = tables.skillCount(code, vtrigger);
        return n ? n : 2;
    };
    if (pick.vskill >= skills(false)) return code + " has no V-Skill " + std::to_string(pick.vskill + 1);
    if (pick.vtrigger >= skills(true)) return code + " has no V-Trigger " + std::to_string(pick.vtrigger + 1);
    if (pick.comment > tables.commentCount(code)) return code + " has no V-Comment " + std::to_string(pick.comment);
    if ((pick.variation & tables.variants(index, pick.costume)) != pick.variation) return costume + " has no variant " + std::to_string(pick.variation);
    return "";
}

std::string SetupProblem(const MatchSetup& setup, const PickTables& tables) {
    for (int p = 0; p < kNumPlayers; p++) {
        if (const std::string why = PickProblem(setup.players[p], tables); !why.empty()) return "P" + std::to_string(p + 1) + ": " + why;
    }
    // A setup's stage is the one to play: never random.
    if (setup.stage == lobby::kRandomStage) return "no stage " + std::to_string(setup.stage);
    return StageRuleProblem(setup.stage, tables);
}

std::string StageRuleProblem(uint16_t stage, const PickTables& tables) {
    if (stage == lobby::kAnyStage || stage == lobby::kRandomStage || tables.isPlayableStage(stage)) return "";
    return "no stage " + std::to_string(stage);
}

std::string MessageProblem(const lobby::Message& m, const PickTables& tables) {
    // A seat's pick before it is made can be empty.
    const auto seat = [&](const lobby::Seat& s) { return s.hasPick || s.pick.character ? PickProblem(s.pick, tables) : ""; };
    switch (m.type) {
        case lobby::Type::PlayerState: return seat(m.seat);
        case lobby::Type::SessionState:
            for (int i = 0; i < kNumPlayers; i++) {
                if (const std::string why = seat(m.seats[i]); !why.empty()) return "P" + std::to_string(i + 1) + ": " + why;
            }
            return StageRuleProblem(m.stage, tables);
        case lobby::Type::Welcome:
        case lobby::Type::Loaded: return SetupProblem(m.setup, tables);
        case lobby::Type::Matches:
            for (const lobby::MatchEntry& e : m.matches) {
                if (const std::string why = SetupProblem(e.setup, tables); !why.empty()) return why;
            }
            return "";
        default: return "";
    }
}

} // namespace sfv
