#pragma once
// The Battle Log (docs/battle-log-plan.md; mock-up canvas "SFVR Battle Log"):
// the player's online matches from SFVR\replays, newest first, as a timeline
// or grouped by set (LB/RB), with filters by character (both sides, LT) and
// by opponent (RT), the focused match's details, and A to watch it (the
// replay viewer, replay_driver.h). Render thread, inside an ImGui frame;
// the files are read on a worker.

#include "mod/hub_screen.h"

#include <cstdint>
#include <functional>
#include <string>

namespace sfv::mod {

struct BattleLogLive {
    std::string myName;  // the player's name (their side of each match)
    std::string buildId; // this game and mod's build: replays of another can't be played
    std::function<std::string(uint32_t packedCode)> charaName;
    std::function<std::string(uint16_t stage)> stageName;
    bool launching = false; // Watch pressed: the replay loads (the screen stays until the battle's loading)
};

struct BattleLogInput {
    bool back = false;
    std::string watch;      // a file to watch
    std::string watchTitle; // its title ("POLARITYDEV vs KANZUKI · game 2 of 4")
};

void OpenBattleLog();
void CloseBattleLog();
bool BattleLogOpen();
// The hub's line: "214 matches" (read in the background; "" until then).
std::string BattleLogInfo();
// Reads the folder again (new matches, files copied in).
void RefreshBattleLog();
void DrawBattleLog(const HubFonts& f, const BattleLogLive& live, bool takeInput, BattleLogInput* in);
// Over a watched replay (replay_driver.h ReplayViewNow): one stripe along
// the bottom, under the gauges, with its controls; takes the pad.
void DrawReplayStripe(const HubFonts& f, bool takeInput);

} // namespace sfv::mod
