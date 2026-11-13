#pragma once
// Whether this install's battle data is the game's own: what can make a
// player fall out of sync with others no matter how good the netcode is.
//
// Two checks:
// - the mod paks (every .pak besides the game's own) and which of the game's
//   files they replace: move data (each character's move scripts, commands
//   and their resource list: Chara/<C>/BattleResource) and stage transition
//   data change what a battle computes; anything else (models, textures,
//   sounds, UI) only how it looks and sounds. Read from the paks' indexes at
//   start (core/pak_index.h).
// - the move data a battle has loaded (the KWBAC, KWBCM and KWBCH objects),
//   hashed and compared with the game's own (data/battle_data.txt, taken
//   from a stock install with `battledata dump`).
//
// Both go into the log, the desync reports and the benchmark's result.

#include <cstdint>
#include <string>
#include <vector>

namespace sfv::mod {

struct ModPak {
    std::string name;  // under Content\Paks ("~mods\x.pak")
    bool readable = false;
    std::string error; // why not
    size_t files = 0;
    std::vector<std::string> moveData;  // "JRI (061)": characters whose move data it replaces
    std::vector<std::string> stageData; // characters whose stage transition data it replaces
    bool cpuAi = false;                 // only the CPU's (not in a match between players)
    bool Battle() const { return !moveData.empty() || !stageData.empty(); }
};

struct ModScan {
    std::vector<ModPak> paks;
    bool BattleData() const;   // one or more change battle data (or can't be read)
    std::string Text() const;  // for logs and reports
    std::string Short() const; // for the benchmark's line ("2 mod paks, 1 changes battle data: JRI")
};
// Read once (the first call reads the paks' indexes: a few ms each).
const ModScan& ScanModPaks();

struct DataCheck {
    bool done = false;
    bool table = false;                // this build has the game's own hashes
    int files = 0, known = 0;          // move data files loaded; those the table has
    std::vector<std::string> changed;  // keys ("KEN/061/BAC_KEN") whose bytes are not the game's own
    std::vector<std::string> unknown;  // keys the table lacks (a newer game version, a mod's added file)
    uint64_t sum = 0;                  // of every file's hash: the same on two PCs with the same data
    bool Stock() const { return done && changed.empty(); }
    std::string Text() const;
    std::string Characters() const; // the changed files' characters ("KEN, CMN")
};

// The battle's loaded move data against the game's own (walks the game's
// objects: up to a second; off the game thread is fine).
DataCheck CheckLoadedBattleData();
// The same on a worker thread, logged ("battle data (<why>): ..."); the last
// result stays (LastDataCheck).
void CheckBattleDataSoon(const char* why);
DataCheck LastDataCheck();

// Research (`battledata dump`): the loaded move data's keys, sizes and hashes
// appended to Polarity\research\battle_data.txt in data/battle_data.txt's
// format; `battledata`: the check now, logged.
void DumpLoadedBattleData();

} // namespace sfv::mod
