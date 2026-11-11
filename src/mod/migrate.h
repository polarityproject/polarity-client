#pragma once
// The move from SFVR to Polarity (docs/polarity-migration-plan.md, 4.2). An
// SFVR install that updates gets Polarity's loader and polarity.dll from
// SFVR's updater; the first Polarity start moves the rest:
//   sfv_rollback[.N].ini, sfv_rollback.panel[.N].ini -> polarity...ini
//   SFVR\ (logs, matches, replays, crashes, fonts, ...) -> Polarity\ (merged)
//   logs\sfvr*.log -> logs\polarity*.log, SFVR-logs-*.zip -> Polarity-logs-*.zip
//   sfv_rollback.dll, its .old and .live copies, sfvr_fonts_license.txt: deleted
//   (only once polarity.dll is there and Polarity's loader runs, which sets
//   POLARITY_LOADER: SFVR's loader, left by an update that could not replace
//   it, loads sfv_rollback.dll and nothing else)
// Each step checks before it acts: a second run, or a run after one cut short,
// finishes what is left and repeats nothing. A step that fails is reported and
// skipped; the game starts either way. Where both names exist, nothing is
// overwritten: the SFVR one stays, and SFVR\ keeps a note saying what stayed.
//
// Runs first in the mod's start, before the log, the settings or the data
// folder are opened (it logs nothing itself: the caller logs `lines`).

#include <string>
#include <vector>

namespace sfv::mod {

struct MigrateResult {
    std::vector<std::string> lines; // what moved, stayed or failed, for the log
    bool moved = false;             // anything moved: tell the player, once
    bool waited = false;            // another game was migrating; this one waited for it
};

// dir: the game's Binaries\Win64 (the mod's folder).
MigrateResult MigrateFromSfvr(const std::wstring& dir);

// The [update] url SFVR builds wrote or shipped, cleared when an ini moves.
constexpr const wchar_t* kSfvrUpdateUrl = L"https://sfvr.pages.dev/update.json";

} // namespace sfv::mod
