#pragma once
// Whether a pick or match setup from another PC (the other player, the host
// a spectator watches, a recording) is one this game has. The game indexes
// its own tables with these values, so nothing from another PC reaches the
// battle data, the preloads or the select screen before this check. A pick
// that fails ends the session; nothing is clamped or replaced.

#include "core/game.h"
#include "core/lobby.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sfv {

// What the check needs from the game's tables (battle_setup.h
// BattleSetupPickTables).
struct PickTables {
    std::function<int(const std::string& code)> charaIndex; // -1: no such character
    std::function<bool(int index)> isFighter;
    std::function<int(int index)> costumeCount;
    // The colors the select screen offers for a costume (0: it does not
    // offer the costume), and their ids (EX colors from 100).
    std::function<int(int index, int costume)> colorCount;
    std::function<std::vector<int>(int index, int costume)> colorIds;
    std::function<bool(const PlayerSetup&)> isEleven; // the silver model: two colors, not offered
    // 0 when not known: two V-Skills, two V-Triggers.
    std::function<size_t(const std::string& code, bool vtrigger)> skillCount;
    // nullopt while the character's V-Comments are still loading.
    std::function<std::optional<size_t>(const std::string& code)> commentCount;
    // PlayerSetup::variation bits; nullopt while still being asked.
    std::function<std::optional<uint8_t>(int index, int costume)> variants;
    std::function<bool(int stage)> isPlayableStage;
};

// A check's outcome. NotYet: something the game finds out on demand (a
// character's V-Comments, a costume's variants) is not known yet; ask again
// shortly.
struct PickVerdict {
    enum class Kind { Fine,
                      Bad,
                      NotYet };
    Kind kind = Kind::Fine;
    std::string why; // Bad: what these tables do not have

    static PickVerdict Bad(std::string why) { return {Kind::Bad, std::move(why)}; }

    bool Fine() const { return kind == Kind::Fine; }
};

PickVerdict PickCheck(const PlayerSetup& pick, const PickTables& tables);
PickVerdict SetupCheck(const MatchSetup& setup, const PickTables& tables);
// The host's stage rule: kAnyStage, kRandomStage or a playable stage.
PickVerdict StageRuleCheck(uint16_t stage, const PickTables& tables);
// Every pick, setup and stage rule a lobby message carries (PlayerState,
// SessionState, Welcome, Loaded, Matches); Fine for the others.
PickVerdict MessageCheck(const lobby::Message& m, const PickTables& tables);

} // namespace sfv
