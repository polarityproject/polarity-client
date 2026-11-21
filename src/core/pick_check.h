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
#include <string>
#include <vector>

namespace sfv {

// What the check needs from the game's tables (battle_setup.h
// GamePickTables).
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
    std::function<size_t(const std::string& code)> commentCount;
    std::function<uint8_t(int index, int costume)> variants; // PlayerSetup::variation bits
    std::function<bool(int stage)> isPlayableStage;
};

// Why the pick or setup is not one these tables have, or "" when it is.
std::string PickProblem(const PlayerSetup& pick, const PickTables& tables);
std::string SetupProblem(const MatchSetup& setup, const PickTables& tables);
// The host's stage rule: kAnyStage, kRandomStage or a playable stage.
std::string StageRuleProblem(uint16_t stage, const PickTables& tables);
// Every pick, setup and stage rule a lobby message carries (PlayerState,
// SessionState, Welcome, Loaded, Matches); "" for the others.
std::string MessageProblem(const lobby::Message& m, const PickTables& tables);

} // namespace sfv
