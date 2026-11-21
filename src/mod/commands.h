#pragma once
// The command file's commands beyond pad steps and game-thread requests
// (control.h): each runs on the command thread with the rest of its line.
// docs/commands.md lists them.

#include <span>
#include <sstream>
#include <string>

namespace sfv::mod {

struct Command {
    const char* name;
    void (*run)(const std::string& name, std::stringstream& args);
};

// Sessions, rooms, screens, replays and the other test commands.
std::span<const Command> CommandsSession();
// Research on the game's objects, memory, tables and input.
std::span<const Command> CommandsObjects();
// Research on battles, rollback and replayed ticks.
std::span<const Command> CommandsBattle();

// The player name test sessions use: `name`'s, or "Player <instance + 1>".
std::string CommandsTestName();

} // namespace sfv::mod
