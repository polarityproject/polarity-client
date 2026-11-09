#pragma once
// The Battle Log's list (docs/battle-log-plan.md): the replays in the
// replays folder, read by their headers (LoadReplayHeader), one entry per
// match (both players' files of a match share its id), newest first, as a
// timeline or grouped by set, with filters by character and opponent.
// Game-agnostic: the mod's screen draws what this works out.

#include "core/replay_file.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sfv::battlelog {

struct Entry {
    std::string path;
    uint64_t size = 0;
    int64_t modified = 0; // file time, for the index cache
    ReplayFile header;    // META, setup and summary only
};

// The side of the player who keeps the log: the side whose name is `myName`
// (a match imported from the opponent has their side as its localPlayer),
// else the file's own side, else P1.
int MySide(const Entry& e, const std::string& myName);

// One entry per match id (0: each its own), keeping the player's own file
// (the one whose localPlayer is the side named myName), then the first;
// newest first (the setup's created time, then the path).
std::vector<Entry> Merge(std::vector<Entry> entries, const std::string& myName);

struct Filter {
    uint32_t mine = 0;   // the player's character (packed code); 0 any
    uint32_t theirs = 0; // the opponent's character; 0 any
    std::string opponent; // the opponent's name as shown (exact, ignoring case); empty any
};
bool Passes(const Entry& e, const Filter& f, const std::string& myName);

// A set: its games oldest first. A match with no set key is a set of its own.
struct Set {
    uint64_t key = 0;
    std::vector<const Entry*> games;
    std::string rule;          // the first game's
    uint8_t wins[kNumPlayers]; // as the last game left it (by side of that game's file)
    int64_t newest = 0;        // created time of the last game
};
// Sets newest first, from entries newest first (Merge).
std::vector<Set> GroupSets(const std::vector<const Entry*>& entries);

// For the filter lists: each character played (by the log's player, or by
// the opponents) with how many matches, most first; and each opponent name.
std::vector<std::pair<uint32_t, int>> Characters(const std::vector<Entry>& entries, const std::string& myName, bool theirs);
std::vector<std::pair<std::string, int>> Opponents(const std::vector<Entry>& entries, const std::string& myName);

// The name a side shows: the summary's, else "P1"/"P2".
std::string NameOf(const Entry& e, int side);

} // namespace sfv::battlelog
