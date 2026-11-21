#pragma once
// Test automation: the command file. Pad steps and waits run in order on a
// thread of their own; game-thread requests (savestates, watches, sync tests)
// are queued for the next battle tick; the other commands (commands.h) run
// on the command thread. docs/commands.md lists them all.

#include "core/game.h"

#include <cstdint>
#include <string>

namespace sfv::mod {

enum class GameCommand { None,
                         Save,
                         Load,
                         RingSave,
                         RingLoad,
                         Layout,
                         SyncTest,
                         Watch,
                         WatchAddr,
                         WatchReport,
                         Unwatch,
                         Stress,
                         LearnNoise };

struct GameRequest {
    GameCommand command = GameCommand::None;
    int arg0 = 0;
    int arg1 = 0;
    int arg2 = 0;
    uint64_t address = 0;
    bool flag = false;
};

// Reads commands from `commandFile` (sfv_cmd.txt next to the game; deleted
// once read) on a thread of its own. docs/commands.md lists them.
bool ControlInstall(const std::wstring& commandFile);
// Called by the battle tick hook: the next queued game-thread command.
GameRequest ControlTakeGameRequest();

} // namespace sfv::mod
