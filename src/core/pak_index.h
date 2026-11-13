#pragma once
// The file list of an Unreal Engine 4 .pak (the game's own are version 3,
// with a plain index; mods are packed the same way), and which of the game's
// files a path is: what a mod pak replaces tells whether it changes what a
// battle computes (move scripts, commands, the stage transitions' data) or
// only how it looks and sounds.

#include <cstdint>
#include <string>
#include <vector>

namespace sfv {

struct PakEntry {
    std::string path; // from the game's root ("StreetFighterV/Content/...")
    uint64_t size = 0, uncompressedSize = 0;
    uint32_t compression = 0;
    bool encrypted = false;
};

struct PakIndex {
    uint32_t version = 0;
    std::string mount;
    std::vector<PakEntry> entries;
};

// The index of the pak at `path` (UTF-8; versions 1 to 3). False with a reason when
// it can't be read (not a pak, a newer format, an encrypted index).
bool ReadPakIndex(const std::string& path, PakIndex* out, std::string* error);
// The same from the file's bytes: the whole file, or its tail from the index
// on (`fileSize` the whole file's size, `at` where `bytes` starts in it).
bool ParsePakIndex(const uint8_t* bytes, size_t size, uint64_t fileSize, uint64_t at, PakIndex* out, std::string* error);

// What a game file is to a battle.
enum class BattleFileKind {
    Other,       // models, textures, animation, sound, UI: how it looks and sounds
    MoveData,    // Chara/<C>/BattleResource/<v>/BAC_ (and _eff), BCM_, BCH_, DA_<C>_BattleResource
    StageData,   // Chara/<C>/DataAsset/StageData/DA_<C>_Stage_<S>: a stage transition KO's data
    CpuAi,       // Chara/<C>/BattleResource/<v>/BAI_: only the CPU uses it
};
struct BattleFile {
    BattleFileKind kind = BattleFileKind::Other;
    std::string character; // "KEN", "CMN" (shared by all)
    std::string version;   // "061" (move data only)
};
BattleFile ClassifyGamePath(const std::string& path);

// The mod's package-like key of a move data file ("KEN/061/BAC_KEN"), as a
// loaded object's package names it (/Game/Chara/KEN/BattleResource/061/BAC_KEN).
std::string BattleDataKey(const std::string& packagePath);

} // namespace sfv
